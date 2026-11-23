use std::fs;
use std::path::Path;

use eframe::egui;

use crate::jobs::{DiffKind, DiffRow, Job, JobKind, JobState, Outcome};

const MAX_FILE_BYTES: usize = 8 * 1024 * 1024;
const MAX_ROWS: usize = 20_000;

/// Side-by-side (left/right) text comparison with a unified colored diff.
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

        egui::Grid::new("compare_inputs").num_columns(3).spacing([12.0, 6.0]).show(ui, |ui| {
            ui.label("Left file:");
            ui.add(
                egui::TextEdit::singleline(&mut self.left)
                    .hint_text("Path to file...")
                    .desired_width(380.0),
            );
            if ui.button("Browse...").clicked() {
                if let Some(f) = rfd::FileDialog::new().pick_file() {
                    self.left = f.display().to_string();
                }
            }
            ui.end_row();
            ui.label("Right file:");
            ui.add(
                egui::TextEdit::singleline(&mut self.right)
                    .hint_text("Path to file...")
                    .desired_width(380.0),
            );
            if ui.button("Browse...").clicked() {
                if let Some(f) = rfd::FileDialog::new().pick_file() {
                    self.right = f.display().to_string();
                }
            }
            ui.end_row();
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
                ui.label(format!(
                    "Files are identical ({} lines).",
                    res.same
                ));
                return;
            }
            ui.label(format!(
                "{} is {:>6} lines and {} is {:>6} lines",
                file_name_of(&res.left),
                res.removed + res.same,
                file_name_of(&res.right),
                res.added + res.same,
            ));
            ui.label(format!(
                "added: {}, removed: {}, identical lines: {}{}",
                res.added,
                res.removed,
                res.same,
                if res.truncated { " (output truncated)" } else { "" }
            ));
            ui.add_space(4.0);
            let added = added_color(ui);
            egui::ScrollArea::vertical()
                .max_height(320.0)
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    for row in &res.rows {
                        let (sign, color) = match row.kind {
                            DiffKind::Same => (" ", ui.visuals().weak_text_color()),
                            DiffKind::Removed => ("−", ui.visuals().error_fg_color),
                            DiffKind::Added => ("+", added),
                        };
                        ui.monospace(
                            egui::RichText::new(format!("{} {}", sign, row.text)).color(color),
                        );
                    }
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

    // Identical content: grouped_ops would yield no groups at all, so report
    // it directly instead of iterating.
    if a == b {
        return Ok(WorkResult {
            same: a.lines().count(),
            ..Default::default()
        });
    }

    let diff = similar::TextDiff::from_lines(&a, &b);
    let mut res = WorkResult::default();
    for group in diff.grouped_ops(3) {
        for op in group.iter() {
            for change in diff.iter_changes(op) {
                if cancel.load(std::sync::atomic::Ordering::Relaxed) {
                    res.truncated = true;
                    return Ok(res);
                }
                let text = change.value().trim_end_matches('\n').to_string();
                let kind = match change.tag() {
                    similar::ChangeTag::Equal => {
                        res.same += 1;
                        DiffKind::Same
                    }
                    similar::ChangeTag::Delete => {
                        res.removed += 1;
                        DiffKind::Removed
                    }
                    similar::ChangeTag::Insert => {
                        res.added += 1;
                        DiffKind::Added
                    }
                };
                if res.rows.len() < MAX_ROWS {
                    res.rows.push(DiffRow { kind, text });
                } else {
                    res.truncated = true;
                }
            }
        }
    }
    Ok(res)
}

/// Theme-aware green for added diff lines.
fn added_color(ui: &egui::Ui) -> egui::Color32 {
    if ui.visuals().dark_mode {
        egui::Color32::from_rgb(120, 200, 140)
    } else {
        egui::Color32::from_rgb(0, 130, 80)
    }
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
    fn changed_files_count_added_and_removed() {
        let tmp = std::env::temp_dir().join("rc-cmp-diff");
        fs::create_dir_all(&tmp).unwrap();
        let a = tmp.join("a.txt");
        let b = tmp.join("b.txt");
        fs::write(&a, "one\ntwo\nthree\n").unwrap();
        fs::write(&b, "one\nTWO\nthree\nfour\n").unwrap();
        let cancel = AtomicBool::new(false);
        let st = std::sync::Mutex::new(JobState::default());
        let res = compare_files(a.to_str().unwrap(), b.to_str().unwrap(), &st, &cancel).unwrap();
        assert_eq!(res.removed, 1); // "two"
        assert_eq!(res.added, 2); // "TWO", "four"
        assert_eq!(res.same, 2); // "one", "three"
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
