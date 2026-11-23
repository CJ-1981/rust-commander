use std::path::{Path, PathBuf};

use eframe::egui;

/// Batch renamer for the active panel's selection, Total Commander style:
/// the name and extension masks may contain literals, `*` (keep the original
/// part) and `[N]` (a counter that increases by the configured step per file).
/// A live preview flags conflicting targets before anything is touched.
#[derive(Default)]
pub struct RenameTool {
    pub files: Vec<PathBuf>,
    pub name_mask: String,
    pub ext_mask: String,
    pub counter_start: i32,
    pub counter_step: i32,
    pub last_result: Option<Result<usize, String>>,
}

/// One preview row: source path and the name it would receive.
struct Plan {
    path: PathBuf,
    new_name: String,
    /// Why the rename cannot be applied (empty = fine).
    problem: String,
}

impl RenameTool {
    pub fn ui(&mut self, ui: &mut egui::Ui, changed: &mut bool) {
        ui.add_space(4.0);
        ui.label("Rename many files at once with masks: `*` keeps the original part, `[N]` inserts a counter.");
        ui.add_space(4.0);

        if self.files.is_empty() {
            ui.label(
                egui::RichText::new(
                    "No files loaded. Select files in a panel, then open Tools > Multi-file rename.",
                )
                .weak()
                .italics(),
            );
            return;
        }
        ui.label(format!("{} file(s) loaded from the active panel.", self.files.len()));
        ui.add_space(4.0);

        egui::Grid::new("rename_masks").num_columns(2).spacing([12.0, 6.0]).show(ui, |ui| {
            ui.label("Name mask:");
            ui.add(
                egui::TextEdit::singleline(&mut self.name_mask)
                    .hint_text("*  e.g. holiday_[N]")
                    .desired_width(240.0),
            );
            ui.end_row();
            ui.label("Extension mask:");
            ui.add(
                egui::TextEdit::singleline(&mut self.ext_mask)
                    .hint_text("*  e.g. jpg, or empty to drop it")
                    .desired_width(240.0),
            );
            ui.end_row();
            ui.label("Counter start:");
            ui.add(egui::DragValue::new(&mut self.counter_start).range(0..=1_000_000));
            ui.end_row();
            ui.label("Counter step:");
            ui.add(egui::DragValue::new(&mut self.counter_step).range(1..=1000));
            ui.end_row();
        });
        ui.add_space(6.0);

        // Live preview.
        let plans = self.plan();
        let blocked = plans.iter().any(|p| !p.problem.is_empty());
        let renamable = plans.iter().filter(|p| p.problem.is_empty() && p.new_name != file_name(&p.path)).count();
        ui.label(format!(
            "Preview: {} would be renamed{}",
            renamable,
            if blocked { ", some rows have problems:" } else { ":" }
        ));
        egui::ScrollArea::vertical()
            .max_height(260.0)
            .auto_shrink([false, false])
            .show(ui, |ui| {
                for p in &plans {
                    let old = crate::panel::nfc(&file_name(&p.path));
                    if p.problem.is_empty() {
                        ui.horizontal(|ui| {
                            ui.monospace(old);
                            ui.label("->");
                            ui.monospace(egui::RichText::new(&p.new_name).strong());
                        });
                    } else {
                        ui.monospace(
                            egui::RichText::new(format!("{}  ({})", old, p.problem))
                                .color(ui.visuals().error_fg_color),
                        );
                    }
                }
            });
        ui.add_space(6.0);

        if let Some(res) = &self.last_result {
            match res {
                Ok(n) => ui.label(format!("Renamed {} file(s).", n)),
                Err(e) => ui.colored_label(ui.visuals().error_fg_color, format!("Error: {}", e)),
            };
            ui.add_space(2.0);
        }

        ui.add_enabled_ui(!blocked && renamable > 0, |ui| {
            if ui.button("Rename").clicked() {
                self.last_result = Some(self.apply());
                *changed = true;
            }
        });
    }

    /// Compute the target name for every loaded file, flagging conflicts.
    fn plan(&self) -> Vec<Plan> {
        let mut counter = self.counter_start;
        let mut seen: Vec<String> = Vec::new();
        self.files
            .iter()
            .map(|path| {
                let old = file_name(path);
                let new = expand_mask(
                    &self.name_mask,
                    &self.ext_mask,
                    &old,
                    counter,
                );
                counter += self.counter_step;

                let mut problem = String::new();
                if new.is_empty() {
                    problem = "empty name".to_string();
                } else if new.contains('/') || new.contains('\\') || new == ".." {
                    problem = "illegal characters".to_string();
                } else if seen.iter().any(|s| s.eq_ignore_ascii_case(&new)) {
                    problem = "duplicate target".to_string();
                } else if new != old {
                    if let Some(dir) = path.parent() {
                        if dir.join(&new).exists() {
                            problem = "target exists".to_string();
                        }
                    }
                }
                seen.push(new.clone());

                Plan {
                    path: path.clone(),
                    new_name: new,
                    problem,
                }
            })
            .collect()
    }

    /// Apply the planned renames; returns how many succeeded.
    fn apply(&mut self) -> Result<usize, String> {
        let mut done = 0;
        for p in self.plan() {
            if p.problem.is_empty() && p.new_name != file_name(&p.path) {
                match p.path.parent() {
                    Some(dir) => {
                        std::fs::rename(&p.path, dir.join(&p.new_name)).map_err(|e| {
                            format!("'{}': {}", file_name(&p.path), e)
                        })?;
                        done += 1;
                    }
                    None => return Err(format!("'{}': no parent folder", file_name(&p.path))),
                }
            }
        }
        self.files.retain(|f| f.exists());
        Ok(done)
    }
}

/// The file name of a path as a plain string.
fn file_name(path: &Path) -> String {
    path.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
}

/// Expand a TC-style mask against `original` (the current file name):
/// `*` = the original name / extension, `[N]` = the counter, everything
/// else is literal text. The counter is consumed left to right.
fn expand_mask(name_mask: &str, ext_mask: &str, original: &str, counter: i32) -> String {
    let mut c = counter;
    let mut name = String::new();
    let mut chars = name_mask.chars().peekable();
    while let Some(ch) = chars.next() {
        if ch == '*' {
            name.push_str(stem_of(original));
        } else if ch == '[' && chars.peek() == Some(&'N') {
            chars.next();
            if chars.peek() == Some(&']') {
                chars.next();
            }
            name.push_str(&c.to_string());
            c += 1;
        } else {
            name.push(ch);
        }
    }

    let ext = if ext_mask.is_empty() {
        String::new()
    } else {
        let mut e = String::new();
        let mut chars = ext_mask.chars().peekable();
        while let Some(ch) = chars.next() {
            if ch == '*' {
                e.push_str(ext_of(original));
            } else if ch == '[' && chars.peek() == Some(&'N') {
                chars.next();
                if chars.peek() == Some(&']') {
                    chars.next();
                }
                e.push_str(&c.to_string());
                c += 1;
            } else {
                e.push(ch);
            }
        }
        e
    };

    if ext.is_empty() {
        name
    } else {
        format!("{}.{}", name, ext)
    }
}

/// Stem of a file name (everything before the last dot, if any).
fn stem_of(name: &str) -> &str {
    match name.rfind('.') {
        Some(i) if i > 0 => &name[..i],
        _ => name,
    }
}

/// Extension of a file name (after the last dot, no dot).
fn ext_of(name: &str) -> &str {
    match name.rfind('.') {
        Some(i) if i > 0 && i < name.len() - 1 => &name[i + 1..],
        _ => "",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn star_keeps_original_parts() {
        assert_eq!(expand_mask("*", "*", "photo.jpg", 1), "photo.jpg");
        assert_eq!(expand_mask("*_copy", "*", "photo.jpg", 1), "photo_copy.jpg");
        assert_eq!(expand_mask("*", "png", "photo.jpg", 1), "photo.png");
    }

    #[test]
    fn counters_increment() {
        assert_eq!(expand_mask("file_[N]", "", "a.txt", 5), "file_5");
        assert_eq!(expand_mask("f[N]_[N]", "*", "x.bin", 1), "f1_2.bin");
    }

    #[test]
    fn empty_ext_mask_drops_extension() {
        assert_eq!(expand_mask("*", "", "archive.tar.gz", 1), "archive.tar");
    }

    #[test]
    fn literals_only() {
        assert_eq!(expand_mask("report", "txt", "whatever.txt", 1), "report.txt");
    }

    #[test]
    fn stems_and_exts() {
        assert_eq!(stem_of("noext"), "noext");
        assert_eq!(stem_of(".hidden"), ".hidden");
        assert_eq!(ext_of("noext"), "");
        assert_eq!(ext_of("a.b.c"), "c");
    }
}
