use std::cell::{Cell, RefCell};
use std::path::PathBuf;
use std::sync::atomic::Ordering;
use std::time::Instant;

use eframe::egui;
use egui_extras::{Column, TableBuilder};

use crate::jobs::{Job, JobKind, JobState};
use crate::ops;
use crate::panel::{fmt_size, fmt_time, list_roots, FileEntry, FilePanel, SortKey};
use crate::tools::{ToolTab, Toolbox};
use crate::viewer::Viewer;

/// Modal dialogs (copy/move destination, mkdir/rename input, delete confirm).
enum Modal {
    Copy { target: String, count: usize },
    Move { target: String, count: usize },
    Mkdir { name: String },
    Rename { path: PathBuf, name: String },
    Delete { count: usize, preview: Vec<String> },
}

/// Which sort header was clicked inside a table body render pass.
#[derive(Clone, Copy)]
struct SortClick {
    key: SortKey,
    flip: bool,
}

/// Short-lived notification shown in the bottom-right corner.
struct Toast {
    at: Instant,
    msg: String,
    error: bool,
}

const SHORTCUTS: &[(&str, &str)] = &[
    ("Tab", "Switch active panel"),
    ("Up / Down", "Move cursor"),
    ("Home / End", "First / last entry"),
    ("Enter", "Open folder / view file"),
    ("Backspace", "Go to parent folder"),
    ("Insert / Space", "Toggle selection at cursor"),
    ("Ctrl+Click", "Add to / remove from selection"),
    ("Shift+Click", "Range selection"),
    ("Double-click", "Enter folder / open file with system app"),
    ("F3", "View file (read-only)"),
    ("F4", "Edit file"),
    ("F5", "Copy selection to opposite panel"),
    ("F6", "Move selection to opposite panel"),
    ("Shift+F6", "Rename current entry"),
    ("F7", "New folder"),
    ("F8 / Delete", "Delete selection (permanent)"),
    ("Ctrl+S", "Save file in the editor"),
    ("Esc", "Close dialog"),
];

pub struct CommanderApp {
    panels: [FilePanel; 2],
    active: usize,
    dark: bool,
    jobs: Vec<Job>,
    modal: Option<Modal>,
    toolbox: Toolbox,
    viewer: Option<Viewer>,
    show_help: bool,
    toasts: Vec<Toast>,
}

/// Default start directory: user home (USERPROFILE on Windows).
pub fn start_dir() -> PathBuf {
    std::env::var("HOME")
        .or_else(|_| std::env::var("USERPROFILE"))
        .ok()
        .map(PathBuf::from)
        .filter(|p| p.is_dir())
        .unwrap_or_else(|| std::env::current_dir().unwrap_or_else(|_| PathBuf::from("/")))
}

impl CommanderApp {
    pub fn new(_cc: &eframe::CreationContext<'_>) -> Self {
        let start = start_dir();
        Self {
            panels: [FilePanel::new(start.clone()), FilePanel::new(start)],
            active: 0,
            dark: true,
            jobs: Vec::new(),
            modal: None,
            toolbox: Toolbox::default(),
            viewer: None,
            show_help: false,
            toasts: Vec::new(),
        }
    }

    // ---------------------------------------------------------------- helpers

    fn toast(&mut self, msg: impl Into<String>, error: bool) {
        self.toasts.push(Toast {
            at: Instant::now(),
            msg: msg.into(),
            error,
        });
        if self.toasts.len() > 5 {
            let drop = self.toasts.len() - 5;
            self.toasts.drain(0..drop);
        }
    }

    fn reload_both(&mut self) {
        self.panels[0].reload();
        self.panels[1].reload();
    }

    fn set_hidden(&mut self, show: bool) {
        for p in self.panels.iter_mut() {
            p.show_hidden = show;
            p.apply_filter_sort();
        }
    }

    fn current_entry(&self, panel_idx: usize) -> Option<FileEntry> {
        let p = &self.panels[panel_idx];
        p.visible_entry(p.cursor).cloned()
    }

    fn selected_entries(&self, panel_idx: usize) -> Vec<FileEntry> {
        let p = &self.panels[panel_idx];
        p.visible
            .iter()
            .filter(|&&vi| p.selected.contains(&p.entries[vi].path))
            .map(|&vi| p.entries[vi].clone())
            .collect()
    }

    /// Entries the next operation applies to: the explicit selection, or the
    /// cursor entry when nothing is selected (Total Commander behavior).
    fn op_items(&self) -> Vec<FileEntry> {
        let sel = self.selected_entries(self.active);
        if sel.is_empty() {
            self.current_entry(self.active).into_iter().collect()
        } else {
            sel
        }
    }

    fn move_cursor(&mut self, delta: i32) {
        let p = &mut self.panels[self.active];
        if p.visible.is_empty() {
            return;
        }
        let cur = p.cursor as i32 + delta;
        p.cursor = cur.clamp(0, p.visible.len() as i32 - 1) as usize;
    }

    fn set_cursor(&mut self, idx: usize) {
        let p = &mut self.panels[self.active];
        if !p.visible.is_empty() {
            p.cursor = idx.min(p.visible.len() - 1);
        }
    }

    fn toggle_select_cursor(&mut self) {
        if let Some(e) = self.current_entry(self.active) {
            let p = &mut self.panels[self.active];
            if p.selected.contains(&e.path) {
                p.selected.remove(&e.path);
            } else {
                p.selected.insert(e.path);
            }
        }
    }

    fn nav_parent(&mut self) {
        if let Some(par) = self
            .panels[self.active]
            .path
            .parent()
            .map(|p| p.to_path_buf())
        {
            self.panels[self.active].set_path(par);
        }
    }

    fn enter_current(&mut self) {
        if let Some(e) = self.current_entry(self.active) {
            if e.is_dir {
                self.panels[self.active].set_path(e.path);
            } else {
                self.viewer = Some(Viewer::open(&e.path, false));
            }
        }
    }

    fn open_viewer(&mut self, editing: bool) {
        if let Some(e) = self.current_entry(self.active) {
            if e.is_dir {
                self.toast("Cannot view a folder - navigate into it", true);
            } else {
                self.viewer = Some(Viewer::open(&e.path, editing));
            }
        }
    }

    fn open_external_current(&mut self) {
        if let Some(e) = self.current_entry(self.active) {
            match open::that(&e.path) {
                Ok(()) => self.toast(format!("Opening '{}' with system app", e.name), false),
                Err(err) => self.toast(format!("Could not open '{}': {}", e.name, err), true),
            }
        }
    }

    // ---------------------------------------------------------------- modals

    fn open_copy_modal(&mut self) {
        let items = self.op_items();
        if items.is_empty() {
            return;
        }
        let target = self.panels[1 - self.active].path.display().to_string();
        self.modal = Some(Modal::Copy {
            target,
            count: items.len(),
        });
    }

    fn open_move_modal(&mut self) {
        let items = self.op_items();
        if items.is_empty() {
            return;
        }
        let target = self.panels[1 - self.active].path.display().to_string();
        self.modal = Some(Modal::Move {
            target,
            count: items.len(),
        });
    }

    fn open_rename_modal(&mut self) {
        if let Some(e) = self.current_entry(self.active) {
            self.modal = Some(Modal::Rename {
                path: e.path,
                name: e.name,
            });
        }
    }

    fn open_delete_modal(&mut self) {
        let items = self.op_items();
        if items.is_empty() {
            return;
        }
        let preview: Vec<String> = items.iter().take(6).map(|e| e.name.clone()).collect();
        self.modal = Some(Modal::Delete {
            count: items.len(),
            preview,
        });
    }

    fn confirm_modal(&mut self) {
        let modal = self.modal.take();
        match modal {
            Some(Modal::Copy { target, .. }) => self.start_transfer(JobKind::Copy, target),
            Some(Modal::Move { target, .. }) => self.start_transfer(JobKind::Move, target),
            Some(Modal::Mkdir { name }) => self.do_mkdir(name),
            Some(Modal::Rename { path, name }) => self.do_rename(path, name),
            Some(Modal::Delete { .. }) => self.start_delete(),
            None => {}
        }
    }

    fn start_transfer(&mut self, kind: JobKind, target: String) {
        let items = self.op_items();
        let dst = PathBuf::from(target.trim());
        if items.is_empty() {
            self.toast("Nothing selected", true);
            return;
        }
        if !dst.is_dir() {
            self.toast("Target folder does not exist", true);
            return;
        }
        let total = items.len() as u64;
        let names: Vec<String> = items.iter().map(|e| e.name.clone()).collect();
        let verb = if kind == JobKind::Copy { "Copy" } else { "Move" };
        let job = Job::spawn(
            kind,
            format!("{} {} item(s)", verb, items.len()),
            move |state, cancel| {
                if let Ok(mut s) = state.lock() {
                    s.total = total;
                }
                for (i, item) in items.iter().enumerate() {
                    if cancel.load(Ordering::Relaxed) {
                        if let Ok(mut s) = state.lock() {
                            s.error = Some("cancelled".to_string());
                        }
                        break;
                    }
                    if let Ok(mut s) = state.lock() {
                        s.current = names[i].clone();
                    }
                    let r = if kind == JobKind::Copy {
                        ops::copy_path(&item.path, &dst)
                    } else {
                        ops::move_path(&item.path, &dst)
                    };
                    if let Err(e) = r {
                        if let Ok(mut s) = state.lock() {
                            s.error = Some(format!("'{}': {}", names[i], e));
                        }
                        break;
                    }
                    if let Ok(mut s) = state.lock() {
                        s.done = i as u64 + 1;
                    }
                }
                if let Ok(mut s) = state.lock() {
                    s.finished = true;
                }
            },
        );
        self.jobs.push(job);
    }

    fn start_delete(&mut self) {
        let items = self.op_items();
        if items.is_empty() {
            return;
        }
        let total = items.len() as u64;
        let names: Vec<String> = items.iter().map(|e| e.name.clone()).collect();
        let job = Job::spawn(
            JobKind::Delete,
            format!("Delete {} item(s)", items.len()),
            move |state, cancel| {
                if let Ok(mut s) = state.lock() {
                    s.total = total;
                }
                for (i, item) in items.iter().enumerate() {
                    if cancel.load(Ordering::Relaxed) {
                        if let Ok(mut s) = state.lock() {
                            s.error = Some("cancelled".to_string());
                        }
                        break;
                    }
                    if let Ok(mut s) = state.lock() {
                        s.current = names[i].clone();
                    }
                    if let Err(e) = ops::remove_path(&item.path) {
                        if let Ok(mut s) = state.lock() {
                            s.error = Some(format!("'{}': {}", names[i], e));
                        }
                        break;
                    }
                    if let Ok(mut s) = state.lock() {
                        s.done = i as u64 + 1;
                    }
                }
                if let Ok(mut s) = state.lock() {
                    s.finished = true;
                }
            },
        );
        self.jobs.push(job);
    }

    fn do_mkdir(&mut self, name: String) {
        let name = name.trim().to_string();
        if name.is_empty() {
            self.toast("Folder name is empty", true);
            return;
        }
        let path = self.panels[self.active].path.join(&name);
        match std::fs::create_dir(&path) {
            Ok(()) => {
                self.reload_both();
                self.toast(format!("Created folder '{}'", name), false);
            }
            Err(e) => self.toast(format!("Create folder failed: {}", e), true),
        }
    }

    fn do_rename(&mut self, path: PathBuf, name: String) {
        let name = name.trim().to_string();
        if name.is_empty() {
            self.toast("Name is empty", true);
            return;
        }
        let new_path = match path.parent() {
            Some(par) => par.join(&name),
            None => PathBuf::from(&name),
        };
        if new_path == path {
            return;
        }
        match std::fs::rename(&path, &new_path) {
            Ok(()) => {
                self.reload_both();
                self.toast(format!("Renamed to '{}'", name), false);
            }
            Err(e) => self.toast(format!("Rename failed: {}", e), true),
        }
    }

    // ------------------------------------------------------------------ jobs

    fn poll_jobs(&mut self, ctx: &egui::Context) {
        let mut any_running = false;
        let mut finished: Vec<(JobKind, JobState, String)> = Vec::new();
        for j in &self.jobs {
            let snap = j.snapshot();
            if snap.finished {
                finished.push((j.kind, snap, j.label.clone()));
            } else {
                any_running = true;
            }
        }
        if !finished.is_empty() {
            for (kind, snap, label) in finished {
                match kind {
                    JobKind::Copy | JobKind::Move | JobKind::Delete => {
                        self.reload_both();
                        match &snap.error {
                            Some(e) if e == "cancelled" => {
                                self.toast(format!("{} cancelled", label), false)
                            }
                            Some(e) => self.toast(format!("{} failed: {}", label, e), true),
                            None => self.toast(format!("{} finished", label), false),
                        }
                    }
                    JobKind::Hash => self.toolbox.hash.on_done(snap),
                    JobKind::Search => self.toolbox.search.on_done(snap),
                    JobKind::Dup => self.toolbox.dup.on_done(snap),
                }
            }
            self.jobs.retain(|j| !j.snapshot().finished);
        }
        if any_running {
            ctx.request_repaint_after(std::time::Duration::from_millis(100));
        }
    }

    // ------------------------------------------------------------------ keys

    fn handle_keys(&mut self, ctx: &egui::Context) {
        // Ctrl+S saves the file in the viewer/editor regardless of focus.
        if self.viewer.is_some() {
            let save = ctx.input(|i| i.modifiers.ctrl && i.key_pressed(egui::Key::S));
            if save {
                if let Some(v) = self.viewer.as_mut() {
                    v.save();
                    self.toast("File saved", false);
                }
            }
        }

        let typing = ctx.wants_keyboard_input();
        let modal_open = self.modal.is_some();

        if !modal_open {
            if ctx.input(|i| i.key_pressed(egui::Key::F5)) {
                self.open_copy_modal();
            }
            if ctx.input(|i| i.key_pressed(egui::Key::F6)) {
                let shift = ctx.input(|i| i.modifiers.shift);
                if shift {
                    self.open_rename_modal();
                } else {
                    self.open_move_modal();
                }
            }
            if ctx.input(|i| i.key_pressed(egui::Key::F7)) {
                self.modal = Some(Modal::Mkdir { name: String::new() });
            }
            if ctx.input(|i| i.key_pressed(egui::Key::F8)) {
                self.open_delete_modal();
            }
            if ctx.input(|i| i.key_pressed(egui::Key::F3)) {
                self.open_viewer(false);
            }
            if ctx.input(|i| i.key_pressed(egui::Key::F4)) {
                self.open_viewer(true);
            }
        }

        if !typing && !modal_open {
            let (tab, up, down, enter, back, ins, space, del, home, end) = ctx.input(|i| (
                i.key_pressed(egui::Key::Tab),
                i.key_pressed(egui::Key::ArrowUp),
                i.key_pressed(egui::Key::ArrowDown),
                i.key_pressed(egui::Key::Enter),
                i.key_pressed(egui::Key::Backspace),
                i.key_pressed(egui::Key::Insert),
                i.key_pressed(egui::Key::Space),
                i.key_pressed(egui::Key::Delete),
                i.key_pressed(egui::Key::Home),
                i.key_pressed(egui::Key::End),
            ));
            if tab {
                self.active = 1 - self.active;
            }
            if up {
                self.move_cursor(-1);
            }
            if down {
                self.move_cursor(1);
            }
            if home {
                self.set_cursor(0);
            }
            if end {
                let len = self.panels[self.active].visible.len();
                if len > 0 {
                    self.set_cursor(len - 1);
                }
            }
            if enter {
                self.enter_current();
            }
            if back {
                self.nav_parent();
            }
            if ins || space {
                self.toggle_select_cursor();
            }
            if del {
                self.open_delete_modal();
            }
        }
    }

    // ------------------------------------------------------------------- UI

    fn show_top_bar(&mut self, ctx: &egui::Context) {
        egui::TopBottomPanel::top("top_bar").show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.menu_button("File", |ui| {
                    if ui.button("New folder  (F7)").clicked() {
                        self.modal = Some(Modal::Mkdir { name: String::new() });
                    }
                    if ui.button("Rename  (Shift+F6)").clicked() {
                        self.open_rename_modal();
                    }
                    if ui.button("Delete  (F8)").clicked() {
                        self.open_delete_modal();
                    }
                    ui.separator();
                    if ui.button("Open with system app").clicked() {
                        self.open_external_current();
                    }
                    ui.separator();
                    if ui.button("Quit").clicked() {
                        ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                    }
                });

                ui.menu_button("Tools", |ui| {
                    if ui.button("Hash calculator").clicked() {
                        self.toolbox.show = true;
                        self.toolbox.tab = ToolTab::Hash;
                    }
                    if ui.button("File search").clicked() {
                        self.toolbox.show = true;
                        self.toolbox.tab = ToolTab::Search;
                        if self.toolbox.search.root.trim().is_empty() {
                            self.toolbox.search.root = self.panels[self.active].path.display().to_string();
                        }
                    }
                    if ui.button("Duplicate finder").clicked() {
                        self.toolbox.show = true;
                        self.toolbox.tab = ToolTab::Duplicates;
                        if self.toolbox.dup.root.trim().is_empty() {
                            self.toolbox.dup.root = self.panels[self.active].path.display().to_string();
                        }
                    }
                    if ui.button("System info").clicked() {
                        self.toolbox.show = true;
                        self.toolbox.tab = ToolTab::System;
                    }
                });

                ui.menu_button("View", |ui| {
                    let mut hid = self.panels[0].show_hidden;
                    if ui.checkbox(&mut hid, "Show hidden files").changed() {
                        self.set_hidden(hid);
                    }
                    let label = if self.dark {
                        "Switch to light theme"
                    } else {
                        "Switch to dark theme"
                    };
                    if ui.button(label).clicked() {
                        self.dark = !self.dark;
                        let dark = self.dark;
                        ctx.set_visuals(if dark {
                            egui::Visuals::dark()
                        } else {
                            egui::Visuals::light()
                        });
                    }
                });

                ui.menu_button("Help", |ui| {
                    if ui.button("Keyboard shortcuts").clicked() {
                        self.show_help = true;
                    }
                });

                ui.separator();
                if ui.button("Up").clicked() {
                    self.nav_parent();
                }
                if ui.button("Home").clicked() {
                    let d = start_dir();
                    self.panels[self.active].set_path(d);
                }
                ui.menu_button("Drives", |ui| {
                    for root in list_roots() {
                        if ui.button(root.display().to_string()).clicked() {
                            self.panels[self.active].set_path(root);
                        }
                    }
                });
                if ui.button("Refresh").clicked() {
                    self.reload_both();
                }
            });
        });
    }

    fn show_status_bar(&mut self, ctx: &egui::Context) {
        egui::TopBottomPanel::bottom("status_bar").show(ctx, |ui| {
            ui.horizontal_wrapped(|ui| {
                for j in &self.jobs {
                    let s = j.snapshot();
                    let text = if s.finished {
                        format!("{} - done", j.label)
                    } else if s.total > 0 {
                        format!("{}: {}/{} - {}", j.label, s.done, s.total, s.current)
                    } else {
                        format!("{}: {}", j.label, s.current)
                    };
                    ui.label(egui::RichText::new(text).small());
                    if !s.finished && ui.small_button("Stop").clicked() {
                        j.cancel();
                    }
                    ui.separator();
                }
                ui.label(
                    egui::RichText::new(
                        "Tab switch panel | Enter open | Backspace up | Insert select | F3 view | F4 edit | F5 copy | F6 move | F7 mkdir | F8 delete",
                    )
                    .weak()
                    .small(),
                );
            });
        });
    }

    fn show_panels(&mut self, ctx: &egui::Context) {
        // SidePanel/CentralPanel give each pane a top-down layout. Do NOT wrap
        // the panes in ui.horizontal + allocate_ui: allocate_ui inherits the
        // parent's horizontal layout, which flattens each pane's contents
        // (path bar, toolbar, table) into one row with a collapsed Name column.
        egui::SidePanel::left("pane_left")
            .resizable(true)
            .min_width(320.0)
            .show(ctx, |ui| self.draw_panel(ui, 0));
        egui::CentralPanel::default().show(ctx, |ui| self.draw_panel(ui, 1));
    }

    fn draw_panel(&mut self, ui: &mut egui::Ui, idx: usize) {
        let active = self.active == idx;
        let accent = ui.visuals().selection.stroke.color;
        let stroke = if active {
            egui::Stroke::new(2.0_f32, accent)
        } else {
            ui.visuals().widgets.inactive.bg_stroke
        };

        egui::Frame::default()
            .stroke(stroke)
            .rounding(6.0)
            .inner_margin(egui::Margin::same(8.0))
            .show(ui, |ui| {
                let p = &mut self.panels[idx];

                // Editable path bar.
                ui.horizontal(|ui| {
                    let resp = ui.add(
                        egui::TextEdit::singleline(&mut p.path_edit)
                            .hint_text("Enter a folder path...")
                            .desired_width(ui.available_width() - 50.0),
                    );
                    let go = ui.button("Go").clicked()
                        || (resp.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)));
                    if go {
                        let np = PathBuf::from(p.path_edit.trim());
                        if np.is_dir() {
                            p.set_path(np);
                        } else {
                            p.path_edit = p.path.display().to_string();
                        }
                    }
                });

                // Panel toolbar.
                ui.horizontal(|ui| {
                    if ui.button("Up").clicked() {
                        if let Some(par) = p.path.parent().map(|x| x.to_path_buf()) {
                            p.set_path(par);
                        }
                    }
                    if ui.button("Home").clicked() {
                        p.set_path(start_dir());
                    }
                    ui.menu_button("Drives", |ui| {
                        for root in list_roots() {
                            if ui.button(root.display().to_string()).clicked() {
                                p.set_path(root);
                            }
                        }
                    });
                    if ui.button("Refresh").clicked() {
                        p.reload();
                    }
                    let mut hid = p.show_hidden;
                    if ui.checkbox(&mut hid, "Hidden").changed() {
                        p.show_hidden = hid;
                        p.apply_filter_sort();
                    }
                });

                ui.add_space(4.0);

                // File table.
                let sort_key = p.sort;
                let ascending = p.ascending;
                let dir_color = egui::Color32::from_rgb(255, 200, 100);
                let sort_click = Cell::new(None::<SortClick>);
                let open_ext = RefCell::new(None::<PathBuf>);

                TableBuilder::new(ui)
                    .striped(true)
                    .vscroll(true)
                    .column(Column::remainder().clip(true).resizable(true))
                    .column(Column::auto().resizable(true))
                    .column(Column::auto())
                    .column(Column::auto())
                    .header(20.0, |mut header| {
                        header.col(|ui| {
                            if sort_header(ui, "Name", sort_key, SortKey::Name, ascending) {
                                sort_click.set(Some(SortClick {
                                    key: SortKey::Name,
                                    flip: sort_key == SortKey::Name,
                                }));
                            }
                        });
                        header.col(|ui| {
                            if sort_header(ui, "Size", sort_key, SortKey::Size, ascending) {
                                sort_click.set(Some(SortClick {
                                    key: SortKey::Size,
                                    flip: sort_key == SortKey::Size,
                                }));
                            }
                        });
                        header.col(|ui| {
                            if sort_header(ui, "Modified", sort_key, SortKey::Modified, ascending) {
                                sort_click.set(Some(SortClick {
                                    key: SortKey::Modified,
                                    flip: sort_key == SortKey::Modified,
                                }));
                            }
                        });
                        header.col(|ui| {
                            if sort_header(ui, "Ext", sort_key, SortKey::Ext, ascending) {
                                sort_click.set(Some(SortClick {
                                    key: SortKey::Ext,
                                    flip: sort_key == SortKey::Ext,
                                }));
                            }
                        });
                    })
                    .body(|mut body| {
                        for vi in 0..p.visible.len() {
                            let e = &p.entries[p.visible[vi]];
                            let is_sel = p.selected.contains(&e.path);
                            let is_cur = vi == p.cursor;
                            let name = e.name.clone();
                            let is_dir = e.is_dir;
                            let path = e.path.clone();
                            let size = e.size;
                            let modified = e.modified;
                            let ext = if is_dir {
                                String::new()
                            } else {
                                e.ext_lower()
                            };

                            body.row(18.0, |mut row| {
                                row.set_selected(is_sel || is_cur);

                                // Name cell (primary interaction).
                                row.col(|ui| {
                                    let mut txt = egui::RichText::new(&name);
                                    txt = if is_dir {
                                        txt.strong().color(dir_color)
                                    } else {
                                        txt
                                    };
                                    let r = ui.selectable_label(is_sel, txt);
                                    if r.clicked() {
                                        let mods = ui.input(|i| i.modifiers);
                                        if mods.ctrl {
                                            if is_sel {
                                                p.selected.remove(&path);
                                            } else {
                                                p.selected.insert(path.clone());
                                            }
                                            p.cursor = vi;
                                        } else if mods.shift {
                                            let lo = vi.min(p.cursor);
                                            let hi = vi.max(p.cursor);
                                            p.selected.clear();
                                            for k in lo..=hi {
                                                p.selected
                                                    .insert(p.entries[p.visible[k]].path.clone());
                                            }
                                        } else {
                                            p.selected.clear();
                                            p.selected.insert(path.clone());
                                            p.cursor = vi;
                                        }
                                    }
                                    if r.double_clicked() {
                                        if is_dir {
                                            p.set_path(path.clone());
                                        } else {
                                            *open_ext.borrow_mut() = Some(path.clone());
                                        }
                                    }
                                });
                                row.col(|ui| {
                                    if ui.selectable_label(is_sel, fmt_size(size)).clicked() {
                                        p.selected.clear();
                                        p.selected.insert(path.clone());
                                        p.cursor = vi;
                                    }
                                });
                                row.col(|ui| {
                                    if ui
                                        .selectable_label(is_sel, fmt_time(modified))
                                        .clicked()
                                    {
                                        p.selected.clear();
                                        p.selected.insert(path.clone());
                                        p.cursor = vi;
                                    }
                                });
                                row.col(|ui| {
                                    let t = if is_dir {
                                        "DIR".to_string()
                                    } else {
                                        ext.clone()
                                    };
                                    if ui.selectable_label(is_sel, t).clicked() {
                                        p.selected.clear();
                                        p.selected.insert(path.clone());
                                        p.cursor = vi;
                                    }
                                });
                            });
                        }
                    });

                if let Some(sc) = sort_click.get() {
                    if sc.flip {
                        p.ascending = !p.ascending;
                    } else {
                        p.sort = sc.key;
                        p.ascending = true;
                    }
                    p.apply_filter_sort();
                }
                if let Some(path) = open_ext.borrow_mut().take() {
                    let _ = open::that(&path);
                }

                // Panel footer.
                ui.add_space(2.0);
                let (dirs, files, bytes) = p.stats();
                let (sn, sbytes) = p.selected_stats();
                ui.label(
                    egui::RichText::new(format!(
                        "{} dirs, {} files, {} | selected: {}, {}",
                        dirs,
                        files,
                        fmt_size(bytes),
                        sn,
                        fmt_size(sbytes)
                    ))
                    .small()
                    .weak(),
                );
            });
    }

    fn show_modal(&mut self, ctx: &egui::Context) {
        if self.modal.is_none() {
            return;
        }
        if ctx.input(|i| i.key_pressed(egui::Key::Escape)) {
            self.modal = None;
            return;
        }
        let mut confirmed = false;
        let mut closed = false;
        {
            let modal = self.modal.as_mut().unwrap();
            match modal {
                Modal::Copy { target, count } => {
                    let mut open = true;
                    egui::Window::new(format!("Copy {} item(s)", count))
                        .open(&mut open)
                        .collapsible(false)
                        .show(ctx, |ui| {
                            ui.label("Copy to folder:");
                            let resp =
                                ui.add(egui::TextEdit::singleline(target).desired_width(420.0));
                            ui.add_space(6.0);
                            ui.horizontal(|ui| {
                                if ui.button("Copy").clicked()
                                    || (resp.lost_focus()
                                        && ui.input(|i| i.key_pressed(egui::Key::Enter)))
                                {
                                    confirmed = true;
                                }
                                if ui.button("Cancel").clicked() {
                                    closed = true;
                                }
                            });
                        });
                    if !open {
                        closed = true;
                    }
                }
                Modal::Move { target, count } => {
                    let mut open = true;
                    egui::Window::new(format!("Move {} item(s)", count))
                        .open(&mut open)
                        .collapsible(false)
                        .show(ctx, |ui| {
                            ui.label("Move to folder:");
                            let resp =
                                ui.add(egui::TextEdit::singleline(target).desired_width(420.0));
                            ui.add_space(6.0);
                            ui.horizontal(|ui| {
                                if ui.button("Move").clicked()
                                    || (resp.lost_focus()
                                        && ui.input(|i| i.key_pressed(egui::Key::Enter)))
                                {
                                    confirmed = true;
                                }
                                if ui.button("Cancel").clicked() {
                                    closed = true;
                                }
                            });
                        });
                    if !open {
                        closed = true;
                    }
                }
                Modal::Mkdir { name } => {
                    let mut open = true;
                    egui::Window::new("New folder")
                        .open(&mut open)
                        .collapsible(false)
                        .show(ctx, |ui| {
                            ui.label("Folder name:");
                            let resp =
                                ui.add(egui::TextEdit::singleline(name).desired_width(320.0));
                            ui.add_space(6.0);
                            ui.horizontal(|ui| {
                                if ui.button("Create").clicked()
                                    || (resp.lost_focus()
                                        && ui.input(|i| i.key_pressed(egui::Key::Enter)))
                                {
                                    confirmed = true;
                                }
                                if ui.button("Cancel").clicked() {
                                    closed = true;
                                }
                            });
                        });
                    if !open {
                        closed = true;
                    }
                }
                Modal::Rename { name, .. } => {
                    let mut open = true;
                    egui::Window::new("Rename")
                        .open(&mut open)
                        .collapsible(false)
                        .show(ctx, |ui| {
                            ui.label("New name:");
                            let resp =
                                ui.add(egui::TextEdit::singleline(name).desired_width(320.0));
                            ui.add_space(6.0);
                            ui.horizontal(|ui| {
                                if ui.button("Rename").clicked()
                                    || (resp.lost_focus()
                                        && ui.input(|i| i.key_pressed(egui::Key::Enter)))
                                {
                                    confirmed = true;
                                }
                                if ui.button("Cancel").clicked() {
                                    closed = true;
                                }
                            });
                        });
                    if !open {
                        closed = true;
                    }
                }
                Modal::Delete { count, preview } => {
                    let mut open = true;
                    egui::Window::new("Confirm delete")
                        .open(&mut open)
                        .collapsible(false)
                        .show(ctx, |ui| {
                            ui.colored_label(
                                egui::Color32::from_rgb(255, 140, 100),
                                format!("Permanently delete {} item(s)?", count),
                            );
                            for name in preview {
                                ui.label(egui::RichText::new(format!("  {}", name)).small());
                            }
                            ui.add_space(6.0);
                            ui.horizontal(|ui| {
                                if ui.button("Delete").clicked() {
                                    confirmed = true;
                                }
                                if ui.button("Cancel").clicked() {
                                    closed = true;
                                }
                            });
                        });
                    if !open {
                        closed = true;
                    }
                }
            }
        }
        if confirmed {
            self.confirm_modal();
        } else if closed {
            self.modal = None;
        }
    }

    fn show_toolbox(&mut self, ctx: &egui::Context) {
        if !self.toolbox.show {
            return;
        }
        let mut open = true;
        egui::Window::new("Utility toolbox")
            .open(&mut open)
            .default_size([700.0, 500.0])
            .show(ctx, |ui| {
                self.toolbox.ui(ui, &mut self.jobs);
            });
        if !open {
            self.toolbox.show = false;
        }
    }

    fn show_viewer(&mut self, ctx: &egui::Context) {
        if self.viewer.is_none() {
            return;
        }
        let mut open = true;
        if let Some(v) = self.viewer.as_mut() {
            let title = format!("{}{}", if v.dirty { "* " } else { "" }, v.name);
            egui::Window::new(title)
                .open(&mut open)
                .default_size([880.0, 560.0])
                .show(ctx, |ui| v.ui(ui));
        }
        if !open {
            self.viewer = None;
        }
    }

    fn show_help_window(&mut self, ctx: &egui::Context) {
        if !self.show_help {
            return;
        }
        let mut open = true;
        egui::Window::new("Keyboard shortcuts")
            .open(&mut open)
            .collapsible(false)
            .show(ctx, |ui| {
                egui::Grid::new("help_grid")
                    .num_columns(2)
                    .spacing([16.0, 4.0])
                    .striped(true)
                    .show(ui, |ui| {
                        for (k, d) in SHORTCUTS {
                            ui.strong(*k);
                            ui.label(*d);
                            ui.end_row();
                        }
                    });
            });
        if !open {
            self.show_help = false;
        }
    }

    fn show_toasts(&mut self, ctx: &egui::Context) {
        let now = Instant::now();
        self.toasts
            .retain(|t| now.duration_since(t.at).as_secs_f32() < 4.0);
        if self.toasts.is_empty() {
            return;
        }
        egui::Area::new(egui::Id::new("toasts"))
            .anchor(egui::Align2::RIGHT_BOTTOM, egui::vec2(-14.0, -34.0))
            .order(egui::Order::Foreground)
            .show(ctx, |ui| {
                for t in &self.toasts {
                    let bg = if t.error {
                        egui::Color32::from_rgb(110, 35, 35)
                    } else {
                        egui::Color32::from_rgb(35, 84, 56)
                    };
                    egui::Frame::default()
                        .fill(bg)
                        .rounding(6.0)
                        .inner_margin(egui::Margin::same(8.0))
                        .show(ui, |ui| {
                            ui.label(egui::RichText::new(&t.msg).color(egui::Color32::WHITE));
                        });
                    ui.add_space(4.0);
                }
            });
    }
}

impl eframe::App for CommanderApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_jobs(ctx);
        self.handle_keys(ctx);
        self.show_top_bar(ctx);
        self.show_status_bar(ctx);
        self.show_panels(ctx);
        self.show_modal(ctx);
        self.show_toolbox(ctx);
        self.show_viewer(ctx);
        self.show_help_window(ctx);
        self.show_toasts(ctx);
    }
}

/// Sortable table header button; returns true when clicked.
fn sort_header(ui: &mut egui::Ui, label: &str, current: SortKey, key: SortKey, ascending: bool) -> bool {
    let arrow = if current == key {
        if ascending {
            " ^"
        } else {
            " v"
        }
    } else {
        ""
    };
    ui.button(format!("{}{}", label, arrow)).clicked()
}
