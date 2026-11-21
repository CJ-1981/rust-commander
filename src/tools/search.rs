use std::collections::VecDeque;
use std::path::PathBuf;
use std::sync::atomic::Ordering;

use eframe::egui;

use crate::jobs::{progress_snapshot, Job, JobKind, JobState, Outcome, SearchHit};
use crate::panel::fmt_size;

const MAX_HITS: usize = 5000;
const MAX_SCANNED: u64 = 200_000;

/// Recursive file search by (case-insensitive) name substring.
#[derive(Default)]
pub struct SearchTool {
    pub root: String,
    pub pattern: String,
    pub hits: Vec<SearchHit>,
    pub scanned: u64,
    pub truncated: bool,
    pub running: bool,
    pub error: Option<String>,
}

impl SearchTool {
    pub fn ui(&mut self, ui: &mut egui::Ui, jobs: &mut Vec<Job>) {
        ui.add_space(4.0);
        ui.label("Find files and folders by name (case-insensitive, recursive).");
        ui.add_space(4.0);

        ui.horizontal(|ui| {
            ui.label("Folder:");
            ui.add(
                egui::TextEdit::singleline(&mut self.root)
                    .hint_text("Start folder...")
                    .desired_width(ui.available_width() - 260.0),
            );
            if ui.button("Browse...").clicked() {
                if let Some(d) = rfd::FileDialog::new().pick_folder() {
                    self.root = d.display().to_string();
                }
            }
        });

        let mut start = false;
        ui.horizontal(|ui| {
            ui.label("Name contains:");
            let resp = ui.add(
                egui::TextEdit::singleline(&mut self.pattern)
                    .hint_text("e.g. report")
                    .desired_width(240.0),
            );
            start = ui.button("Search").clicked()
                || (resp.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)));
            if self.running {
                if ui.button("Cancel").clicked() {
                    if let Some(j) = jobs.iter_mut().rev().find(|j| j.kind == JobKind::Search) {
                        j.cancel();
                    }
                }
            }
        });
        if start {
            self.start(jobs);
        }

        if let Some(state) = progress_snapshot(jobs, JobKind::Search) {
            if !state.finished {
                ui.horizontal(|ui| {
                    ui.spinner();
                    ui.label(format!("{} ({} matches so far)", state.current, state.done));
                });
            }
        }

        if let Some(e) = &self.error {
            ui.colored_label(egui::Color32::from_rgb(255, 120, 120), format!("Error: {}", e));
        }

        if !self.hits.is_empty() || self.scanned > 0 {
            ui.separator();
            ui.label(format!(
                "{} result(s){} after scanning {} entr{}",
                self.hits.len(),
                if self.truncated { " (truncated)" } else { "" },
                self.scanned,
                if self.scanned == 1 { "y" } else { "ies" }
            ));
            ui.add_space(4.0);
            egui::ScrollArea::vertical()
                .max_height(280.0)
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    for h in &self.hits {
                        ui.horizontal(|ui| {
                            let label = if h.is_dir {
                                format!("{}/", h.name)
                            } else {
                                h.name.clone()
                            };
                            ui.monospace(label);
                            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                                ui.small(fmt_size(h.size));
                            });
                        });
                        ui.label(
                            egui::RichText::new(h.path.display().to_string())
                                .weak()
                                .small(),
                        );
                        ui.add_space(2.0);
                    }
                });
        }
    }

    /// Called by the app when the search job has finished.
    pub fn on_done(&mut self, state: JobState) {
        self.running = false;
        if let Some(Outcome::Search { hits, scanned, truncated }) = state.outcome {
            self.hits = hits;
            self.scanned = scanned;
            self.truncated = truncated;
            self.error = None;
        } else if let Some(e) = state.error {
            self.error = Some(e);
        }
    }

    fn start(&mut self, jobs: &mut Vec<Job>) {
        let root = PathBuf::from(self.root.trim());
        if !root.is_dir() {
            self.error = Some("Start folder does not exist".to_string());
            return;
        }
        let pat = self.pattern.trim().to_lowercase();
        self.error = None;
        self.running = true;
        self.hits.clear();
        self.scanned = 0;
        jobs.push(Job::spawn(JobKind::Search, "Search".to_string(), move |state, cancel| {
            let mut hits: Vec<SearchHit> = Vec::new();
            let mut truncated = false;
            let mut scanned = 0u64;
            let mut queue = VecDeque::new();
            queue.push_back(root);

            'outer: while let Some(dir) = queue.pop_front() {
                if cancel.load(Ordering::Relaxed) {
                    state.lock().unwrap().error = Some("cancelled".to_string());
                    break;
                }
                let rd = match std::fs::read_dir(&dir) {
                    Ok(rd) => rd,
                    Err(_) => continue,
                };
                for e in rd.flatten() {
                    scanned += 1;
                    if scanned > MAX_SCANNED {
                        truncated = true;
                        break 'outer;
                    }
                    let path = e.path();
                    let name = e.file_name().to_string_lossy().into_owned();
                    let ft = match e.file_type() {
                        Ok(t) => t,
                        Err(_) => continue,
                    };
                    // Do not follow symlinked directories (loop protection).
                    if ft.is_dir() && !ft.is_symlink() {
                        queue.push_back(path.clone());
                    }
                    if !pat.is_empty() && name.to_lowercase().contains(&pat) {
                        let size = e
                            .metadata()
                            .map(|m| if m.is_dir() { 0 } else { m.len() })
                            .unwrap_or(0);
                        hits.push(SearchHit {
                            path: path.clone(),
                            name: name.clone(),
                            is_dir: ft.is_dir(),
                            size,
                        });
                        if hits.len() >= MAX_HITS {
                            truncated = true;
                            break 'outer;
                        }
                    }
                    if scanned % 200 == 0 {
                        if let Ok(mut s) = state.lock() {
                            s.done = hits.len() as u64;
                            s.current = format!("scanned {} entries", scanned);
                        }
                    }
                }
            }

            let mut s = state.lock().unwrap();
            s.done = hits.len() as u64;
            s.finished = true;
            s.outcome = Some(Outcome::Search {
                hits,
                scanned,
                truncated,
            });
        }));
    }
}
