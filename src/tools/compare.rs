use std::fs;
use std::path::Path;

use eframe::egui;

use crate::jobs::{DiffKind, DiffRow, Job, JobKind, JobState, Outcome};

const MAX_FILE_BYTES: usize = 8 * 1024 * 1024;
const MAX_ROWS: usize = 20_000;
const CONTEXT: usize = 3;

/// Two-file text comparison with a side-by-side colored diff
/// (left column = left file, right column = right file).
#[derive(Default)]
pub struct CompareTool {
    pub left: String,
    pub right: String,
    pub running: bool,
    pub error: Option<String>,
    pub result: Option<CompareResult>,
}

#[derive(Clone, Debug, Default)]
pub struct CompareResult {
    pub rows: Vec<DiffRow>,
    pub added: usize,
    pub removed: usize,
    pub same: usize,
    pub truncated: bool,
    pub left: String,
    pub right: String,
}

impl CompareTool {
    pub fn ui(&mut self, ui: &mut egui::Ui, jobs: &mut Vec<Job>) {
        ui.add_space(4.0);
        ui.label("Compare two text files line by line.");
        ui.add_space(4.0);

        // Horizontal rows (not a Grid): TextEdit widths inside Grid cells get
        // clamped to the cell's sizing-pass width and collapse. The fields
        // here stretch with the toolbox window instead.
        ui.horizontal(|ui| {
            ui.label("Left file:");
            ui.add(
                egui::TextEdit::singleline(&mut self.left)
                    .hint_text("Path to file...")
                    .desired_width((ui.available_width() - 210.0).max(200.0)),
            );
            if ui.button("Browse...").clicked() {
                if let Some(f) = rfd::FileDialog::new().pick_file() {
                    self.left = f.display().to_string();
                }
            }
        });
        ui.horizontal(|ui| {
            ui.label("Right file:");
            ui.add(
                egui::TextEdit::singleline(&mut self.right)
                    .hint_text("Path to file...")
                    .desired_width((ui.available_width() - 210.0).max(200.0)),
            );
            if ui.button("Browse...").clicked() {
                if let Some(f) = rfd::FileDialog::new().pick_file() {
                    self.right = f.display().to_string();
                }
            }
        });
        ui.add_space(6.0);

        if self.running && ui.button("Cancel").clicked() {
            if let Some(j) = jobs.iter_mut().rev().find(|j| j.kind == JobKind::Compare) {
                j.cancel();
            }
        } else if ui.button("Compare").clicked() {
            self.start(jobs);
        }

        if let Some(state) = crate::jobs::progress_snapshot(jobs, JobKind::Compare) {
            if !state.finished {
                ui.horizontal(|ui| {
                    ui.spinner();
                    ui.label(state.current);
                });
            }
        }

        if let Some(e) = &self.error {
            ui.colored_label(ui.visuals().error_fg_color, format!("Error: {}", e));
        }

        if let Some(res) = &self.result {
            ui.separator();
            if res.added == 0 && res.removed == 0 {
                ui.label(format!("Files are identical ({} lines).", res.same));
                return;
            }
            let added = added_color(ui);
            let removed = removed_color(ui);
            ui.label(format!(
                "{} is {} lines and {} is {} lines",
                file_name_of(&res.left),
                res.removed + res.same,
                file_name_of(&res.right),
                res.added + res.same,
            ));
            ui.label(format!(
                "added: {}, removed: {}{}",
                res.added,
                res.removed,
                if res.truncated { " (output truncated)" } else { "" }
            ));
            ui.add_space(4.0);

            egui::ScrollArea::both()
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    egui::Grid::new("diff_grid")
                        .num_columns(4)
                        .spacing([10.0, 2.0])
                        .show(ui, |ui| {
                            for row in &res.rows {
                                match &row.kind {
                                    DiffKind::Gap => {
                                        let n = row.left.as_deref().unwrap_or("");
                                        ui.label(egui::RichText::new("").small());
                                        ui.label(
                                            egui::RichText::new(format!(
                                                "⋯ {} unchanged lines ⋯",
                                                n
                                            ))
                                            .weak()
                                            .small(),
                                        );
                                        ui.label(egui::RichText::new("").small());
                                        ui.label(egui::RichText::new("").small());
                                    }
                                    DiffKind::Same => {
                                        ui.label(egui::RichText::new(num(row.lno)).weak().small());
                                        ui.monospace(
                                            egui::RichText::new(text(row.left.as_ref()))
                                                .color(ui.visuals().weak_text_color()),
                                        );
                                        ui.label(egui::RichText::new(num(row.rno)).weak().small());
                                        ui.monospace(
                                            egui::RichText::new(text(row.right.as_ref()))
                                                .color(ui.visuals().weak_text_color()),
                                        );
                                    }
                                    DiffKind::Removed => {
                                        ui.label(egui::RichText::new(num(row.lno)).weak().small());
                                        ui.monospace(
                                            egui::RichText::new(text(row.left.as_ref())).color(removed),
                                        );
                                        ui.label(egui::RichText::new("").small());
                                        ui.monospace(egui::RichText::new(""));
                                    }
                                    DiffKind::Added => {
                                        ui.label(egui::RichText::new("").small());
                                        ui.monospace(egui::RichText::new(""));
                                        ui.label(egui::RichText::new(num(row.rno)).weak().small());
                                        ui.monospace(
                                            egui::RichText::new(text(row.right.as_ref())).color(added),
                                        );
                                    }
                                    DiffKind::Replaced => {
                                        ui.label(egui::RichText::new(num(row.lno)).weak().small());
                                        ui.monospace(
                                            egui::RichText::new(text(row.left.as_ref())).color(removed),
                                        );
                                        ui.label(egui::RichText::new(num(row.rno)).weak().small());
                                        ui.monospace(
                                            egui::RichText::new(text(row.right.as_ref())).color(added),
                                        );
                                    }
                                }
                                ui.end_row();
                            }
                        });
                });
        }
    }

    fn start(&mut self, jobs: &mut Vec<Job>) {
        let left = self.left.trim().to_string();
        let right = self.right.trim().to_string();
        self.error = None;
        self.result = None;
        self.running = true;
        jobs.push(Job::spawn(JobKind::Compare, "Compare".to_string(), move |state, cancel| {
            match compare_files(&left, &right, state, cancel) {
                Ok(res) => {
                    let mut s = state.lock().unwrap();
                    s.finished = true;
                    s.outcome = Some(Outcome::Compare {
                        rows: res.rows,
                        added: res.added,
                        removed: res.removed,
                        same: res.same,
                        truncated: res.truncated,
                    });
                }
                Err(e) => {
                    let mut s = state.lock().unwrap();
                    s.finished = true;
                    s.error = Some(e);
                }
            }
        }));
    }

    /// Called by the app when the compare job has finished.
    pub fn on_done(&mut self, state: JobState) {
        self.running = false;
        if let Some(Outcome::Compare { rows, added, removed, same, truncated }) = state.outcome {
            self.result = Some(CompareResult {
                rows,
                added,
                removed,
                same,
                truncated,
                left: self.left.trim().to_string(),
                right: self.right.trim().to_string(),
            });
            self.error = None;
        } else if let Some(e) = state.error {
            self.error = Some(e);
        }
    }
}

#[derive(Clone, Debug, Default)]
struct WorkResult {
    rows: Vec<DiffRow>,
    added: usize,
    removed: usize,
    same: usize,
    truncated: bool,
}

fn read_text(path: &str, state: &std::sync::Mutex<JobState>) -> Result<String, String> {
    if path.is_empty() {
        return Err("both files must be given".to_string());
    }
    let bytes = fs::read(path).map_err(|e| format!("'{}': {}", path, e))?;
    if bytes.len() > MAX_FILE_BYTES {
        return Err(format!("'{}' is larger than 8 MB", path));
    }
    if bytes.contains(&0) {
        return Err(format!("'{}' looks binary and cannot be compared as text", path));
    }
    if let Ok(mut s) = state.lock() {
        s.current = format!("read {}", path);
    }
    Ok(String::from_utf8_lossy(&bytes).into_owned())
}

fn compare_files(
    left: &str,
    right: &str,
    state: &std::sync::Mutex<JobState>,
    cancel: &std::sync::atomic::AtomicBool,
) -> Result<WorkResult, String> {
    let a = read_text(left, state)?;
    let b = read_text(right, state)?;
    if let Ok(mut s) = state.lock() {
        s.current = "diffing...".to_string();
    }

    // Identical content: the diff would yield no groups at all, so report
    // it directly instead of iterating.
    if a == b {
        return Ok(WorkResult {
            same: a.lines().count(),
            ..Default::default()
        });
    }

    let old_lines: Vec<&str> = a.lines().collect();
    let new_lines: Vec<&str> = b.lines().collect();
    let diff = similar::TextDiff::configure().diff_lines(&a, &b);
    let mut res = WorkResult::default();
    let mut lno: u32 = 1;
    let mut rno: u32 = 1;
    let mut prev_old_end: usize = 0;
    let mut rows_full = false;

    for group in diff.grouped_ops(CONTEXT) {
        // Skipped unchanged block before this group (not before the first).
        let skipped = group
            .first()
            .map(|op| op.old_range().start)
            .unwrap_or(0)
            .saturating_sub(prev_old_end);
        if skipped > 0 && !res.rows.is_empty() && !rows_full {
            res.rows.push(DiffRow {
                kind: DiffKind::Gap,
                lno: None,
                left: Some(format!("{}", skipped)),
                rno: None,
                right: None,
            });
        }
        for op in group.iter() {
            if cancel.load(std::sync::atomic::Ordering::Relaxed) {
                res.truncated = true;
                return Ok(res);
            }
            let old = op.old_range();
            let new = op.new_range();
            match op.tag() {
                similar::DiffTag::Equal => {
                    for i in 0..old.len() {
                        push_row(
                            &mut res,
                            &mut rows_full,
                            &mut lno,
                            &mut rno,
                            DiffKind::Same,
                            Some(old_lines[old.start + i].to_string()),
                            Some(new_lines[new.start + i].to_string()),
                        );
                    }
                }
                similar::DiffTag::Delete => {
                    for i in 0..old.len() {
                        push_row(
                            &mut res,
                            &mut rows_full,
                            &mut lno,
                            &mut rno,
                            DiffKind::Removed,
                            Some(old_lines[old.start + i].to_string()),
                            None,
                        );
                    }
                }
                similar::DiffTag::Insert => {
                    for i in 0..new.len() {
                        push_row(
                            &mut res,
                            &mut rows_full,
                            &mut lno,
                            &mut rno,
                            DiffKind::Added,
                            None,
                            Some(new_lines[new.start + i].to_string()),
                        );
                    }
                }
                similar::DiffTag::Replace => {
                    let n = old.len().max(new.len());
                    for i in 0..n {
                        let l = old_lines.get(old.start + i).map(|s| s.to_string());
                        let r = new_lines.get(new.start + i).map(|s| s.to_string());
                        let kind = match (&l, &r) {
                            (Some(_), Some(_)) => DiffKind::Replaced,
                            (Some(_), None) => DiffKind::Removed,
                            (None, Some(_)) => DiffKind::Added,
                            (None, None) => continue,
                        };
                        push_row(&mut res, &mut rows_full, &mut lno, &mut rno, kind, l, r);
                    }
                }
            }
        }
        prev_old_end = group
            .last()
            .map(|op| op.old_range().end)
            .unwrap_or(prev_old_end);
    }

    Ok(res)
}

/// Append one aligned row, tracking line numbers, per-kind counts and the
/// global row cap (once full, further rows are dropped but changes counted).
fn push_row(
    res: &mut WorkResult,
    rows_full: &mut bool,
    lno: &mut u32,
    rno: &mut u32,
    kind: DiffKind,
    left: Option<String>,
    right: Option<String>,
) {
    let (mut l, mut r) = (None, None);
    match kind {
        DiffKind::Same | DiffKind::Replaced => {
            l = Some(*lno);
            r = Some(*rno);
        }
        DiffKind::Removed => l = Some(*lno),
        DiffKind::Added => r = Some(*rno),
        DiffKind::Gap => {}
    }
    if left.is_some() {
        *lno += 1;
    }
    if right.is_some() {
        *rno += 1;
    }
    match kind {
        DiffKind::Same => res.same += 1,
        DiffKind::Removed => res.removed += 1,
        DiffKind::Added => res.added += 1,
        DiffKind::Replaced => {
            res.removed += 1;
            res.added += 1;
        }
        DiffKind::Gap => {}
    }
    if !*rows_full {
        if res.rows.len() >= MAX_ROWS {
            *rows_full = true;
            res.truncated = true;
        } else {
            res.rows.push(DiffRow {
                kind,
                lno: l,
                left,
                rno: r,
                right,
            });
        }
    }
}

/// Theme-aware colors for diff lines.
fn added_color(ui: &egui::Ui) -> egui::Color32 {
    if ui.visuals().dark_mode {
        egui::Color32::from_rgb(120, 200, 140)
    } else {
        egui::Color32::from_rgb(0, 130, 80)
    }
}

fn removed_color(ui: &egui::Ui) -> egui::Color32 {
    if ui.visuals().dark_mode {
        egui::Color32::from_rgb(240, 110, 110)
    } else {
        egui::Color32::from_rgb(180, 30, 30)
    }
}

fn num(n: Option<u32>) -> String {
    match n {
        Some(n) => n.to_string(),
        None => String::new(),
    }
}

fn text(s: Option<&String>) -> String {
    s.cloned().unwrap_or_default()
}

fn file_name_of(path: &str) -> String {
    Path::new(path)
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| path.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicBool;

    #[test]
    fn identical_files_have_no_changes() {
        let tmp = std::env::temp_dir().join("rc-cmp-same");
        fs::create_dir_all(&tmp).unwrap();
        let a = tmp.join("a.txt");
        let b = tmp.join("b.txt");
        fs::write(&a, "one\ntwo\n").unwrap();
        fs::write(&b, "one\ntwo\n").unwrap();
        let cancel = AtomicBool::new(false);
        let st = std::sync::Mutex::new(JobState::default());
        let res = compare_files(a.to_str().unwrap(), b.to_str().unwrap(), &st, &cancel).unwrap();
        assert_eq!((res.added, res.removed, res.same), (0, 0, 2));
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn changed_files_align_side_by_side() {
        let tmp = std::env::temp_dir().join("rc-cmp-diff");
        fs::create_dir_all(&tmp).unwrap();
        let a = tmp.join("a.txt");
        let b = tmp.join("b.txt");
        fs::write(&a, "one\ntwo\nthree\n").unwrap();
        fs::write(&b, "one\nTWO\nthree\nfour\n").unwrap();
        let cancel = AtomicBool::new(false);
        let st = std::sync::Mutex::new(JobState::default());
        let res = compare_files(a.to_str().unwrap(), b.to_str().unwrap(), &st, &cancel).unwrap();
        assert_eq!(res.removed, 1); // "two" (replaced pair counts as removed)
        assert_eq!(res.added, 2); // "TWO", "four"
        assert_eq!(res.same, 2); // "one", "three"
        // Side-by-side alignment: the replaced pair shares a row.
        let two = res
            .rows
            .iter()
            .find(|r| r.left.as_deref() == Some("two"))
            .unwrap();
        assert_eq!(two.kind, DiffKind::Replaced);
        assert_eq!(two.right.as_deref(), Some("TWO"));
        let four = res
            .rows
            .iter()
            .find(|r| r.right.as_deref() == Some("four"))
            .unwrap();
        assert_eq!(four.kind, DiffKind::Added);
        assert_eq!(four.lno, None);
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn rejects_missing_and_binary() {
        let cancel = AtomicBool::new(false);
        let st = std::sync::Mutex::new(JobState::default());
        assert!(compare_files("", "", &st, &cancel).is_err());
        let bin = std::env::temp_dir().join("rc-cmp-bin");
        fs::write(&bin, [0u8, 1, 2]).unwrap();
        assert!(compare_files(bin.to_str().unwrap(), bin.to_str().unwrap(), &st, &cancel).is_err());
        let _ = fs::remove_file(&bin);
    }
}
