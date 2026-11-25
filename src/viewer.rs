use std::fs;
use std::path::{Path, PathBuf};

use eframe::egui;

/// Id of the editor's text widget: the app checks whether it has focus
/// (Tab then indents instead of switching panes).
pub fn editor_id() -> egui::Id {
    egui::Id::new("viewer_editor")
}

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
                .map(|n| crate::panel::nfc(&n.to_string_lossy()))
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
                let output = egui::TextEdit::multiline(&mut self.text)
                    .font(egui::TextStyle::Monospace)
                    .desired_width(ui.available_width())
                    .desired_rows(22)
                    .interactive(self.editing)
                    .id(editor_id())
                    .show(ui);
                let resp = &output.response;
                if resp.changed() {
                    self.dirty = true;
                }

                if self.editing && resp.has_focus() {
                    // Tab belongs to the text here, not to the panes. egui does
                    // not hand Tab to a text field (it uses Tab to move the
                    // keyboard focus), so the field claims it and the
                    // indentation is done below.
                    ui.memory_mut(|mem| {
                        mem.set_focus_lock_filter(
                            resp.id,
                            egui::EventFilter {
                                tab: true,
                                horizontal_arrows: true,
                                vertical_arrows: true,
                                escape: false,
                            },
                        )
                    });
                    let (tab, shift) =
                        ui.input(|i| (i.key_pressed(egui::Key::Tab), i.modifiers.shift));
                    if tab {
                        if let Some(range) = output.cursor_range {
                            let (a, b) = (
                                range.primary.ccursor.index,
                                range.secondary.ccursor.index,
                            );
                            let (min, max) = indent_text(&mut self.text, a, b, !shift);
                            let mut state = output.state.clone();
                            state.cursor.set_char_range(Some(egui::text::CCursorRange::two(
                                egui::text::CCursor::new(min),
                                egui::text::CCursor::new(max),
                            )));
                            state.store(ui.ctx(), resp.id);
                            self.dirty = true;
                        }
                    }
                }
            });
        }
    }
}

/// Tab / Shift+Tab in the editor. `min..max` is the selected range of
/// characters, empty when both ends coincide. Tab inserts a tab at the cursor,
/// or indents every line that has a selected character; Shift+Tab removes one
/// indentation step (a tab, or up to four spaces) from those lines. Returns
/// the new selection.
fn indent_text(text: &mut String, min: usize, max: usize, indent: bool) -> (usize, usize) {
    let (min, max) = (min.min(max), max.max(min));
    let mut starts = vec![0];
    starts.extend(
        text.char_indices()
            .filter(|(_, c)| *c == '\n')
            .map(|(i, _)| text[..=i].chars().count()),
    );

    if min == max {
        let start = *starts.iter().rev().find(|&&s| s <= min).unwrap_or(&0);
        if indent {
            insert_char(text, min, '\t');
            (min + 1, min + 1)
        } else {
            let removed = remove_indent(text, start);
            let cursor = if min > start {
                (min - removed).max(start)
            } else {
                start
            };
            (cursor, cursor)
        }
    } else {
        // Lines with at least one selected character.
        let selected: Vec<usize> = starts
            .iter()
            .enumerate()
            .filter(|&(i, &s)| {
                let end = starts.get(i + 1).copied().unwrap_or(usize::MAX);
                s < max && end > min
            })
            .map(|(_, &s)| s)
            .collect();
        let first = selected[0];

        if indent {
            for &s in selected.iter().rev() {
                insert_char(text, s, '\t');
            }
            (first, max + selected.len())
        } else {
            let mut removed_total = 0;
            let mut removed_at_first = 0;
            for (i, &s) in selected.iter().enumerate().rev() {
                let n = remove_indent(text, s);
                removed_total += n;
                if i == 0 {
                    removed_at_first = n;
                }
            }
            let new_min = if min <= first {
                first
            } else {
                (min - removed_at_first).max(first)
            };
            (new_min, max - removed_total)
        }
    }
}

/// Byte offset of the character at character index `idx` (the end of the text
/// if `idx` is past it).
fn byte_at(text: &str, idx: usize) -> usize {
    text.char_indices().nth(idx).map_or(text.len(), |(b, _)| b)
}

fn insert_char(text: &mut String, idx: usize, c: char) {
    let at = byte_at(text, idx);
    text.insert(at, c);
}

/// Remove one indentation step from the start of the line that begins at
/// character index `start`. Returns how many characters were removed.
fn remove_indent(text: &mut String, start: usize) -> usize {
    let from = byte_at(text, start);
    let rest = &text[from..];
    let n = if rest.starts_with('\t') {
        1
    } else {
        rest.chars().take_while(|&c| c == ' ').take(4).count()
    };
    let to = byte_at(text, start + n);
    text.replace_range(from..to, "");
    n
}

#[cfg(test)]
mod tests {
    use super::indent_text;

    fn run(text: &str, min: usize, max: usize, indent: bool) -> (String, usize, usize) {
        let mut text = text.to_string();
        let range = indent_text(&mut text, min, max, indent);
        (text, range.0, range.1)
    }

    #[test]
    fn tab_at_the_cursor_inserts_a_tab() {
        assert_eq!(run("abc", 1, 1, true), ("a\tbc".to_string(), 2, 2));
        assert_eq!(run("abc", 0, 0, true), ("\tabc".to_string(), 1, 1));
        assert_eq!(run("abc", 3, 3, true), ("abc\t".to_string(), 4, 4));
    }

    #[test]
    fn shift_tab_at_the_cursor_unindents_the_line() {
        assert_eq!(run("\tabc", 3, 3, false), ("abc".to_string(), 2, 2));
        // At the very start of the line the cursor stays there.
        assert_eq!(run("\tabc", 0, 0, false), ("abc".to_string(), 0, 0));
        // Nothing to remove.
        assert_eq!(run("abc", 1, 1, false), ("abc".to_string(), 1, 1));
    }

    #[test]
    fn tab_indents_every_selected_line() {
        // The selection ends at a line start: that line is not part of it.
        assert_eq!(
            run("one\ntwo\nthree\n", 0, 8, true),
            ("\tone\n\ttwo\nthree\n".to_string(), 0, 10)
        );
    }

    #[test]
    fn shift_tab_takes_it_back() {
        let (indented, min, max) = run("one\ntwo\nthree\n", 0, 8, true);
        assert_eq!(run(&indented, min, max, false), ("one\ntwo\nthree\n".to_string(), 0, 8));
    }

    #[test]
    fn a_selection_that_starts_inside_a_line_covers_whole_lines() {
        // The selection covers the end of line one, line two, and "th".
        assert_eq!(
            run("one\ntwo\nthree", 2, 10, true),
            ("\tone\n\ttwo\n\tthree".to_string(), 0, 13)
        );
        // Ending exactly at a line start leaves that line out.
        assert_eq!(
            run("one\ntwo\nthree", 2, 8, true),
            ("\tone\n\ttwo\nthree".to_string(), 0, 10)
        );
    }

    #[test]
    fn shift_tab_removes_up_to_four_spaces() {
        assert_eq!(run("    a\n  b", 0, 9, false), ("a\nb".to_string(), 0, 3));
    }

    #[test]
    fn shift_tab_keeps_a_selection_that_starts_after_the_indent() {
        assert_eq!(run("\tone\ntwo", 2, 8, false), ("one\ntwo".to_string(), 1, 7));
    }

    #[test]
    fn the_end_of_the_text_can_be_selected() {
        assert_eq!(run("a\nb", 2, 3, true), ("a\n\tb".to_string(), 2, 4));
    }
}
