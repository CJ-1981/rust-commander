use std::fs;
use std::path::{Path, PathBuf};

use eframe::egui;

/// Built-in text viewer / editor (F3 view, F4 edit).
pub struct Viewer {
    pub path: PathBuf,
    pub name: String,
    pub text: String,
    pub editing: bool,
    pub dirty: bool,
    pub error: Option<String>,
    pub binary: bool,
}

impl Viewer {
    /// Load a file for viewing. Text is read lossily; binary files are detected
    /// via NUL bytes and only reported, not displayed.
    pub fn open(path: &Path, editing: bool) -> Self {
        let (text, error, binary) = match fs::read(path) {
            Ok(bytes) if bytes.len() > 8 * 1024 * 1024 => (
                String::new(),
                Some("File is larger than 8 MB and cannot be shown in the built-in viewer."
                    .to_string()),
                false,
            ),
            Ok(bytes) if bytes.contains(&0) => (
                String::new(),
                Some(
                    "Binary file: cannot be displayed as text. Use 'Open external' instead."
                        .to_string(),
                ),
                true,
            ),
            Ok(bytes) => (String::from_utf8_lossy(&bytes).into_owned(), None, false),
            Err(e) => (String::new(), Some(e.to_string()), false),
        };
        Self {
            path: path.to_path_buf(),
            name: path
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default(),
            text,
            editing,
            dirty: false,
            error,
            binary,
        }
    }

    pub fn save(&mut self) {
        match fs::write(&self.path, self.text.as_bytes()) {
            Ok(()) => {
                self.dirty = false;
                self.error = None;
            }
            Err(e) => self.error = Some(format!("Save failed: {}", e)),
        }
    }

    pub fn reload(&mut self) {
        *self = Viewer::open(&self.path, self.editing);
    }

    pub fn ui(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            if ui.selectable_label(self.editing, "Edit").clicked() {
                self.editing = !self.editing;
            }
            if ui.button("Save").clicked() {
                self.save();
            }
            if ui.button("Reload").clicked() {
                self.reload();
            }
            if ui.button("Open external").clicked() {
                let _ = open::that(&self.path);
            }
            ui.separator();
            ui.label(
                egui::RichText::new(self.path.display().to_string())
                    .weak()
                    .small(),
            );
        });
        ui.separator();

        if let Some(e) = &self.error {
            ui.colored_label(ui.visuals().error_fg_color, e);
            ui.add_space(4.0);
        }

        if !self.binary {
            egui::ScrollArea::vertical().show(ui, |ui| {
                let te = egui::TextEdit::multiline(&mut self.text)
                    .font(egui::TextStyle::Monospace)
                    .desired_width(ui.available_width())
                    .desired_rows(22)
                    .interactive(self.editing);
                let resp = ui.add(te);
                if resp.changed() {
                    self.dirty = true;
                }
            });
        }
    }
}
