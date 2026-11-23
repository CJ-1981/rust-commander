use std::collections::{HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;

use eframe::egui;
use md5::Digest;

use crate::jobs::{progress_snapshot, DupGroup, Job, JobKind, JobState, Outcome};
use crate::panel::fmt_size;

const MAX_FILES: u64 = 100_000;
const MAX_HASH_FILE_SIZE: u64 = 256 * 1024 * 1024;
const CHUNK: usize = 1 << 20; // 1 MiB

/// Content-based duplicate file finder (same size -> same MD5).
#[derive(Default)]
pub struct DupTool {
    pub root: String,
    pub groups: Vec<DupGroup>,
    pub scanned: u64,
    pub truncated: bool,
    pub running: bool,
    pub error: Option<String>,
}

impl DupTool {
    pub fn ui(&mut self, ui: &mut egui::Ui, jobs: &mut Vec<Job>, nav: &mut Option<PathBuf>) {
        ui.add_space(4.0);
        ui.label("Find duplicate files (identical content) inside a folder tree.");
        ui.add_space(4.0);

        ui.horizontal(|ui| {
            ui.label("Folder:");
            ui.add(
                egui::TextEdit::singleline(&mut self.root)
                    .hint_text("Start folder...")
                    .desired_width(ui.available_width() - 250.0),
            );
            if ui.button("Browse...").clicked() {
                if let Some(d) = rfd::FileDialog::new().pick_folder() {
                    self.root = crate::panel::nfc(&d.display().to_string());
                }
            }
        });

        ui.horizontal(|ui| {
            if ui.button("Scan for duplicates").clicked() {
                self.start(jobs);
            }
            if self.running && ui.button("Cancel").clicked() {
                if let Some(j) = jobs.iter_mut().rev().find(|j| j.kind == JobKind::Dup) {
                    j.cancel();
                }
            }
        });

        if let Some(state) = progress_snapshot(jobs, JobKind::Dup) {
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

        if !self.groups.is_empty() {
            ui.separator();
            let wasted: u64 = self
                .groups
                .iter()
                .map(|g| g.size * (g.files.len() as u64 - 1))
                .sum();
            ui.label(format!(
                "{} duplicate group(s), {} wasted{}",
                self.groups.len(),
                fmt_size(wasted),
                if self.truncated { " (scan truncated)" } else { "" }
            ));
            ui.add_space(4.0);
            egui::ScrollArea::vertical()
                .max_height(320.0)
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    for (i, g) in self.groups.iter().enumerate() {
                        ui.strong(format!(
                            "#{}  {} copies of {}  [md5 {}...]  ({} wasted)",
                            i + 1,
                            g.files.len(),
                            fmt_size(g.size),
                            g.hash.chars().take(8).collect::<String>(),
                            fmt_size(g.size * (g.files.len() as u64 - 1))
                        ));
                        for f in &g.files {
                            let r = ui.selectable_label(
                                false,
                                egui::RichText::new(crate::panel::nfc(&f.display().to_string()))
                                    .small()
                                    .weak(),
                            );
                            if r.double_clicked() {
                                *nav = Some(f.clone());
                            }
                        }
                        ui.add_space(6.0);
                    }
                });
        } else if !self.running && self.scanned > 0 {
            ui.separator();
            ui.label(format!(
                "No duplicates found (scanned {} files{})",
                self.scanned,
                if self.truncated { ", truncated" } else { "" }
            ));
        }
    }

    /// Called by the app when the duplicate scan has finished.
    pub fn on_done(&mut self, state: JobState) {
        self.running = false;
        if let Some(Outcome::Dup { groups, scanned, truncated }) = state.outcome {
            self.groups = groups;
            self.scanned = scanned;
            self.truncated = truncated;
            if state.error.is_none() {
                self.error = None;
            }
        } else if let Some(e) = state.error {
            self.error = Some(e);
        }
    }

    fn start(&mut self, jobs: &mut Vec<Job>) {
        let root = crate::panel::resolve_typed_path(self.root.trim());
        if !root.is_dir() {
            self.error = Some("Start folder does not exist".to_string());
            return;
        }
        self.error = None;
        self.groups.clear();
        self.scanned = 0;
        self.running = true;
        jobs.push(Job::spawn(JobKind::Dup, "Duplicates".to_string(), move |state, cancel| {
            // Pass 1: index files by size.
            let mut by_size: HashMap<u64, Vec<PathBuf>> = HashMap::new();
            let mut queue = VecDeque::new();
            queue.push_back(root);
            let mut scanned = 0u64;
            let mut truncated = false;

            'walk: while let Some(dir) = queue.pop_front() {
                if cancel.load(Ordering::Relaxed) {
                    state.lock().unwrap().error = Some("cancelled".to_string());
                    break;
                }
                let rd = match std::fs::read_dir(&dir) {
                    Ok(r) => r,
                    Err(_) => continue,
                };
                for e in rd.flatten() {
                    scanned += 1;
                    if scanned > MAX_FILES {
                        truncated = true;
                        break 'walk;
                    }
                    let ft = match e.file_type() {
                        Ok(t) => t,
                        Err(_) => continue,
                    };
                    if ft.is_dir() && !ft.is_symlink() {
                        queue.push_back(e.path());
                    } else if ft.is_file() {
                        if let Ok(meta) = e.metadata() {
                            let size = meta.len();
                            if size > 0 {
                                by_size.entry(size).or_default().push(e.path());
                            }
                        }
                    }
                }
                if let Ok(mut s) = state.lock() {
                    s.current = format!("indexed {} files", scanned);
                }
            }

            // Pass 2: hash candidates that share a size.
            let mut groups: Vec<DupGroup> = Vec::new();
            let mut sizes: Vec<u64> = by_size.keys().copied().collect();
            sizes.sort_unstable_by(|a, b| b.cmp(a));
            let mut hashed = 0u64;

            'hash: for size in sizes {
                let files = &by_size[&size];
                if files.len() < 2 || size > MAX_HASH_FILE_SIZE {
                    continue;
                }
                let mut by_hash: HashMap<String, Vec<PathBuf>> = HashMap::new();
                for f in files {
                    if cancel.load(Ordering::Relaxed) {
                        state.lock().unwrap().error = Some("cancelled".to_string());
                        break 'hash;
                    }
                    if let Ok(h) = hash_file(f) {
                        by_hash.entry(h).or_default().push(f.clone());
                    }
                    hashed += 1;
                    if let Ok(mut s) = state.lock() {
                        s.done = hashed;
                        s.current = format!("hashed {} candidate file(s)", hashed);
                    }
                }
                for (h, fs) in by_hash {
                    if fs.len() > 1 {
                        groups.push(DupGroup {
                            size,
                            hash: h,
                            files: fs,
                        });
                    }
                }
            }

            groups.sort_by_key(|g| std::cmp::Reverse(g.size));
            let mut s = state.lock().unwrap();
            s.finished = true;
            s.outcome = Some(Outcome::Dup {
                groups,
                scanned,
                truncated,
            });
        }));
    }
}

fn hash_file(path: &Path) -> std::io::Result<String> {
    use std::io::Read;
    let mut f = std::fs::File::open(path)?;
    let mut h = md5::Md5::new();
    let mut buf = vec![0u8; CHUNK];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(format!("{:x}", h.finalize()))
}
