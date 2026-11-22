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
    /// Confirm discarding unsaved edits in the viewer.
    DiscardViewer,
}

/// Actions requested from a table row's context menu, executed after the
/// panel frame is drawn (they need `&mut self`, unavailable inside the UI
/// closures that borrow the panel).
enum RowAction {
    /// Open a file in the built-in viewer.
    View(PathBuf),
    /// Copy the entry (or the selection containing it) to the opposite panel.
    CopyTo(PathBuf),
    Rename(PathBuf),
    Delete(PathBuf),
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
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        let prefs: Prefs = cc
            .storage
            .and_then(|s| eframe::get_value(s, "prefs"))
            .unwrap_or_default();
        cc.egui_ctx.set_visuals(if prefs.dark {
            egui::Visuals::dark()
        } else {
            egui::Visuals::light()
        });
        let start = start_dir();
        let mut app = Self {
            panels: [FilePanel::new(start.clone()), FilePanel::new(start)],
            active: 0,
            dark: prefs.dark,
            jobs: Vec::new(),
            modal: None,
            toolbox: Toolbox::default(),
            viewer: None,
            show_help: false,
            toasts: Vec::new(),
        };
        if prefs.show_hidden {
            app.set_hidden(true);
        }
        app
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

    /// Point the operation target at `path`: if the panel's selection already
    /// contains it the selection stands, otherwise the selection is reduced to
    /// that single entry (used by row context-menu actions).
    fn focus_op_selection(&mut self, panel_idx: usize, path: &PathBuf) {
        let p = &mut self.panels[panel_idx];
        if !p.selected.contains(path) {
            p.selected.clear();
            p.selected.insert(path.clone());
            if let Some(vi) = p
                .visible
                .iter()
                .position(|&vi| &p.entries[vi].path == path)
            {
                p.cursor = vi;
            }
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

    /// Refuse to swap the viewer while it has unsaved edits.
    fn viewer_busy_with_edits(&self) -> bool {
        matches!(&self.viewer, Some(v) if v.dirty)
    }

    fn enter_current(&mut self) {
        if let Some(e) = self.current_entry(self.active) {
            if e.is_dir {
                self.panels[self.active].set_path(e.path);
            } else {
                if self.viewer_busy_with_edits() {
                    self.toast("Save or discard the open editor first", true);
                    return;
                }
                self.viewer = Some(Viewer::open(&e.path, false));
            }
        }
    }

    fn open_viewer(&mut self, editing: bool) {
        if let Some(e) = self.current_entry(self.active) {
            if e.is_dir {
                self.toast("Cannot view a folder - navigate into it", true);
            } else {
                if self.viewer_busy_with_edits() {
                    self.toast("Save or discard the open editor first", true);
                    return;
                }
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
            Some(Modal::DiscardViewer) => self.viewer = None,
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

                // No global Up/Home/Drives/Refresh here: those live in each
                // panel's own toolbar (the top bar acted only on the active
                // panel, duplicating it).

                // Icon toolbar: mouse access to the function-key operations.
                // All act on the active (highlighted) panel.
                let modal_open = self.modal.is_some();
                let has_items = !self.op_items().is_empty();
                let can_op = !modal_open && has_items;
                ui.separator();
                if icon_button(
                    ui,
                    IconKind::View,
                    can_op,
                    "View file in built-in viewer (F3)",
                ) {
                    self.open_viewer(false);
                }
                if icon_button(ui, IconKind::Edit, can_op, "Edit file (F4)") {
                    self.open_viewer(true);
                }
                ui.separator();
                if icon_button(
                    ui,
                    IconKind::Copy,
                    can_op,
                    "Copy selection to opposite panel (F5)",
                ) {
                    self.open_copy_modal();
                }
                if icon_button(
                    ui,
                    IconKind::Move,
                    can_op,
                    "Move selection to opposite panel (F6)",
                ) {
                    self.open_move_modal();
                }
                ui.separator();
                if icon_button(
                    ui,
                    IconKind::NewFolder,
                    !modal_open,
                    "New folder (F7)",
                ) {
                    self.modal = Some(Modal::Mkdir { name: String::new() });
                }
                if icon_button(ui, IconKind::Rename, can_op, "Rename (Shift+F6)") {
                    self.open_rename_modal();
                }
                if icon_button(
                    ui,
                    IconKind::Delete,
                    can_op,
                    "Delete selection permanently (F8)",
                ) {
                    self.open_delete_modal();
                }
                ui.separator();
                if icon_button(
                    ui,
                    IconKind::OpenExternal,
                    can_op,
                    "Open selected file with system app",
                ) {
                    self.open_external_current();
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
                        "Tab panel · F5 copy · F6 move · F8 delete · F3 view · shortcuts: Help menu",
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
            .default_width((ctx.screen_rect().width() * 0.45).max(360.0))
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
        // Subtle accent tint marks the active pane beyond the border stroke.
        let fill = if active {
            accent.gamma_multiply(0.06)
        } else {
            egui::Color32::TRANSPARENT
        };
        // Deferred results collected while drawing (panel state is borrowed
        // inside the closures below; these are applied once it is released).
        let go_failed = Cell::new(false);
        let clicked = Cell::new(false);
        let row_action = RefCell::new(None::<RowAction>);
        let open_ext = RefCell::new(None::<PathBuf>);

        egui::Frame::default()
            .stroke(stroke)
            .fill(fill)
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
                            go_failed.set(true);
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
                let dir_color = dir_color(ui);
                let sort_click = Cell::new(None::<SortClick>);

                TableBuilder::new(ui)
                    .striped(true)
                    .vscroll(true)
                    .column(Column::remainder().clip(true).resizable(true))
                    .column(Column::auto().resizable(true))
                    .column(Column::auto())
                    .column(Column::auto())
                    .header(22.0, |mut header| {
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

                            body.row(20.0, |mut row| {
                                row.set_selected(is_sel || is_cur);

                                // Name cell (primary interaction).
                                row.col(|ui| {
                                    // Keep the keyboard cursor visible while
                                    // navigating with the arrow keys.
                                    if is_cur {
                                        ui.scroll_to_cursor(None);
                                    }
                                    let mut txt = egui::RichText::new(&name);
                                    txt = if is_dir {
                                        txt.strong().color(dir_color)
                                    } else {
                                        txt
                                    };
                                    // Selectable label plus an invisible click
                                    // catcher over the rest of the cell, so the
                                    // entire line is clickable.
                                    let r = ui
                                        .horizontal(|ui| {
                                            let r = ui.selectable_label(is_sel, txt);
                                            let (_, catcher) = ui.allocate_exact_size(
                                                egui::vec2(
                                                    ui.available_width(),
                                                    ui.available_height(),
                                                ),
                                                egui::Sense::click(),
                                            );
                                            r.union(catcher)
                                        })
                                        .inner;
                                    if r.clicked() {
                                        clicked.set(true);
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
                                        clicked.set(true);
                                        if is_dir {
                                            p.set_path(path.clone());
                                        } else {
                                            *open_ext.borrow_mut() = Some(path.clone());
                                        }
                                    }
                                    // Right-click menu (row actions needing
                                    // `self` are deferred via `row_action`).
                                    r.context_menu(|ui| {
                                        clicked.set(true);
                                        if ui.button(if is_dir { "Open" } else { "Open with system app" }).clicked() {
                                            if is_dir {
                                                p.set_path(path.clone());
                                            } else {
                                                *open_ext.borrow_mut() = Some(path.clone());
                                            }
                                            ui.close_menu();
                                        }
                                        if !is_dir && ui.button("Open in viewer").clicked() {
                                            row_action.borrow_mut().replace(RowAction::View(path.clone()));
                                            ui.close_menu();
                                        }
                                        ui.separator();
                                        if ui.button("Copy to other panel").clicked() {
                                            row_action.borrow_mut().replace(RowAction::CopyTo(path.clone()));
                                            ui.close_menu();
                                        }
                                        if ui.button("Rename…").clicked() {
                                            row_action.borrow_mut().replace(RowAction::Rename(path.clone()));
                                            ui.close_menu();
                                        }
                                        if ui.button("Delete…").clicked() {
                                            row_action.borrow_mut().replace(RowAction::Delete(path.clone()));
                                            ui.close_menu();
                                        }
                                        ui.separator();
                                        if ui.button("Copy name").clicked() {
                                            ui.ctx().copy_text(name.clone());
                                            ui.close_menu();
                                        }
                                        if ui.button("Copy full path").clicked() {
                                            ui.ctx().copy_text(path.display().to_string());
                                            ui.close_menu();
                                        }
                                    });
                                });
                                row.col(|ui| {
                                    let r = ui
                                        .horizontal(|ui| {
                                            let r = ui.selectable_label(is_sel, fmt_size(size));
                                            let (_, catcher) = ui.allocate_exact_size(
                                                egui::vec2(
                                                    ui.available_width(),
                                                    ui.available_height(),
                                                ),
                                                egui::Sense::click(),
                                            );
                                            r.union(catcher)
                                        })
                                        .inner;
                                    if r.clicked() {
                                        p.selected.clear();
                                        p.selected.insert(path.clone());
                                        p.cursor = vi;
                                    }
                                });
                                row.col(|ui| {
                                    let r = ui
                                        .horizontal(|ui| {
                                            let r = ui.selectable_label(is_sel, fmt_time(modified));
                                            let (_, catcher) = ui.allocate_exact_size(
                                                egui::vec2(
                                                    ui.available_width(),
                                                    ui.available_height(),
                                                ),
                                                egui::Sense::click(),
                                            );
                                            r.union(catcher)
                                        })
                                        .inner;
                                    if r.clicked() {
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
                                    let r = ui
                                        .horizontal(|ui| {
                                            let r = ui.selectable_label(is_sel, t);
                                            let (_, catcher) = ui.allocate_exact_size(
                                                egui::vec2(
                                                    ui.available_width(),
                                                    ui.available_height(),
                                                ),
                                                egui::Sense::click(),
                                            );
                                            r.union(catcher)
                                        })
                                        .inner;
                                    if r.clicked() {
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
                if p.visible.is_empty() {
                    let msg = match &p.load_error {
                        Some(e) => format!("Cannot open folder: {}", e),
                        None => "This folder is empty".to_string(),
                    };
                    ui.label(egui::RichText::new(msg).weak().italics());
                } else {
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
                }
            });

        // Apply deferred actions collected while drawing (they need `self`,
        // which was borrowed by the frame closures above).
        if go_failed.get() {
            self.toast("Folder not found", true);
        }
        if clicked.get() {
            self.active = idx;
        }
        let action = row_action.borrow_mut().take();
        if let Some(action) = action {
            match action {
                RowAction::View(path) => {
                    if self.viewer_busy_with_edits() {
                        self.toast("Save or discard the open editor first", true);
                    } else {
                        self.viewer = Some(Viewer::open(&path, false));
                    }
                }
                RowAction::CopyTo(path) => {
                    self.active = idx;
                    self.focus_op_selection(idx, &path);
                    self.open_copy_modal();
                }
                RowAction::Rename(path) => {
                    self.active = idx;
                    let name = self.panels[idx]
                        .entries
                        .iter()
                        .find(|e| e.path == path)
                        .map(|e| e.name.clone())
                        .unwrap_or_default();
                    self.modal = Some(Modal::Rename { path, name });
                }
                RowAction::Delete(path) => {
                    self.active = idx;
                    self.focus_op_selection(idx, &path);
                    self.open_delete_modal();
                }
            }
        }
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
                                ui.visuals().warn_fg_color,
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
                Modal::DiscardViewer => {
                    let mut open = true;
                    let name = self
                        .viewer
                        .as_ref()
                        .map(|v| v.name.clone())
                        .unwrap_or_default();
                    egui::Window::new("Unsaved changes")
                        .open(&mut open)
                        .collapsible(false)
                        .show(ctx, |ui| {
                            ui.label(format!("Close '{}' without saving?", name));
                            ui.add_space(6.0);
                            ui.horizontal(|ui| {
                                if ui.button("Discard changes").clicked() {
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
        // Tools report "navigate the active panel here" requests via this
        // out-parameter (they cannot reach the panels themselves).
        let mut nav: Option<PathBuf> = None;
        egui::Window::new("Utility toolbox")
            .open(&mut open)
            .default_size([700.0, 500.0])
            .show(ctx, |ui| {
                self.toolbox.ui(ui, &mut self.jobs, &mut nav);
            });
        if let Some(target) = nav {
            let p = &mut self.panels[self.active];
            if target.is_dir() {
                p.set_path(target);
            } else {
                let file = target.clone();
                let par = file
                    .parent()
                    .map(|par| par.to_path_buf())
                    .unwrap_or_else(|| PathBuf::from("/"));
                p.set_path(par);
                if let Some(vi) = p
                    .visible
                    .iter()
                    .position(|&vi| p.entries[vi].path == file)
                {
                    p.cursor = vi;
                    p.selected.insert(file);
                }
            }
        }
        if !open {
            self.toolbox.show = false;
        }
    }

    fn show_viewer(&mut self, ctx: &egui::Context) {
        if self.viewer.is_none() {
            return;
        }
        let mut open = true;
        let mut dirty = false;
        if let Some(v) = self.viewer.as_mut() {
            dirty = v.dirty;
            let title = format!("{}{}", if v.dirty { "* " } else { "" }, v.name);
            egui::Window::new(title)
                .open(&mut open)
                .default_size([880.0, 560.0])
                .show(ctx, |ui| v.ui(ui));
        }
        if !open {
            if dirty {
                // Closing over unsaved edits asks for confirmation instead of
                // silently dropping them.
                self.modal = Some(Modal::DiscardViewer);
            } else {
                self.viewer = None;
            }
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
    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        eframe::set_value(
            storage,
            "prefs",
            &Prefs {
                dark: self.dark,
                show_hidden: self.panels[0].show_hidden,
            },
        );
    }

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
            " ↑"
        } else {
            " ↓"
        }
    } else {
        ""
    };
    ui.button(format!("{}{}", label, arrow)).clicked()
}

/// Accent color for directory names, readable in both themes.
fn dir_color(ui: &egui::Ui) -> egui::Color32 {
    if ui.visuals().dark_mode {
        egui::Color32::from_rgb(255, 200, 100)
    } else {
        egui::Color32::from_rgb(150, 90, 0)
    }
}

/// The vector glyphs drawn in the top toolbar (see `icon_button`).
#[derive(Clone, Copy)]
enum IconKind {
    View,
    Edit,
    Copy,
    Move,
    NewFolder,
    Rename,
    Delete,
    OpenExternal,
}

/// Square toolbar button with a hand-drawn vector icon. Returns true on click.
/// Icons are painted with the egui painter (no font glyph or asset involved),
/// so they always match the active theme.
fn icon_button(ui: &mut egui::Ui, kind: IconKind, enabled: bool, tip: &str) -> bool {
    let visuals = ui.visuals().clone();
    let h = ui.spacing().interact_size.y;
    let (rect, mut resp) = ui.allocate_exact_size(egui::vec2(h * 1.6, h), egui::Sense::click());

    let fg = if enabled {
        visuals.strong_text_color()
    } else {
        visuals.widgets.inactive.fg_stroke.color
    };
    let (bg, stroke) = if !enabled {
        (egui::Color32::TRANSPARENT, egui::Stroke::NONE)
    } else if resp.is_pointer_button_down_on() {
        (
            visuals.widgets.active.bg_fill,
            visuals.widgets.active.bg_stroke,
        )
    } else if resp.hovered() {
        (
            visuals.widgets.hovered.bg_fill,
            visuals.widgets.hovered.bg_stroke,
        )
    } else {
        (
            visuals.widgets.inactive.bg_fill,
            visuals.widgets.inactive.bg_stroke,
        )
    };
    let rounding = visuals.widgets.inactive.rounding;
    ui.painter().rect_filled(rect, rounding, bg);
    if stroke.width > 0.0 {
        ui.painter().rect_stroke(rect, rounding, stroke);
    }
    draw_icon(ui.painter(), kind, rect.shrink(4.0), fg);

    resp = resp.on_hover_text(tip);
    resp.on_disabled_hover_text(format!("{} (nothing selected)", tip))
        .clicked()
}

/// Paint one toolbar glyph inside `r` using simple shapes.
fn draw_icon(p: &egui::Painter, kind: IconKind, r: egui::Rect, c: egui::Color32) {
    let st = egui::Stroke::new(1.5_f32, c);
    match kind {
        // Magnifier (inspect / view).
        IconKind::View => {
            let cc = egui::pos2(r.left() + r.width() * 0.38, r.top() + r.height() * 0.38);
            let rad = r.width() * 0.32;
            p.circle_stroke(cc, rad, st);
            p.line_segment(
                [
                    egui::pos2(cc.x + rad * 0.72, cc.y + rad * 0.72),
                    egui::pos2(r.right() - 0.5, r.bottom() - 0.5),
                ],
                st,
            );
        }
        // Pencil.
        IconKind::Edit => {
            let a = egui::pos2(r.left() + 1.0, r.bottom() - 3.0);
            let b = egui::pos2(r.right() - 3.0, r.top() + 1.0);
            p.line_segment([a, b], egui::Stroke::new(2.0_f32, c));
            p.add(egui::Shape::convex_polygon(
                vec![
                    a,
                    egui::pos2(a.x + 3.0, r.bottom() - 0.5),
                    egui::pos2(r.left() + 0.5, r.bottom() - 0.5),
                ],
                c,
                egui::Stroke::NONE,
            ));
        }
        // Two stacked sheets.
        IconKind::Copy => {
            let w = r.width() * 0.55;
            let back = egui::Rect::from_min_size(
                egui::pos2(r.left(), r.top()),
                egui::vec2(w, w * 1.05),
            );
            let front = egui::Rect::from_min_size(
                egui::pos2(r.left() + w * 0.45, r.top() + w * 0.45),
                egui::vec2(w, w * 1.05),
            );
            p.rect_stroke(back, 1.5, st);
            p.rect_filled(front, 1.5, c);
        }
        // Box with arrow leaving to the right.
        IconKind::Move => {
            let bw = r.width() * 0.5;
            let bh = r.height() * 0.56;
            let b = egui::Rect::from_min_size(
                egui::pos2(r.left(), r.center().y - bh / 2.0),
                egui::vec2(bw, bh),
            );
            p.rect_stroke(b, 1.5, st);
            let y = b.center().y;
            p.line_segment([egui::pos2(b.center().x, y), egui::pos2(r.right(), y)], st);
            p.add(egui::Shape::convex_polygon(
                vec![
                    egui::pos2(r.right(), y),
                    egui::pos2(r.right() - 3.5, y - 2.5),
                    egui::pos2(r.right() - 3.5, y + 2.5),
                ],
                c,
                egui::Stroke::NONE,
            ));
        }
        // Folder outline with a plus sign.
        IconKind::NewFolder => {
            let fw = r.width() * 0.62;
            let fh = r.height() * 0.52;
            let x0 = r.left();
            let y0 = r.bottom() - fh;
            let tab_w = fw * 0.38;
            let tab_h = fh * 0.45;
            let mut pts = vec![
                egui::pos2(x0, r.bottom()),
                egui::pos2(x0, y0 - tab_h),
                egui::pos2(x0 + tab_w, y0 - tab_h),
                egui::pos2(x0 + tab_w + tab_h * 0.7, y0),
                egui::pos2(x0 + fw, y0),
                egui::pos2(x0 + fw, r.bottom()),
                egui::pos2(x0, r.bottom()),
            ];
            p.add(egui::Shape::line(pts, st));
            let px = r.right() - 1.5;
            let py = r.top() + 2.0;
            let s = r.width() * 0.16;
            p.line_segment([egui::pos2(px - s, py), egui::pos2(px + s, py)], st);
            p.line_segment([egui::pos2(px, py - s), egui::pos2(px, py + s)], st);
        }
        // I-beam text cursor.
        IconKind::Rename => {
            let cx = r.center().x;
            let top = r.top() + 1.0;
            let bot = r.bottom() - 1.0;
            p.line_segment([egui::pos2(cx, top), egui::pos2(cx, bot)], st);
            let s = r.width() * 0.2;
            for y in [top, bot] {
                p.line_segment([egui::pos2(cx - s, y), egui::pos2(cx + s, y)], st);
            }
        }
        // Trash can.
        IconKind::Delete => {
            let w = r.width() * 0.62;
            let body = egui::Rect::from_min_size(
                egui::pos2(r.center().x - w / 2.0, r.top() + r.height() * 0.32),
                egui::vec2(w, r.height() * 0.6),
            );
            p.rect_stroke(body, 1.5, st);
            p.rect_filled(
                egui::Rect::from_min_size(
                    egui::pos2(body.left() - 2.0, r.top() + r.height() * 0.22),
                    egui::vec2(w + 4.0, 1.5),
                ),
                0.5,
                c,
            );
            p.rect_stroke(
                egui::Rect::from_min_size(
                    egui::pos2(r.center().x - w * 0.18, r.top()),
                    egui::vec2(w * 0.36, r.height() * 0.24),
                ),
                1.0,
                st,
            );
            for dx in [-w * 0.16_f32, w * 0.16] {
                let x = body.center().x + dx;
                p.line_segment(
                    [
                        egui::pos2(x, body.top() + 2.0),
                        egui::pos2(x, body.bottom() - 2.0),
                    ],
                    st,
                );
            }
        }
        // Box with an arrow escaping to the top-right.
        IconKind::OpenExternal => {
            let w = r.width() * 0.55;
            let bx = egui::Rect::from_min_size(
                egui::pos2(r.left(), r.bottom() - w),
                egui::vec2(w, w),
            );
            let pts = vec![
                egui::pos2(bx.right(), bx.bottom()),
                egui::pos2(bx.left(), bx.bottom()),
                egui::pos2(bx.left(), bx.top()),
                egui::pos2(bx.center().x, bx.top()),
            ];
            p.add(egui::Shape::line(pts, st));
            let from = egui::pos2(bx.center().x, bx.center().y);
            let to = egui::pos2(r.right() - 1.0, r.top() + 1.0);
            p.line_segment([from, to], st);
            p.add(egui::Shape::convex_polygon(
                vec![
                    to,
                    egui::pos2(to.x - 4.0, to.y + 0.5),
                    egui::pos2(to.x - 0.5, to.y + 4.0),
                ],
                c,
                egui::Stroke::NONE,
            ));
        }
    }
}

/// Persisted preferences, loaded at startup and written on shutdown.
#[derive(serde::Serialize, serde::Deserialize)]
struct Prefs {
    dark: bool,
    show_hidden: bool,
}

impl Default for Prefs {
    fn default() -> Self {
        Self {
            dark: true,
            show_hidden: false,
        }
    }
}
