use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use eframe::egui;
use md5::Digest;
use sha2::Sha256;

use crate::jobs::{progress_snapshot, Job, JobKind, JobState, Outcome};

const CHUNK: usize = 1 << 20; // 1 MiB

/// Theme-aware green for "match" indicators.
fn added_like_color(ui: &egui::Ui) -> egui::Color32 {
    if ui.visuals().dark_mode {
        egui::Color32::from_rgb(120, 200, 140)
    } else {
        egui::Color32::from_rgb(0, 130, 80)
    }
}

/// MD5 + SHA-256 checksum calculator.
#[derive(Default)]
pub struct HashTool {
    pub path: String,
    pub md5: Option<String>,
    pub sha256: Option<String>,
    pub expected: String,
    pub busy: bool,
    pub error: Option<String>,
}

impl HashTool {
    pub fn ui(&mut self, ui: &mut egui::Ui, jobs: &mut Vec<Job>) {
        ui.add_space(4.0);
        ui.label("Compute MD5 and SHA-256 checksums of a file.");
        ui.add_space(4.0);

        let mut start = false;
        ui.horizontal(|ui| {
            ui.label("File:");
            let resp = ui.add(
                egui::TextEdit::singleline(&mut self.path)
                    .hint_text("Path to file...")
                    .desired_width(ui.available_width() - 110.0),
            );
            if ui.button("Browse...").clicked() {
                if let Some(f) = rfd::FileDialog::new().pick_file() {
                    self.path = f.display().to_string();
                    self.error = None;
                }
            }
            start = ui.button("Compute").clicked()
                || (resp.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)));
        });
        if start {
            self.start(jobs);
        }

        if let Some(state) = progress_snapshot(jobs, JobKind::Hash) {
            if !state.finished && state.total > 0 {
                let frac = (state.done as f32 / state.total as f32).clamp(0.0, 1.0);
                ui.add(
                    egui::ProgressBar::new(frac)
                        .text(format!("{}%", (frac * 100.0) as u64)),
                );
            }
        }

        if self.busy {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label("Hashing...");
                if ui.button("Cancel").clicked() {
                    if let Some(j) = jobs.iter_mut().rev().find(|j| j.kind == JobKind::Hash) {
                        j.cancel();
                    }
                }
            });
        }

        if let Some(e) = &self.error {
            ui.colored_label(ui.visuals().error_fg_color, format!("Error: {}", e));
        }
        if let Some(m) = &self.md5 {
            ui.horizontal(|ui| {
                ui.strong("MD5:");
                ui.monospace(m);
                if ui.small_button("Copy").clicked() {
                    ui.ctx().copy_text(m.clone());
                }
            });
        }
        if let Some(s) = &self.sha256 {
            ui.horizontal(|ui| {
                ui.strong("SHA-256:");
                ui.monospace(s);
                if ui.small_button("Copy").clicked() {
                    ui.ctx().copy_text(s.clone());
                }
            });
        }

        // Verify against an expected checksum (either algorithm matches).
        if let (Some(md5), Some(sha256)) = (&self.md5, &self.sha256) {
            ui.add_space(6.0);
            ui.horizontal(|ui| {
                ui.label("Expected checksum:");
                ui.add(
                    egui::TextEdit::singleline(&mut self.expected)
                        .hint_text("paste MD5 or SHA-256 to verify")
                        .desired_width(ui.available_width() - 24.0),
                );
            });
            let want = self.expected.trim().to_lowercase();
            if !want.is_empty() {
                if want == md5.to_lowercase() {
                    ui.colored_label(added_like_color(ui), "MATCH (MD5)");
                } else if want == sha256.to_lowercase() {
                    ui.colored_label(added_like_color(ui), "MATCH (SHA-256)");
                } else {
                    ui.colored_label(ui.visuals().error_fg_color, "MISMATCH");
                }
            }
        }
    }

    /// Called by the app when the hash job has finished.
    pub fn on_done(&mut self, state: JobState) {
        self.busy = false;
        if let Some(Outcome::Hash { path, md5, sha256 }) = state.outcome {
            self.path = path;
            self.md5 = md5;
            self.sha256 = sha256;
            self.error = None;
        } else if let Some(e) = state.error {
            self.error = Some(e);
        }
    }

    fn start(&mut self, jobs: &mut Vec<Job>) {
        let path = PathBuf::from(self.path.trim());
        if !path.is_file() {
            self.error = Some("File not found (or not a regular file)".to_string());
            return;
        }
        self.error = None;
        self.md5 = None;
        self.sha256 = None;
        self.busy = true;
        jobs.push(Job::spawn(JobKind::Hash, "Hash".to_string(), move |state, cancel| {
            match compute(&path, state, cancel) {
                Ok((m, sh)) => {
                    let mut s = state.lock().unwrap();
                    s.finished = true;
                    s.outcome = Some(Outcome::Hash {
                        path: path.display().to_string(),
                        md5: Some(m),
                        sha256: Some(sh),
                    });
                }
                Err(e) => {
                    let mut s = state.lock().unwrap();
                    s.finished = true;
                    s.error = Some(e.to_string());
                }
            }
        }));
    }
}

/// Stream the file in chunks through both hashers at once.
fn compute(
    path: &Path,
    state: &Mutex<JobState>,
    cancel: &AtomicBool,
) -> std::io::Result<(String, String)> {
    let total = std::fs::metadata(path)?.len();
    let mut file = std::fs::File::open(path)?;
    let mut md5h = md5::Md5::new();
    let mut sha = Sha256::new();
    let mut buf = vec![0u8; CHUNK];

    {
        let mut s = state.lock().unwrap();
        s.total = total;
        s.current = path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
    }

    let mut read_total = 0u64;
    loop {
        if cancel.load(Ordering::Relaxed) {
            return Err(std::io::Error::other("cancelled"));
        }
        let n = file.read(&mut buf)?;
        if n == 0 {
            break;
        }
        md5h.update(&buf[..n]);
        sha.update(&buf[..n]);
        read_total += n as u64;
        if let Ok(mut s) = state.lock() {
            s.done = read_total;
        }
    }

    Ok((format!("{:x}", md5h.finalize()), format!("{:x}", sha.finalize())))
}
