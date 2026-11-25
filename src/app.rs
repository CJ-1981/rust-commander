use std::cell::{Cell, RefCell};
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::time::Instant;

use eframe::egui;
use egui_extras::{Column, TableBuilder};

use crate::jobs::{await_decision, Job, JobKind, Outcome, JobState};
use crate::ops;
use crate::panel::{display_name, fmt_size, fmt_time, list_roots, FileEntry, FilePanel, Pane, SortKey};
use crate::tools::{ToolTab, Toolbox};
use crate::viewer::Viewer;

/// Modal dialogs (copy/move destination, mkdir/rename input, delete confirm).
enum Modal {
    Copy { target: String, count: usize },
    Move { target: String, count: usize },
    Mkdir { name: String },
    Rename { path: PathBuf, name: String },
    Delete { count: usize, preview: Vec<String>, permanent: bool },
    /// Rename a folder tab of pane `pane_idx` (empty name = folder name).
    RenameTab { pane_idx: usize, tab_idx: usize, name: String },
    /// Create an empty file in the active pane.
    NewFile { name: String },
    /// Pin a folder to the Favorites menu.
    AddBookmark { path: PathBuf, name: String },
    /// A transfer hit an existing target; ask what to do.
    Overwrite { job_id: u64, path: String, is_dir: bool },
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
    DeletePermanent(PathBuf),
}

/// Which sort header was clicked inside a table body render pass.
#[derive(Clone, Copy)]
struct SortClick {
    key: SortKey,
    flip: bool,
}

/// A mouse press that began on a file row and is still held. Inside its own
/// pane it keeps range-selecting rows; once the pointer enters the other pane
/// it turns into a copy/move of `items` (see `settle_row_drag`).
struct RowDrag {
    from: usize,
    /// The pointer has entered the other pane: this is a drop now.
    committed: bool,
    items: Vec<FileEntry>,
}

/// The row under the pointer in the drop pane during the current frame.
struct DropRow {
    pane: usize,
    path: PathBuf,
    is_dir: bool,
    rect: egui::Rect,
}

/// How many frames egui's widget focus is handed back after a Tab. The
/// focus it moves can land one frame later.
const TAB_RELEASE_FRAMES: u8 = 3;

/// Size the Copy / Move menu is drawn at (used to keep it inside the window).
const DROP_MENU_WIDTH: f32 = 320.0;
const DROP_MENU_HEIGHT: f32 = 190.0;

/// Copy / Move chooser shown where a drag (or an OS file drop) was released.
struct DropMenu {
    items: Vec<FileEntry>,
    target: PathBuf,
    pos: egui::Pos2,
    /// Input time (seconds) the menu opened at; clicks right at that moment
    /// (the release that ended the drag) don't close it.
    opened: f64,
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
    ("Shift + Up / Down", "Extend the selection"),
    ("Home / End", "First / last entry"),
    ("Shift + Home / End", "Extend the selection to the first / last entry"),
    ("Enter", "Open folder / view file"),
    ("Backspace", "Go to parent folder"),
    ("Insert / Space", "Toggle selection at cursor"),
    ("Ctrl+Click", "Add to / remove from selection"),
    ("Shift+Click", "Range selection"),
    ("Press + Drag", "Range selection with the mouse"),
    ("Drag to other pane", "Copy / move the dragged rows (choose on drop)"),
    ("Double-click", "Enter folder / view file in the built-in viewer"),
    ("F3", "View file (read-only)"),
    ("F4", "Edit file"),
    ("F5", "Copy selection to opposite panel"),
    ("F6", "Move selection to opposite panel"),
    ("Shift+F6", "Rename current entry"),
    ("F7", "New folder"),
    ("F8 / Delete", "Move selection to Trash"),
    ("Ctrl+S", "Save file in the editor"),
    ("Tab (in the editor)", "Indent the line or selection (Shift+Tab removes the indentation)"),
    ("Ctrl+T", "New folder tab"),
    ("Ctrl+W", "Close folder tab"),
    ("Ctrl+N", "New empty file"),
    ("Ctrl+F", "Focus the folder filter"),
    ("Alt+Left / Right", "Back / forward in folder history"),
    ("Esc", "Close dialog"),
];

pub struct CommanderApp {
    panes: [Pane; 2],
    active: usize,
    dark: bool,
    /// Width of the left pane (the divider position); `None` = half/half.
    pane_width: Option<f32>,
    /// Recursive folder sizes, computed on demand (path -> (bytes, items)).
    dir_sizes: std::collections::HashMap<String, (u64, u64)>,
    /// Pinned folders shown in each pane's Favorites menu.
    bookmarks: Vec<Bookmark>,
    /// Focus the active pane's filter box on the next frame (Ctrl+F).
    focus_filter: bool,
    /// Row press-drag in progress.
    row_drag: Option<RowDrag>,
    /// Frames left in which to hand egui's widget focus back after a Tab.
    tab_release: u8,
    drop_menu: Option<DropMenu>,
    /// Screen rects of the two panes, updated every frame.
    pane_rects: [egui::Rect; 2],
    drop_row: Option<DropRow>,
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
        // Hangul fallback: egui's bundled fonts have no Korean coverage, so
        // CJK file names rendered as tofu boxes. Noto Sans KR (SIL OFL) is
        // appended last so it only serves glyphs the default fonts lack.
        let mut fonts = egui::FontDefinitions::default();
        fonts.font_data.insert(
            "noto_sans_kr".to_owned(),
            std::sync::Arc::new(egui::FontData::from_static(include_bytes!(
                "../resources/fonts/NotoSansKR.ttf"
            ))),
        );
        for family in [egui::FontFamily::Proportional, egui::FontFamily::Monospace] {
            fonts
                .families
                .entry(family)
                .or_default()
                .push("noto_sans_kr".to_owned());
        }
        cc.egui_ctx.set_fonts(fonts);
        let start = start_dir();
        let mut app = Self {
            panes: [Pane::new(start.clone()), Pane::new(start)],
            active: 0,
            dark: prefs.dark,
            pane_width: prefs.pane_width,
            dir_sizes: std::collections::HashMap::new(),
            bookmarks: prefs.bookmarks,
            focus_filter: false,
            row_drag: None,
            tab_release: 0,
            drop_menu: None,
            pane_rects: [egui::Rect::NOTHING; 2],
            drop_row: None,
            jobs: Vec::new(),
            modal: None,
            toolbox: Toolbox::default(),
            viewer: None,
            show_help: false,
            toasts: Vec::new(),
        };
        if !prefs.tabs.is_empty() {
            for (pane, saved) in app.panes.iter_mut().zip(&prefs.tabs) {
                if saved.is_empty() {
                    continue;
                }
                pane.tabs = saved
                    .iter()
                    .map(|t| {
                        let mut fp = FilePanel::new(PathBuf::from(&t.path));
                        fp.name = t.name.clone();
                        fp
                    })
                    .collect();
                pane.active = pane.active.min(pane.tabs.len() - 1);
            }
        }
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
        for pane in &mut self.panes {
            for tab in &mut pane.tabs {
                tab.reload();
            }
        }
    }

    fn set_hidden(&mut self, show: bool) {
        let sizes = self.dir_sizes.clone();
        for pane in &mut self.panes {
            for tab in &mut pane.tabs {
                tab.show_hidden = show;
                tab.apply_filter_sort_with(&sizes);
            }
        }
    }

    fn current_entry(&self, panel_idx: usize) -> Option<FileEntry> {
        let p = self.panes[panel_idx].cur_ref();
        p.visible_entry(p.cursor).cloned()
    }

    fn selected_entries(&self, panel_idx: usize) -> Vec<FileEntry> {
        let p = self.panes[panel_idx].cur_ref();
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

    /// A plain file to prefill the compare tool with: the first selected
    /// file, else the cursor entry if it is a file, else the first visible
    /// file in the listing.
    fn first_file_entry(&self, pane_idx: usize) -> Option<FileEntry> {
        let p = self.panes[pane_idx].cur_ref();
        for &vi in &p.visible {
            let e = &p.entries[vi];
            if !e.is_dir && p.selected.contains(&e.path) {
                return Some(e.clone());
            }
        }
        if let Some(e) = p.visible_entry(p.cursor).filter(|e| !e.is_dir) {
            return Some(e.clone());
        }
        p.visible
            .iter()
            .map(|&vi| &p.entries[vi])
            .find(|e| !e.is_dir)
            .cloned()
    }

    /// Point the operation target at `path`: if the panel's selection already
    /// contains it the selection stands, otherwise the selection is reduced to
    /// that single entry (used by row context-menu actions).
    fn focus_op_selection(&mut self, panel_idx: usize, path: &PathBuf) {
        let p = self.panes[panel_idx].cur();
        if !p.selected.contains(path) {
            p.selected.clear();
            p.selected.insert(path.clone());
            if let Some(vi) = p
                .visible
                .iter()
                .position(|&vi| &p.entries[vi].path == path)
            {
                p.cursor = vi;
                p.anchor = Some(vi);
                p.scroll_needed = true;
            }
        }
    }

    /// Switch the active pane and bring its cursor into view.
    fn switch_pane(&mut self) {
        self.active = 1 - self.active;
        self.panes[self.active].cur().scroll_needed = true;
    }

    fn toggle_select_cursor(&mut self) {
        if let Some(e) = self.current_entry(self.active) {
            let p = self.panes[self.active].cur();
            if p.selected.contains(&e.path) {
                p.selected.remove(&e.path);
            } else {
                p.selected.insert(e.path);
            }
        }
    }

    fn nav_parent(&mut self) {
        if let Some(par) = self
            .panes[self.active].cur()
            .path
            .parent()
            .map(|p| p.to_path_buf())
        {
            self.panes[self.active].cur().set_path(par);
        }
    }

    /// Refuse to swap the viewer while it has unsaved edits.
    fn viewer_busy_with_edits(&self) -> bool {
        matches!(&self.viewer, Some(v) if v.dirty)
    }

    fn enter_current(&mut self) {
        if let Some(e) = self.current_entry(self.active) {
            if e.is_dir {
                self.panes[self.active].cur().set_path(e.path);
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
        let target = crate::panel::nfc(&self.panes[1 - self.active].cur().path.display().to_string());
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
        let target = crate::panel::nfc(&self.panes[1 - self.active].cur().path.display().to_string());
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
        self.open_delete_modal_with(false);
    }

    /// Delete dialog; `permanent` skips the trash entirely.
    fn open_delete_modal_with(&mut self, permanent: bool) {
        let items = self.op_items();
        if items.is_empty() {
            return;
        }
        let preview: Vec<String> = items.iter().take(6).map(|e| e.name.clone()).collect();
        self.modal = Some(Modal::Delete {
            count: items.len(),
            preview,
            permanent,
        });
    }

    fn confirm_modal(&mut self) {
        let modal = self.modal.take();
        match modal {
            Some(Modal::Copy { target, .. }) => self.start_transfer(JobKind::Copy, target),
            Some(Modal::Move { target, .. }) => self.start_transfer(JobKind::Move, target),
            Some(Modal::Mkdir { name }) => self.do_mkdir(name),
            Some(Modal::Rename { path, name }) => self.do_rename(path, name),
            Some(Modal::Delete { permanent, .. }) => self.start_delete(permanent),
            Some(Modal::NewFile { name }) => self.do_new_file(name),
            // Overwrite prompts resolve directly in show_modal.
            Some(Modal::Overwrite { .. }) => {}
            Some(Modal::AddBookmark { path, name }) => {
                let name = name.trim().to_string();
                let name = if name.is_empty() {
                    crate::panel::display_name(&path)
                } else {
                    name
                };
                self.bookmarks.push(Bookmark {
                    name,
                    path: path.display().to_string(),
                });
                self.toast("Added to Favorites", false);
            }
            Some(Modal::RenameTab {
                pane_idx,
                tab_idx,
                name,
            }) => {
                let name = name.trim().to_string();
                self.panes[pane_idx].tabs[tab_idx].name =
                    if name.is_empty() { None } else { Some(name) };
            }
            Some(Modal::DiscardViewer) => self.viewer = None,
            None => {}
        }
    }

    fn start_transfer(&mut self, kind: JobKind, target: String) {
        let items = self.op_items();
        self.start_transfer_items(kind, items, PathBuf::from(target.trim()));
    }

    /// Copy or move `items` (the selection, a drag, or dropped files) into
    /// the folder `dst`. Entries already in `dst` are left alone.
    fn start_transfer_items(&mut self, kind: JobKind, items: Vec<FileEntry>, dst: PathBuf) {
        if items.is_empty() {
            self.toast("Nothing selected", true);
            return;
        }
        if !dst.is_dir() {
            self.toast("Target folder does not exist", true);
            return;
        }
        let in_target = |e: &FileEntry| e.path.parent().is_some_and(|p| same_folder(p, &dst));
        let items: Vec<FileEntry> = items.into_iter().filter(|e| !in_target(e)).collect();
        if items.is_empty() {
            self.toast("Those are already in this folder", true);
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
                let mut copied = 0u64;
                let mut skipped = 0u64;
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
                    // Existing target? Ask the user (the UI answers via the
                    // shared state; "all" answers repeat automatically).
                    let ask = |target: &Path, is_dir: bool| {
                        {
                            let mut s = state.lock().unwrap();
                            s.conflict = Some(crate::jobs::ConflictInfo {
                                path: target.display().to_string(),
                                is_dir,
                            });
                        }
                        await_decision(state, cancel)
                            .map(|d| match d {
                                crate::jobs::Decision::Overwrite => ops::Overwrite::Yes,
                                crate::jobs::Decision::Skip => ops::Overwrite::No,
                            })
                            .unwrap_or(ops::Overwrite::No) // cancelled = skip
                    };
                    let r = if kind == JobKind::Copy {
                        ops::copy_path(&item.path, &dst, ask)
                    } else {
                        ops::move_path(&item.path, &dst, ask)
                    };
                    match r {
                        Err(e) => {
                            if let Ok(mut s) = state.lock() {
                                s.error = Some(format!("'{}': {}", names[i], e));
                            }
                            break;
                        }
                        Ok(out) => match out {
                            ops::TransferOutcome::Copied => copied += 1,
                            ops::TransferOutcome::Skipped => skipped += 1,
                        },
                    }
                    if let Ok(mut s) = state.lock() {
                        s.done = i as u64 + 1;
                    }
                }
                if let Ok(mut s) = state.lock() {
                    s.current = format!("{} copied, {} skipped", copied, skipped);
                    s.finished = true;
                }
            },
        );
        self.jobs.push(job);
    }

    fn start_delete(&mut self, permanent: bool) {
        let items = self.op_items();
        if items.is_empty() {
            return;
        }
        let total = items.len() as u64;
        let names: Vec<String> = items.iter().map(|e| e.name.clone()).collect();
        let verb = if permanent { "Delete" } else { "Trash" };
        let job = Job::spawn(
            JobKind::Delete,
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
                    let r = if permanent {
                        ops::remove_path(&item.path)
                    } else {
                        ops::trash_path(&item.path)
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

    fn do_mkdir(&mut self, name: String) {
        let name = name.trim().to_string();
        if name.is_empty() {
            self.toast("Folder name is empty", true);
            return;
        }
        let path = self.panes[self.active].cur().path.join(&name);
        match std::fs::create_dir(&path) {
            Ok(()) => {
                self.reload_both();
                self.toast(format!("Created folder '{}'", name), false);
            }
            Err(e) => self.toast(format!("Create folder failed: {}", e), true),
        }
    }

    /// Create an empty file; refuses to clobber an existing one.
    fn do_new_file(&mut self, name: String) {
        let name = name.trim().to_string();
        if name.is_empty() {
            self.toast("File name is empty", true);
            return;
        }
        let path = self.panes[self.active].cur().path.join(&name);
        match std::fs::File::create_new(&path) {
            Ok(_) => {
                self.reload_both();
                self.toast(format!("Created file '{}'", name), false);
            }
            Err(e) => self.toast(format!("Create file failed: {}", e), true),
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
                    JobKind::Compare => self.toolbox.compare.on_done(snap),
                    JobKind::Sizes => {
                        if let Some(Outcome::Sizes { sizes }) = &snap.outcome {
                            for s in sizes {
                                self.dir_sizes.insert(s.path.clone(), (s.bytes, s.items));
                            }
                            let n = sizes.len();
                            self.toast(format!("Sizes computed for {} folder(s)", n), false);
                        }
                    }
                }
            }
            self.jobs.retain(|j| !j.snapshot().finished);
        }
        // A transfer hit an existing target: surface the overwrite prompt
        // (one at a time; Esc answers Skip).
        if self.modal.is_none() {
            let pending = self.jobs.iter().find_map(|j| {
                j.snapshot()
                    .conflict
                    .map(|c| (j.id, c))
            });
            if let Some((job_id, c)) = pending {
                self.modal = Some(Modal::Overwrite {
                    job_id,
                    path: c.path,
                    is_dir: c.is_dir,
                });
                ctx.request_repaint();
            }
        }
        if any_running {
            ctx.request_repaint_after(std::time::Duration::from_millis(100));
        }
    }

    /// Compute recursive sizes for the given folders in the background;
    /// results land in `self.dir_sizes` and also cover all subfolders.
    fn start_sizes_job(&mut self, dirs: Vec<PathBuf>) {
        let pending: Vec<String> = dirs
            .into_iter()
            .filter(|d| !self.dir_sizes.contains_key(&d.display().to_string()))
            .map(|d| d.display().to_string())
            .collect();
        if pending.is_empty() {
            self.toast("Folder sizes are already known", false);
            return;
        }
        let job = Job::spawn(
            JobKind::Sizes,
            format!("Sizes of {} folder(s)", pending.len()),
            move |state, cancel| {
                if let Ok(mut s) = state.lock() {
                    s.total = pending.len() as u64;
                }
                let mut out: Vec<crate::jobs::DirSize> = Vec::new();
                for (i, dir) in pending.iter().enumerate() {
                    if cancel.load(Ordering::Relaxed) {
                        break;
                    }
                    if let Ok(mut s) = state.lock() {
                        s.current = format!("scanning {}", dir);
                        s.done = i as u64;
                    }
                    // Iterative walk; sizes of every subfolder are reported too.
                    let mut stack = vec![PathBuf::from(dir)];
                    let (mut bytes, mut items) = (0u64, 0u64);
                    while let Some(d) = stack.pop() {
                        if cancel.load(Ordering::Relaxed) {
                            break;
                        }
                        let rd = match std::fs::read_dir(&d) {
                            Ok(rd) => rd,
                            Err(_) => continue,
                        };
                        for e in rd.flatten() {
                            items += 1;
                            match e.file_type() {
                                Ok(ft) if ft.is_dir() && !ft.is_symlink() => {
                                    stack.push(e.path());
                                }
                                Ok(ft) if ft.is_file() => {
                                    if let Ok(meta) = e.metadata() {
                                        bytes += meta.len();
                                    }
                                }
                                _ => {}
                            }
                        }
                    }
                    out.push(crate::jobs::DirSize {
                        path: dir.clone(),
                        bytes,
                        items,
                    });
                }
                let mut s = state.lock().unwrap();
                s.done = out.len() as u64;
                s.finished = true;
                s.outcome = Some(Outcome::Sizes { sizes: out });
            },
        );
        self.jobs.push(job);
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
        // The drop menu behaves like a dialog: keys must not reach the file
        // panels behind it (Enter would open a file).
        let modal_open = self.modal.is_some() || self.drop_menu.is_some();

        // Tab belongs to the panels, even while a text field is being edited:
        // it switches the active pane and leaves the field. egui has its own
        // idea of Tab (walk the keyboard focus through the widgets, starting
        // with the toolbar), so its focus is handed back at the end of the
        // frame. Two places keep the usual meaning: in the editor Tab indents,
        // and inside a dialog it moves between the dialog's controls.
        let editor_focused = self.viewer.as_ref().is_some_and(|v| v.editing)
            && ctx.memory(|m| m.has_focus(crate::viewer::editor_id()));
        if !modal_open && !editor_focused && ctx.input(|i| i.key_pressed(egui::Key::Tab)) {
            self.tab_release = TAB_RELEASE_FRAMES;
            if ctx.input(|i| !i.modifiers.ctrl && !i.modifiers.alt) {
                self.switch_pane();
            }
        }

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
            // Folder tab shortcuts.
            let (ctrl, key_t, key_w) = ctx.input(|i| {
                (
                    i.modifiers.ctrl,
                    i.key_pressed(egui::Key::T),
                    i.key_pressed(egui::Key::W),
                )
            });
            if ctrl && key_t {
                let path = self.panes[self.active].cur().path.clone();
                self.panes[self.active].open_tab(path);
            }
            if ctrl && key_w {
                self.panes[self.active].close_tab(self.panes[self.active].active);
            }

            // Back/forward through folder history.
            let (alt, key_left, key_right) = ctx.input(|i| {
                (
                    i.modifiers.alt,
                    i.key_pressed(egui::Key::ArrowLeft),
                    i.key_pressed(egui::Key::ArrowRight),
                )
            });
            if alt && key_left && !self.panes[self.active].cur().go_back() {
                self.toast("No earlier folder in history", true);
            }
            if alt && key_right && !self.panes[self.active].cur().go_forward() {
                self.toast("Nothing to go forward to", true);
            }

            if ctrl && ctx.input(|i| i.key_pressed(egui::Key::N)) {
                self.modal = Some(Modal::NewFile { name: String::new() });
            }
            if ctrl && ctx.input(|i| i.key_pressed(egui::Key::F)) {
                self.focus_filter = true;
            }

            let (up, down, enter, back, ins, space, del, home, end) = ctx.input(|i| (
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
            // Arrow keys move a single highlight; with Shift held they grow
            // (or shrink) a selection that starts where the cursor was.
            let extend = ctx.input(|i| i.modifiers.shift);
            if up {
                self.panes[self.active].cur().move_cursor(-1, extend);
            }
            if down {
                self.panes[self.active].cur().move_cursor(1, extend);
            }
            if home {
                self.panes[self.active].cur().go_to(0, extend);
            }
            if end {
                let len = self.panes[self.active].cur().visible.len();
                if len > 0 {
                    self.panes[self.active].cur().go_to(len - 1, extend);
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
                        ui.close_menu();
                        self.modal = Some(Modal::Mkdir { name: String::new() });
                    }
                    if ui.button("New file  (Ctrl+N)").clicked() {
                        ui.close_menu();
                        self.modal = Some(Modal::NewFile { name: String::new() });
                    }
                    if ui.button("Rename  (Shift+F6)").clicked() {
                        ui.close_menu();
                        self.open_rename_modal();
                    }
                    if ui.button("Delete  (F8)").clicked() {
                        ui.close_menu();
                        self.open_delete_modal();
                    }
                    ui.separator();
                    if ui.button("Open with system app").clicked() {
                        ui.close_menu();
                        self.open_external_current();
                    }
                    ui.separator();
                    if ui.button("Quit").clicked() {
                        ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                    }
                });

                ui.menu_button("Tools", |ui| {
                    if ui.button("Hash calculator").clicked() {
                        ui.close_menu();
                        self.toolbox.show = true;
                        self.toolbox.tab = ToolTab::Hash;
                    }
                    if ui.button("File search").clicked() {
                        ui.close_menu();
                        self.toolbox.show = true;
                        self.toolbox.tab = ToolTab::Search;
                        if self.toolbox.search.root.trim().is_empty() {
                            self.toolbox.search.root = self.panes[self.active].cur().path.display().to_string();
                        }
                    }
                    if ui.button("Duplicate finder").clicked() {
                        ui.close_menu();
                        self.toolbox.show = true;
                        self.toolbox.tab = ToolTab::Duplicates;
                        if self.toolbox.dup.root.trim().is_empty() {
                            self.toolbox.dup.root = self.panes[self.active].cur().path.display().to_string();
                        }
                    }
                    if ui.button("Multi-file rename").clicked() {
                        ui.close_menu();
                        self.toolbox.show = true;
                        self.toolbox.tab = ToolTab::Rename;
                        let items = self.op_items();
                        self.toolbox.rename.files =
                            items.iter().map(|e| e.path.clone()).collect();
                        self.toolbox.rename.last_result = None;
                    }
                    if ui.button("Compare files").clicked() {
                        ui.close_menu();
                        self.toolbox.show = true;
                        self.toolbox.tab = ToolTab::Compare;
                        self.toolbox.compare.left = self
                            .first_file_entry(0)
                            .map(|e| crate::panel::nfc(&e.path.display().to_string()))
                            .unwrap_or_default();
                        self.toolbox.compare.right = self
                            .first_file_entry(1)
                            .map(|e| crate::panel::nfc(&e.path.display().to_string()))
                            .unwrap_or_default();
                    }
                    if ui.button("System info").clicked() {
                        ui.close_menu();
                        self.toolbox.show = true;
                        self.toolbox.tab = ToolTab::System;
                    }
                });

                ui.menu_button("View", |ui| {
                    let mut hid = self.panes[0].cur_ref().show_hidden;
                    if ui.checkbox(&mut hid, "Show hidden files").changed() {
                        ui.close_menu();
                        self.set_hidden(hid);
                    }
                    let label = if self.dark {
                        "Switch to light theme"
                    } else {
                        "Switch to dark theme"
                    };
                    if ui.button(label).clicked() {
                        ui.close_menu();
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
                        ui.close_menu();
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
                    ICON_VIEW,
                    can_op,
                    "View file in built-in viewer (F3)",
                ) {
                    self.open_viewer(false);
                }
                if icon_button(ui, ICON_EDIT, can_op, "Edit file (F4)") {
                    self.open_viewer(true);
                }
                ui.separator();
                if icon_button(
                    ui,
                    ICON_COPY,
                    can_op,
                    "Copy selection to opposite panel (F5)",
                ) {
                    self.open_copy_modal();
                }
                if icon_button(
                    ui,
                    ICON_MOVE,
                    can_op,
                    "Move selection to opposite panel (F6)",
                ) {
                    self.open_move_modal();
                }
                ui.separator();
                if icon_button(
                    ui,
                    ICON_NEW_FOLDER,
                    !modal_open,
                    "New folder (F7)",
                ) {
                    self.modal = Some(Modal::Mkdir { name: String::new() });
                }
                if icon_button(ui, ICON_RENAME, can_op, "Rename (Shift+F6)") {
                    self.open_rename_modal();
                }
                if icon_button(
                    ui,
                    ICON_DELETE,
                    can_op,
                    "Move selection to Trash (F8)",
                ) {
                    self.open_delete_modal();
                }
                ui.separator();
                if icon_button(
                    ui,
                    ICON_OPEN_EXT,
                    can_op,
                    "Open selected file with system app",
                ) {
                    self.open_external_current();
                }
                ui.separator();
                if icon_button(ui, ICON_TOOLS, true, "Open the utility toolbox") {
                    self.toolbox.show = true;
                }

                // Version, right-aligned (also in the OS window title).
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.label(
                        egui::RichText::new(format!("v{}", env!("CARGO_PKG_VERSION")))
                            .weak()
                            .small(),
                    );
                });
            });
        });
    }

    /// Advance the row drag in progress, if any. Called once per frame, after
    /// both panes have been drawn: the drag is committed to a copy/move when
    /// the pointer enters the other pane, and the drop menu opens when it is
    /// released there.
    fn settle_row_drag(&mut self, ctx: &egui::Context) {
        let Some(from) = self.row_drag.as_ref().map(|d| d.from) else {
            return;
        };
        let held = ctx.input(|i| i.pointer.primary_down());
        let pos = ctx.input(|i| i.pointer.latest_pos());
        let over_other = pos
            .and_then(|p| self.pane_rects.iter().position(|r| r.contains(p)))
            .filter(|pane| *pane != from);

        if !held {
            let committed = self.row_drag.as_ref().is_some_and(|d| d.committed);
            if let (true, Some(pane)) = (committed, over_other) {
                if let Some(drag) = self.row_drag.take() {
                    self.finish_drop(ctx, drag.items, pane, pos);
                }
            }
            self.row_drag = None;
        } else if over_other.is_some() && !self.row_drag.as_ref().is_some_and(|d| d.committed) {
            self.commit_row_drag();
        }
    }

    /// Turn the row drag into a drop: the carried entries become the source
    /// pane's selection, and range selection pauses until the button is
    /// released.
    fn commit_row_drag(&mut self) {
        let Some(from) = self.row_drag.as_ref().map(|d| d.from) else {
            return;
        };
        let src = self.panes[from].cur();
        let items = if src.sweep_kept_base && src.drag_from.is_some() {
            // Started on a selected row: carry what was selected.
            src.entries_matching(&src.sweep_base)
        } else if let Some(e) = src
            .drag_from
            .or(src.press_row)
            .and_then(|vi| src.visible.get(vi))
            .map(|&ei| src.entries[ei].clone())
        {
            // Started on an unselected row: carry that row, unless a modifier
            // kept an existing selection containing it.
            let carried = src.selected.clone();
            if src.drag_from.is_none() && carried.contains(&e.path) {
                src.entries_matching(&carried)
            } else {
                vec![e]
            }
        } else {
            src.entries_matching(&src.selected)
        };
        src.selected = items.iter().map(|e| e.path.clone()).collect();
        src.drag_from = None;
        src.sweep_locked = true;
        if let Some(d) = &mut self.row_drag {
            d.committed = true;
            d.items = items;
        }
    }

    /// A drag (or an OS file drop) was released over pane `pane`. Shift means
    /// move and Ctrl / Command means copy; otherwise the menu asks.
    fn finish_drop(
        &mut self,
        ctx: &egui::Context,
        items: Vec<FileEntry>,
        pane: usize,
        pos: Option<egui::Pos2>,
    ) {
        if self.modal.is_some() {
            self.toast("Close the dialog before copying or moving files", true);
            return;
        }
        let target = match &self.drop_row {
            Some(row) if row.pane == pane && row.is_dir => row.path.clone(),
            _ => self.panes[pane].cur().path.clone(),
        };
        let mods = ctx.input(|i| i.modifiers);
        let direct = if mods.shift {
            Some(JobKind::Move)
        } else if mods.ctrl || mods.command {
            Some(JobKind::Copy)
        } else {
            None
        };
        match direct {
            Some(kind) => self.start_transfer_items(kind, items, target),
            None => {
                let screen = ctx.screen_rect();
                let pos = pos.unwrap_or_else(|| self.pane_rects[pane].center());
                // Open at the pointer, but not past the window's edge.
                let pos = egui::pos2(
                    pos.x.min(screen.right() - DROP_MENU_WIDTH - 8.0),
                    pos.y.min(screen.bottom() - DROP_MENU_HEIGHT - 8.0),
                );
                self.drop_menu = Some(DropMenu {
                    items,
                    target,
                    pos,
                    opened: ctx.input(|i| i.time),
                });
            }
        }
    }

    /// The Copy / Move menu shown at the pointer after a drop.
    fn show_drop_menu(&mut self, ctx: &egui::Context) {
        let Some(menu) = &self.drop_menu else {
            return;
        };
        let (items, target) = (menu.items.clone(), menu.target.clone());
        let mut action = None;
        let mut close = false;
        let area = egui::Area::new(egui::Id::new("drop_menu"))
            .order(egui::Order::Foreground)
            .fixed_pos(menu.pos)
            .show(ctx, |ui| {
                egui::Frame::popup(ui.style()).show(ui, |ui| {
                    ui.set_width(DROP_MENU_WIDTH);
                    ui.label(format!(
                        "{} item(s) dropped into '{}'",
                        items.len(),
                        display_name(&target)
                    ));
                    // Wrapped: an unwrapped path would make the menu huge.
                    ui.add(egui::Label::new(
                        egui::RichText::new(crate::panel::nfc(&target.display().to_string()))
                            .weak()
                            .small(),
                    ).wrap());
                    ui.separator();
                    if ui.button("Copy here").clicked() {
                        action = Some(JobKind::Copy);
                    }
                    if ui.button("Move here").clicked() {
                        action = Some(JobKind::Move);
                    }
                    ui.separator();
                    if ui.button("Cancel").clicked() {
                        close = true;
                    }
                    ui.add(
                        egui::Label::new(
                            egui::RichText::new(
                                "Hold Shift (move) or Ctrl / Command (copy) while dropping to skip this menu.",
                            )
                            .weak()
                            .small(),
                        )
                        .wrap(),
                    );
                });
            });

        let settled = ctx.input(|i| i.time) - menu.opened > 0.1;
        if ctx.input(|i| i.key_pressed(egui::Key::Escape))
            || (settled && area.response.clicked_elsewhere())
        {
            close = true;
        }
        if close || action.is_some() {
            self.drop_menu = None;
        }
        if let Some(kind) = action {
            self.start_transfer_items(kind, items, target);
        }
    }

    /// Files dropped from the OS (Finder, Explorer...) onto a pane: offer to
    /// copy or move them into that pane's folder.
    fn handle_os_drops(&mut self, ctx: &egui::Context) {
        let dropped = ctx.input(|i| i.raw.dropped_files.clone());
        if dropped.is_empty() {
            return;
        }
        let items: Vec<FileEntry> = dropped
            .into_iter()
            .filter_map(|f| f.path)
            .filter(|p| p.exists())
            .map(|p| FileEntry::from_path(&p))
            .collect();
        if items.is_empty() {
            return;
        }
        let pos = ctx.input(|i| i.pointer.latest_pos());
        let pane = pos
            .and_then(|p| self.pane_rects.iter().position(|r| r.contains(p)))
            .unwrap_or(self.active);
        self.finish_drop(ctx, items, pane, pos);
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
        // Both panes are drawn inside one CentralPanel with a hand-managed
        // split. egui's SidePanel resize state snapped back after dragging
        // (its stored rect is re-derived from the content rect each frame);
        // owning the width here is deterministic. Each pane gets an explicit
        // rectangle and a top-down layout: plain allocate_ui would inherit
        // the surrounding layout and flatten a pane into a single row.
        // Only rows hovered during this frame count as drop targets.
        self.drop_row = None;
        egui::CentralPanel::default().show(ctx, |ui| {
            let avail = ui.available_size();
            let default_w = (avail.x * 0.5).round();
            let mut width = self
                .pane_width
                .unwrap_or(default_w)
                .clamp(320.0, (avail.x - 320.0).max(360.0));

            let origin = ui.cursor().min;
            let pane0 = egui::Rect::from_min_size(origin, egui::vec2(width, avail.y));
            let divider = egui::Rect::from_min_size(
                egui::pos2(pane0.right(), origin.y),
                egui::vec2(10.0, avail.y),
            );
            let pane1 = egui::Rect::from_min_size(
                egui::pos2(divider.right(), origin.y),
                egui::vec2((avail.x - width - 10.0).max(200.0), avail.y),
            );
            // The whole allotted panes (not just their content), so a drag
            // counts as being over a pane anywhere inside it.
            self.pane_rects = [pane0, pane1];
            self.show_pane(ui, pane0, 0);

            // Divider: thin line with a generous grab area.
            let drect = divider;
            let dresp = ui.allocate_rect(drect, egui::Sense::drag());
            let accent = ui.visuals().selection.stroke.color;
            let (line_color, line_w) = if dresp.dragged() {
                (accent, 2.0_f32)
            } else if dresp.hovered() {
                (accent.gamma_multiply(0.7), 2.0)
            } else {
                (ui.visuals().widgets.noninteractive.bg_stroke.color, 1.0)
            };
            let x = drect.center().x;
            ui.painter().line_segment(
                [
                    egui::pos2(x, drect.top() + 6.0),
                    egui::pos2(x, drect.bottom() - 6.0),
                ],
                egui::Stroke::new(line_w, line_color),
            );
            if dresp.hovered() || dresp.dragged() {
                ui.ctx().set_cursor_icon(egui::CursorIcon::ResizeHorizontal);
            }
            if dresp.double_clicked() {
                width = default_w;
            }
            if dresp.dragged() {
                if let Some(p) = dresp.interact_pointer_pos() {
                    // Pane 0 starts where the divider region begins minus
                    // its current width.
                    let pane0_left = drect.left() - width;
                    width = (p.x - pane0_left)
                        .clamp(320.0, (avail.x - 320.0).max(360.0));
                }
            }

            self.show_pane(ui, pane1, 1);

            self.settle_row_drag(ui.ctx());
            self.pane_width = Some(width);
        });
    }


    /// Draw pane `idx` inside `rect`, claiming only the space the pane was
    /// allotted. The file table registers invisible column-resize handles just
    /// past its last column; counting those made every pane slightly wider
    /// than its share, which nudged the divider and squeezed the other pane.
    fn show_pane(&mut self, ui: &mut egui::Ui, rect: egui::Rect, idx: usize) {
        // Child carries an explicit id salt: unsalted child UIs all derive
        // the same id, which made the two panes' tables, scroll areas and
        // widgets collide (egui's "First/Second use of Table ID" paint).
        let mut pane = ui.new_child(
            egui::UiBuilder::new()
                .id_salt(if idx == 0 { "pane_left" } else { "pane_right" })
                .max_rect(rect)
                .layout(egui::Layout::top_down(egui::Align::Min)),
        );
        self.draw_panel(&mut pane, idx);
        let used = pane.min_rect().intersect(rect);
        ui.allocate_rect(used, egui::Sense::hover());
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
        // In-row navigation must wait for the table pass to end: set_path
        // replaces entries/visible, and the loop below indexes them with
        // bounds taken before the pass (panicked on folder double-click).
        let nav = RefCell::new(None::<PathBuf>);
        // Tab actions deferred out of the frame closure (rename needs
        // `self.modal`).
        let tab_rename = RefCell::new(None::<usize>);
        let term_dir = RefCell::new(None::<PathBuf>);
        let add_bm = Cell::new(false);
        let sizes_dirs = RefCell::new(None::<Vec<PathBuf>>);
        // Drag-and-drop scratch for this pane's table pass: the row under
        // the pointer while a drag from the other pane is over this pane, and
        // whether a press began on one of this pane's rows.
        let drop_here = RefCell::new(None::<(PathBuf, bool, egui::Rect)>);
        let drop_target = self.row_drag.as_ref().is_some_and(|d| d.from != idx);
        let drag_began = Cell::new(false);

        // The pane's box is the whole rectangle it was allotted: it keeps its
        // shape however short the listing or narrow the pane, and nothing in
        // the pane paints outside it. (Around the content, a narrow pane's box
        // stopped at the width of its toolbar.)
        let box_rect = ui.max_rect();
        let painter = ui.painter();
        painter.rect_filled(box_rect, 6.0, fill);
        painter.rect_stroke(box_rect.shrink(1.0), 6.0, stroke);
        ui.shrink_clip_rect(box_rect.expand(2.0));
        let mut content = ui.new_child(
            egui::UiBuilder::new()
                .id_salt(if idx == 0 {
                    "pane_left_content"
                } else {
                    "pane_right_content"
                })
                .max_rect(box_rect.shrink2(egui::vec2(8.0, 8.0)))
                .layout(egui::Layout::top_down(egui::Align::Min)),
        );
        ui.allocate_rect(box_rect, egui::Sense::hover());
        {
                let ui = &mut content;

                let pane = &mut self.panes[idx];

                // Folder tab strip.
                let mut new_tab = false;
                let mut close_tab: Option<usize> = None;
                ui.horizontal_wrapped(|ui| {
                    let labels: Vec<String> =
                        (0..pane.tabs.len()).map(|ti| pane.tab_label(ti)).collect();
                    for (ti, label) in labels.iter().enumerate() {
                        let r = ui.selectable_label(ti == pane.active, label.as_str())
                            .on_hover_text(match pane.tabs[ti].name.clone() {
                                Some(custom) => format!("{} ({})", label, custom),
                                None => {
                                    crate::panel::nfc(&pane.tabs[ti].path.display().to_string())
                                }
                            });
                        if r.clicked() {
                            let now = ui.input(|i| i.time);
                            pane.active = ti;
                            clicked.set(true);
                            // Only a second click on the same tab renames it.
                            if pane.tab_clicked_at(ti, now) {
                                tab_rename.borrow_mut().replace(ti);
                            }
                        }
                        if r.clicked_by(egui::PointerButton::Middle)
                            && pane.tabs.len() > 1
                        {
                            close_tab = Some(ti);
                        }
                        r.context_menu(|ui| {
                            clicked.set(true);
                            if ui.button("Rename tab…").clicked() {
                                tab_rename.borrow_mut().replace(ti);
                                ui.close_menu();
                            }
                            if pane.tabs.len() > 1 && ui.button("Close tab").clicked() {
                                close_tab = Some(ti);
                                ui.close_menu();
                            }
                        });
                    }
                    if ui
                        .small_button("+")
                        .on_hover_text("New tab at the current folder (Ctrl+T)")
                        .clicked()
                    {
                        new_tab = true;
                        clicked.set(true);
                    }
                });
                if new_tab {
                    let path = pane.cur().path.clone();
                    pane.open_tab(path);
                }
                if let Some(ti) = close_tab {
                    pane.close_tab(ti);
                }

                let p = pane.cur();

                // Editable path bar.
                ui.horizontal(|ui| {
                    let resp = field_edit(
                        ui,
                        (idx, "path"),
                        &mut p.path_edit,
                        ui.available_width() - 50.0,
                        "Enter a folder path...",
                    );
                    let go = ui.button("Go").clicked()
                        || (resp.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)));
                    if go {
                        let np = crate::panel::resolve_typed_path(p.path_edit.trim());
                        if np.is_dir() {
                            p.set_path(np);
                        } else {
                            p.path_edit = crate::panel::nfc(&p.path.display().to_string());
                            go_failed.set(true);
                        }
                    }
                });

                // Panel toolbar. It wraps, so a narrow pane keeps every
                // control reachable instead of pushing some out of the pane.
                ui.horizontal_wrapped(|ui| {
                    if ui
                        .add_enabled(!p.back.is_empty(), egui::Button::new("←"))
                        .on_hover_text("Back (Alt+←)")
                        .clicked()
                    {
                        p.go_back();
                    }
                    if ui
                        .add_enabled(!p.fwd.is_empty(), egui::Button::new("→"))
                        .on_hover_text("Forward (Alt+→)")
                        .clicked()
                    {
                        p.go_forward();
                    }
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
                            if ui.button(crate::panel::nfc(&root.display().to_string())).clicked() {
                                ui.close_menu();
                                p.set_path(root);
                            }
                        }
                    });
                    if ui.button("Refresh").clicked() {
                        p.reload();
                    }
                    if ui
                        .small_button(">_")
                        .on_hover_text("Open a terminal in this folder")
                        .clicked()
                    {
                        term_dir.replace(Some(p.path.clone()));
                    }
                    ui.menu_button("★", |ui| {
                        let bookmarks = self.bookmarks.clone();
                        for (i, bm) in bookmarks.iter().enumerate() {
                            ui.horizontal(|ui| {
                                if ui
                                    .small_button(crate::panel::nfc(&bm.name).as_str())
                                    .on_hover_text(crate::panel::nfc(&bm.path))
                                    .clicked()
                                {
                                    ui.close_menu();
                                    p.set_path(PathBuf::from(&bm.path));
                                }
                                if ui.small_button("✕").on_hover_text("Remove").clicked() {
                                    self.bookmarks.remove(i);
                                }
                            });
                        }
                        if !self.bookmarks.is_empty() {
                            ui.separator();
                        }
                        if ui.button("Add current folder…").clicked() {
                            ui.close_menu();
                            add_bm.set(true);
                        }
                    });
                    if ui
                        .small_button("Sizes")
                        .on_hover_text("Compute recursive sizes of the listed folders")
                        .clicked()
                    {
                        let dirs: Vec<PathBuf> = p
                            .visible
                            .iter()
                            .map(|&vi| p.entries[vi].path.clone())
                            .filter(|path| path.is_dir())
                            .collect();
                        sizes_dirs.replace(Some(dirs));
                    }
                    let want_filter_focus = self.focus_filter;
                    if want_filter_focus {
                        self.focus_filter = false;
                    }
                    let fresp = field_edit(ui, (idx, "filter"), &mut p.filter, 110.0, "Filter");
                    if want_filter_focus && idx == self.active {
                        fresp.request_focus();
                    }
                    if fresp.changed() {
                        p.apply_filter_sort_with(&self.dir_sizes);
                    }
                    if fresp.has_focus() && ui.input(|i| i.key_pressed(egui::Key::Escape)) {
                        p.filter.clear();
                        p.apply_filter_sort_with(&self.dir_sizes);
                    }
                    let mut hid = p.show_hidden;
                    if ui.checkbox(&mut hid, "Hidden").changed() {
                        p.show_hidden = hid;
                        p.apply_filter_sort_with(&self.dir_sizes);
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
                    // The name column takes whatever width is left, so the
                    // table always fits the pane and follows the divider. (A
                    // resizable remainder column keeps its first width, which
                    // left a gap in wide panes and pushed columns out of
                    // narrow ones.)
                    .column(Column::remainder().clip(true).resizable(false))
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
                        // Row count captured up-front: rows must never mutate
                        // the listing mid-pass (navigation is deferred instead).
                        let row_count = p.visible.len();
                        for vi in 0..row_count {
                            if vi >= p.visible.len() {
                                break;
                            }
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
                                // The cursor row of the inactive pane is
                                // shaded below instead of highlighted like a
                                // selection, so it doesn't read as one.
                                row.set_selected(is_sel || (is_cur && active));

                                // Name cell (primary interaction).
                                row.col(|ui| {
                                    cursor_shade(ui, is_cur && !is_sel && !active);
                                    // Keep the keyboard cursor visible while
                                    // navigating with the arrow keys — only
                                    // on cursor moves, so wheel scrolling is
                                    // never fought by the auto-scroll.
                                    if is_cur && p.scroll_needed {
                                        ui.scroll_to_cursor(None);
                                        p.scroll_needed = false;
                                    }
                                    let mut txt = egui::RichText::new(&name);
                                    txt = if is_dir {
                                        txt.strong().color(dir_color)
                                    } else {
                                        txt
                                    };
                                    let r = ui
                                        // On one line: a column sizes itself to its
                                        // content, so text that wrapped would leave
                                        // the column too narrow for it.
                                        .horizontal(|ui| ui.selectable_label(is_sel, txt))
                                        .inner
                                        // The rest of the cell is clickable too. It is
                                        // registered without claiming space: a full-width
                                        // widget would count as the column's used width,
                                        // so the column could never get narrower.
                                        .union(ui.interact(
                                            ui.max_rect(),
                                            ui.id().with("cell_hit"),
                                            egui::Sense::click(),
                                        ));
                                    cell_press(
                                        ui,
                                        p,
                                        vi,
                                        &path,
                                        is_dir,
                                        &r,
                                        self.row_drag.is_some(),
                                        drop_target,
                                        &drop_here,
                                        &drag_began,
                                    );
                                    if r.clicked() {
                                        clicked.set(true);
                                        let mods = ui.input(|i| i.modifiers);
                                        // Opening needs a second plain click on
                                        // this same row in quick succession.
                                        let opened = if mods.any() {
                                            p.last_click = None;
                                            false
                                        } else {
                                            p.clicked_at(&path, ui.input(|i| i.time))
                                        };
                                        if mods.ctrl {
                                            if is_sel {
                                                p.selected.remove(&path);
                                            } else {
                                                p.selected.insert(path.clone());
                                            }
                                            p.cursor = vi;
                                        } else if mods.shift {
                                            p.extend_to(vi);
                                        } else {
                                            p.select_only(vi);
                                        }
                                        if opened {
                                            if is_dir {
                                                *nav.borrow_mut() = Some(path.clone());
                                            } else {
                                                // Open the built-in viewer, not the
                                                // system app: launching another app
                                                // steals focus mid-interaction and
                                                // kills drag selection. External
                                                // open stays on the toolbar/context
                                                // menu (RowAction::View).
                                                row_action
                                                    .borrow_mut()
                                                    .replace(RowAction::View(path.clone()));
                                            }
                                        }
                                    }
                                    // Right-click menu (row actions needing
                                    // `self` are deferred via `row_action`).
                                    r.context_menu(|ui| {
                                        clicked.set(true);
                                        if ui.button(if is_dir { "Open" } else { "Open with system app" }).clicked() {
                                            if is_dir {
                                                *nav.borrow_mut() = Some(path.clone());
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
                                        if is_dir
                                            && ui.button("Calculate folder size").clicked()
                                        {
                                            sizes_dirs.replace(Some(vec![path.clone()]));
                                            ui.close_menu();
                                        }
                                        if ui.button("Move to Trash…").clicked() {
                                            row_action.borrow_mut().replace(RowAction::Delete(path.clone()));
                                            ui.close_menu();
                                        }
                                        if ui.button("Delete permanently…").clicked() {
                                            row_action.borrow_mut().replace(RowAction::DeletePermanent(path.clone()));
                                            ui.close_menu();
                                        }
                                        ui.separator();
                                        if ui.button("Copy name").clicked() {
                                            ui.ctx().copy_text(name.clone());
                                            ui.close_menu();
                                        }
                                        if ui.button("Copy full path").clicked() {
                                            ui.ctx()
                                                .copy_text(crate::panel::nfc(&path.display().to_string()));
                                            ui.close_menu();
                                        }
                                    });
                                });
                                row.col(|ui| {
                                    cursor_shade(ui, is_cur && !is_sel && !active);
                                    let size_txt = if is_dir {
                                        match self
                                            .dir_sizes
                                            .get(&path.display().to_string())
                                        {
                                            Some((bytes, items)) => {
                                                format!("{} · {} items", fmt_size(*bytes), items)
                                            }
                                            None => "—".to_string(),
                                        }
                                    } else {
                                        fmt_size(size)
                                    };
                                    let r = ui
                                        // On one line: a column sizes itself to its
                                        // content, so text that wrapped would leave
                                        // the column too narrow for it.
                                        .horizontal(|ui| ui.selectable_label(is_sel, size_txt))
                                        .inner
                                        // The rest of the cell is clickable too. It is
                                        // registered without claiming space: a full-width
                                        // widget would count as the column's used width,
                                        // so the column could never get narrower.
                                        .union(ui.interact(
                                            ui.max_rect(),
                                            ui.id().with("cell_hit"),
                                            egui::Sense::click(),
                                        ));
                                    cell_press(
                                        ui,
                                        p,
                                        vi,
                                        &path,
                                        is_dir,
                                        &r,
                                        self.row_drag.is_some(),
                                        drop_target,
                                        &drop_here,
                                        &drag_began,
                                    );
                                    if r.clicked() {
                                        clicked.set(true);
                                        p.select_only(vi);
                                    }
                                });
                                row.col(|ui| {
                                    cursor_shade(ui, is_cur && !is_sel && !active);
                                    let r = ui
                                        // On one line: a column sizes itself to its
                                        // content, so text that wrapped would leave
                                        // the column too narrow for it.
                                        .horizontal(|ui| ui.selectable_label(is_sel, fmt_time(modified)))
                                        .inner
                                        // The rest of the cell is clickable too. It is
                                        // registered without claiming space: a full-width
                                        // widget would count as the column's used width,
                                        // so the column could never get narrower.
                                        .union(ui.interact(
                                            ui.max_rect(),
                                            ui.id().with("cell_hit"),
                                            egui::Sense::click(),
                                        ));
                                    cell_press(
                                        ui,
                                        p,
                                        vi,
                                        &path,
                                        is_dir,
                                        &r,
                                        self.row_drag.is_some(),
                                        drop_target,
                                        &drop_here,
                                        &drag_began,
                                    );
                                    if r.clicked() {
                                        clicked.set(true);
                                        p.select_only(vi);
                                    }
                                });
                                row.col(|ui| {
                                    cursor_shade(ui, is_cur && !is_sel && !active);
                                    let t = if is_dir {
                                        "DIR".to_string()
                                    } else {
                                        ext.clone()
                                    };
                                    let r = ui
                                        // On one line: a column sizes itself to its
                                        // content, so text that wrapped would leave
                                        // the column too narrow for it.
                                        .horizontal(|ui| ui.selectable_label(is_sel, t))
                                        .inner
                                        // The rest of the cell is clickable too. It is
                                        // registered without claiming space: a full-width
                                        // widget would count as the column's used width,
                                        // so the column could never get narrower.
                                        .union(ui.interact(
                                            ui.max_rect(),
                                            ui.id().with("cell_hit"),
                                            egui::Sense::click(),
                                        ));
                                    cell_press(
                                        ui,
                                        p,
                                        vi,
                                        &path,
                                        is_dir,
                                        &r,
                                        self.row_drag.is_some(),
                                        drop_target,
                                        &drop_here,
                                        &drag_began,
                                    );
                                    if r.clicked() {
                                        clicked.set(true);
                                        p.select_only(vi);
                                    }
                                });
                            });
                        }

                        // Sweeping near the list's top or bottom edge scrolls
                        // it, so a drag can reach rows beyond the visible ones.
                        if p.drag_from.is_some() {
                            let list = body.ui_mut();
                            let clip = list.clip_rect();
                            if let Some(pos) = list.input(|i| i.pointer.latest_pos()) {
                                let near_edge = |edge: f32| (pos.y - edge).abs() <= 28.0;
                                let step = if near_edge(clip.bottom()) {
                                    -16.0
                                } else if near_edge(clip.top()) {
                                    16.0
                                } else {
                                    0.0
                                };
                                if step != 0.0 && pos.x >= clip.left() && pos.x <= clip.right() {
                                    list.scroll_with_delta_animation(
                                        egui::vec2(0.0, step),
                                        egui::style::ScrollAnimation::none(),
                                    );
                                    list.ctx().request_repaint();
                                }
                            }
                        }
                    });

                // A press that began on one of this pane's rows starts a row
                // drag (it becomes a copy/move if it reaches the other pane).
                if drag_began.get() && self.row_drag.is_none() {
                    self.row_drag = Some(RowDrag {
                        from: idx,
                        committed: false,
                        items: Vec::new(),
                    });
                }
                if let Some((path, is_dir, rect)) = drop_here.borrow_mut().take() {
                    self.drop_row = Some(DropRow {
                        pane: idx,
                        path,
                        is_dir,
                        rect,
                    });
                }

                // Press-drag multi-select ends when the button is released.
                if p.drag_from.is_some() && !ui.input(|i| i.pointer.primary_down()) {
                    p.drag_from = None;
                }

                if let Some(sc) = sort_click.get() {
                    if sc.flip {
                        p.ascending = !p.ascending;
                    } else {
                        p.sort = sc.key;
                        p.ascending = true;
                    }
                    p.apply_filter_sort_with(&self.dir_sizes);
                }
                if let Some(path) = nav.borrow_mut().take() {
                    p.set_path(path);
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
        }

        // Drop highlight: a drag from the other pane is over this pane.
        if self
            .row_drag
            .as_ref()
            .is_some_and(|d| d.committed && d.from != idx)
            && ui.rect_contains_pointer(ui.max_rect())
        {
            let rect = ui.max_rect();
            let accent = ui.visuals().selection.stroke.color;
            ui.painter().rect_filled(rect, 6.0, accent.gamma_multiply(0.10));
            ui.painter()
                .rect_stroke(rect, 6.0, egui::Stroke::new(3.0_f32, accent));

            // A folder row under the pointer is the destination itself.
            let into = match &self.drop_row {
                Some(row) if row.pane == idx && row.is_dir => {
                    ui.painter()
                        .rect_stroke(row.rect, 3.0, egui::Stroke::new(2.0_f32, accent));
                    Some(row.path.clone())
                }
                _ => None,
            };
            let count = self.row_drag.as_ref().map_or(0, |d| d.items.len());
            let caption = match into {
                Some(folder) => format!(
                    "Drop to copy or move {} item(s) into '{}'",
                    count,
                    display_name(&folder)
                ),
                None => format!("Drop to copy or move {} item(s)", count),
            };
            let text_color = ui.visuals().strong_text_color();
            let galley = ui
                .painter()
                .layout(
                    caption,
                    egui::FontId::proportional(15.0),
                    text_color,
                    f32::INFINITY,
                );
            let box_rect = egui::Rect::from_center_size(
                rect.center(),
                galley.size() + 2.0 * egui::vec2(14.0, 10.0),
            );
            ui.painter()
                .rect_filled(box_rect, 6.0, ui.visuals().window_fill());
            ui.painter()
                .rect_stroke(box_rect, 6.0, egui::Stroke::new(1.0_f32, accent));
            ui.painter()
                .galley(box_rect.center() - galley.size() / 2.0, galley, text_color);
        }

        // Apply deferred actions collected while drawing (they need `self`,
        // which was borrowed by the frame closures above).
        if let Some(dir) = term_dir.borrow_mut().take() {
            match ops::open_terminal(&dir) {
                Ok(()) => self.toast("Opening terminal", false),
                Err(e) => self.toast(format!("Could not open terminal: {}", e), true),
            }
        }
        if let Some(dirs) = sizes_dirs.borrow_mut().take() {
            self.start_sizes_job(dirs);
            // Sorting by Size can now use the new numbers.
            if self.panes[idx].cur_ref().sort == SortKey::Size {
                let sizes = self.dir_sizes.clone();
                self.panes[idx].cur().apply_filter_sort_with(&sizes);
            }
        }
        if add_bm.get() {
            let path = self.panes[idx].cur().path.clone();
            self.modal = Some(Modal::AddBookmark {
                path: path.clone(),
                name: display_name(&path),
            });
        }
        if let Some(ti) = tab_rename.borrow_mut().take() {
            let name = self.panes[idx].tab_label(ti);
            self.modal = Some(Modal::RenameTab {
                pane_idx: idx,
                tab_idx: ti,
                name,
            });
        }
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
                    let name = self.panes[idx]
                        .cur()
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
                    self.open_delete_modal_with(false);
                }
                RowAction::DeletePermanent(path) => {
                    self.active = idx;
                    self.focus_op_selection(idx, &path);
                    self.open_delete_modal_with(true);
                }
            }
        }
    }

    fn show_modal(&mut self, ctx: &egui::Context) {
        if self.modal.is_none() {
            return;
        }
        if ctx.input(|i| i.key_pressed(egui::Key::Escape)) {
            if let Some(Modal::Overwrite { job_id, .. }) = &self.modal {
                if let Some(j) = self.jobs.iter().find(|j| j.id == *job_id) {
                    j.resolve(crate::jobs::Decision::Skip, false);
                }
            }
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
                Modal::RenameTab { name, .. } => {
                    let mut open = true;
                    egui::Window::new("Rename tab")
                        .open(&mut open)
                        .collapsible(false)
                        .show(ctx, |ui| {
                            ui.label("Tab name (empty = folder name):");
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
                Modal::NewFile { name } => {
                    let mut open = true;
                    egui::Window::new("New file")
                        .open(&mut open)
                        .collapsible(false)
                        .show(ctx, |ui| {
                            ui.label("File name:");
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
                Modal::AddBookmark { path, name } => {
                    let mut open = true;
                    egui::Window::new("Add favorite")
                        .open(&mut open)
                        .collapsible(false)
                        .show(ctx, |ui| {
                            ui.label(format!("Folder: {}", path.display()));
                            ui.label("Name:");
                            let resp =
                                ui.add(egui::TextEdit::singleline(name).desired_width(320.0));
                            ui.add_space(6.0);
                            ui.horizontal(|ui| {
                                if ui.button("Add").clicked()
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
                Modal::Overwrite {
                    job_id,
                    path,
                    is_dir,
                } => {
                    let mut open = true;
                    let what = if *is_dir { "folder" } else { "file" };
                    egui::Window::new("Target already exists")
                        .open(&mut open)
                        .collapsible(false)
                        .show(ctx, |ui| {
                            ui.label(format!(
                                "A {} with this name already exists at the target:",
                                what
                            ));
                            ui.monospace(path);
                            ui.add_space(6.0);
                            ui.horizontal(|ui| {
                                if ui.button("Overwrite").clicked() {
                                    if let Some(j) =
                                        self.jobs.iter().find(|j| j.id == *job_id)
                                    {
                                        j.resolve(crate::jobs::Decision::Overwrite, false);
                                    }
                                    closed = true;
                                }
                                if ui.button("Skip").clicked() {
                                    if let Some(j) =
                                        self.jobs.iter().find(|j| j.id == *job_id)
                                    {
                                        j.resolve(crate::jobs::Decision::Skip, false);
                                    }
                                    closed = true;
                                }
                            });
                            ui.add_space(4.0);
                            ui.horizontal(|ui| {
                                if ui.button("Overwrite all").clicked() {
                                    if let Some(j) =
                                        self.jobs.iter().find(|j| j.id == *job_id)
                                    {
                                        j.resolve(crate::jobs::Decision::Overwrite, true);
                                    }
                                    closed = true;
                                }
                                if ui.button("Skip all").clicked() {
                                    if let Some(j) =
                                        self.jobs.iter().find(|j| j.id == *job_id)
                                    {
                                        j.resolve(crate::jobs::Decision::Skip, true);
                                    }
                                    closed = true;
                                }
                                if ui.button("Cancel job").clicked() {
                                    if let Some(j) =
                                        self.jobs.iter().find(|j| j.id == *job_id)
                                    {
                                        j.cancel();
                                        j.resolve(crate::jobs::Decision::Skip, true);
                                    }
                                    closed = true;
                                }
                            });
                        });
                    if !open {
                        closed = true;
                    }
                }
                Modal::Delete {
                    count,
                    preview,
                    permanent,
                } => {
                    let mut open = true;
                    let title = if *permanent {
                        "Delete permanently"
                    } else {
                        "Move to Trash"
                    };
                    let warning = if *permanent {
                        format!(
                            "Permanently delete {} item(s)? This cannot be undone.",
                            count
                        )
                    } else {
                        format!("Move {} item(s) to the Trash?", count)
                    };
                    egui::Window::new(title)
                        .open(&mut open)
                        .collapsible(false)
                        .show(ctx, |ui| {
                            ui.colored_label(ui.visuals().warn_fg_color, warning);
                            for name in preview {
                                ui.label(egui::RichText::new(format!("  {}", name)).small());
                            }
                            ui.add_space(6.0);
                            ui.horizontal(|ui| {
                                if *permanent {
                                    if ui.button("Delete permanently").clicked() {
                                        confirmed = true;
                                    }
                                } else {
                                    if ui.button("Move to Trash").clicked() {
                                        confirmed = true;
                                    }
                                    if ui.button("Delete permanently…").clicked() {
                                        *permanent = true;
                                        confirmed = true;
                                    }
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
        // out-parameter (they cannot reach the panels themselves); `changed`
        // is set when a tool has modified files on disk.
        let mut nav: Option<PathBuf> = None;
        let mut changed = false;
        egui::Window::new("Utility toolbox")
            .open(&mut open)
            .default_size([700.0, 500.0])
            .show(ctx, |ui| {
                self.toolbox.ui(ui, &mut self.jobs, &mut nav, &mut changed);
            });
        if changed {
            self.reload_both();
            self.toast("Renamed files - panels refreshed", false);
        }
        // The compare tool reads its two files from the pane selections; fill
        // any empty field so the tab is usable however it was opened.
        if self.toolbox.tab == ToolTab::Compare {
            if self.toolbox.compare.left.is_empty() {
                self.toolbox.compare.left = self
                    .first_file_entry(0)
                    .map(|e| crate::panel::nfc(&e.path.display().to_string()))
                    .unwrap_or_default();
            }
            if self.toolbox.compare.right.is_empty() {
                self.toolbox.compare.right = self
                    .first_file_entry(1)
                    .map(|e| crate::panel::nfc(&e.path.display().to_string()))
                    .unwrap_or_default();
            }
        }
        if let Some(target) = nav {
            let p = self.panes[self.active].cur();
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
                    p.anchor = Some(vi);
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
            // The window keeps one identity whatever its title: egui identifies
            // a window by its title, so the "* " marker appearing on the first
            // edit made this a new window and the editor lost the keyboard
            // focus after one typed character.
            egui::Window::new(title)
                .id(egui::Id::new("viewer_window"))
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
                show_hidden: self.panes[0].cur_ref().show_hidden,
                pane_width: self.pane_width,
                bookmarks: self.bookmarks.clone(),
                tabs: self
                    .panes
                    .iter()
                    .map(|pane| {
                        pane.tabs
                            .iter()
                            .map(|t| PersistTab {
                                path: t.path.display().to_string(),
                                name: t.name.clone(),
                            })
                            .collect()
                    })
                    .collect(),
            },
        );
    }

    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.tick(ctx);
    }
}

impl CommanderApp {
    /// Render one frame. Shared by the eframe app and the headless UI tests
    /// (which have no eframe window to drive).
    fn tick(&mut self, ctx: &egui::Context) {
        self.poll_jobs(ctx);
        self.handle_os_drops(ctx);
        self.handle_keys(ctx);
        self.show_top_bar(ctx);
        self.show_status_bar(ctx);
        self.show_panels(ctx);
        self.show_drop_menu(ctx);
        self.show_modal(ctx);
        self.show_toolbox(ctx);
        self.show_viewer(ctx);
        self.show_help_window(ctx);
        self.show_toasts(ctx);

        // After a Tab, take egui's keyboard focus back off whatever widget it
        // landed on: a focused button would react to Enter and Space, and the
        // panels' own keys would be treated as typing.
        if self.tab_release > 0 {
            self.tab_release -= 1;
            let focused = ctx.memory_mut(|m| m.focused());
            if let Some(id) = focused {
                ctx.memory_mut(|m| m.surrender_focus(id));
            }
        }
    }
}

#[cfg(test)]
#[path = "app_tests.rs"]
mod tests;

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

/// Toolbar glyphs (monochrome emoji, rendered by egui's bundled Noto
/// emoji font - all verified present via examples/emoji_coverage.rs).
const ICON_VIEW: &str = "🔍";
const ICON_EDIT: &str = "✏";
const ICON_COPY: &str = "📋";
const ICON_MOVE: &str = "🔁";
const ICON_NEW_FOLDER: &str = "📁";
const ICON_RENAME: &str = "📝";
const ICON_DELETE: &str = "🗑";
const ICON_OPEN_EXT: &str = "↗";
const ICON_TOOLS: &str = "🔧";

/// Toolbar button with an emoji icon. Returns true on click. Greyed out and
/// inert when `enabled` is false (tooltip still explains why).
fn icon_button(ui: &mut egui::Ui, icon: &'static str, enabled: bool, tip: &str) -> bool {
    let txt = egui::RichText::new(icon).size(15.0);
    let txt = if enabled { txt } else { txt.weak() };
    let mut resp = ui.add(egui::Button::new(txt));
    resp = resp.on_hover_text(tip);
    if !enabled {
        resp.on_disabled_hover_text(format!("{} (nothing selected)", tip));
        return false;
    }
    resp.clicked()
}

/// Dim bar marking the cursor row of the pane that is not active. Unlike a
/// selected row it must not read as part of the selection.
fn cursor_shade(ui: &mut egui::Ui, show: bool) {
    if show {
        let accent = ui.visuals().selection.bg_fill;
        ui.painter()
            .rect_filled(ui.max_rect(), 0.0, accent.gamma_multiply(0.35));
    }
}

/// A single-line text field that claims only its own rectangle. Left alone,
/// `TextEdit` also lays an invisible strip beside the field for text that
/// doesn't fit (so a scroll area could follow the caret); that strip advances
/// the row and pushes whatever follows — "Go" after the path bar, "Hidden"
/// after the filter — away, out of the pane for long paths or narrow panes.
fn field_edit(
    ui: &mut egui::Ui,
    id_salt: impl std::hash::Hash,
    text: &mut String,
    width: f32,
    hint: &str,
) -> egui::Response {
    // The extra 8px is TextEdit's default side margin. The height is left
    // generous: only the field's own extent gets claimed.
    let rect = egui::Rect::from_min_size(
        ui.cursor().min,
        egui::vec2(width + 8.0, ui.spacing().interact_size.y + 16.0),
    );
    let mut field = ui.new_child(egui::UiBuilder::new().id_salt(id_salt).max_rect(rect));
    let resp = field.add(
        egui::TextEdit::singleline(text)
            .hint_text(hint)
            .desired_width(width),
    );
    ui.allocate_rect(field.min_rect().intersect(rect), egui::Sense::hover());
    resp
}

/// Per-cell glue around [`row_press`] for one table row. While a drag from
/// the other pane is over this pane (`target`), the row under the pointer is
/// remembered as the drop destination.
#[allow(clippy::too_many_arguments)]
fn cell_press(
    ui: &mut egui::Ui,
    p: &mut FilePanel,
    vi: usize,
    path: &Path,
    is_dir: bool,
    r: &egui::Response,
    row_drag: bool,
    target: bool,
    target_row: &RefCell<Option<(PathBuf, bool, egui::Rect)>>,
    began: &Cell<bool>,
) {
    if target && ui.rect_contains_pointer(ui.max_rect()) {
        target_row
            .borrow_mut()
            .replace((path.to_path_buf(), is_dir, ui.max_rect()));
    }
    let started = row_press(ui, p, vi, path, r.rect, row_drag);
    began.set(began.get() || started);
}

/// Mouse handling for one table row.
///
/// A press that begins on the row (reported once through the return value)
/// starts a row drag: inside its own pane it keeps range-selecting, and it
/// turns into a copy/move once the pointer enters the other pane. A plain
/// press anchors a range selection that every row the pointer passes extends;
/// anchoring on an already-selected row extends the existing selection
/// instead of replacing it. With Ctrl / Shift held at the press nothing is
/// selected, so the drag carries the selection unchanged.
fn row_press(
    ui: &mut egui::Ui,
    p: &mut FilePanel,
    vi: usize,
    path: &Path,
    rect: egui::Rect,
    row_drag: bool,
) -> bool {
    if !ui.input(|i| i.pointer.primary_down()) {
        p.press_row = None;
        p.sweep_locked = false;
        return false;
    }

    let mut began = false;
    if p.press_row.is_none()
        && ui.input(|i| i.pointer.press_origin()).is_some_and(|o| rect.contains(o))
    {
        p.press_row = Some(vi);
        began = true;
    }

    if p.sweep_locked {
        return began;
    }
    let hovered = ui.rect_contains_pointer(ui.max_rect());
    let unmodified = !ui.input(|i| i.modifiers.any());
    if let Some(anchor) = p.drag_from {
        if hovered {
            p.selected = p.sweep_base.clone();
            let (lo, hi) = (anchor.min(vi), anchor.max(vi));
            for k in lo..=hi {
                if let Some(&ei) = p.visible.get(k) {
                    p.selected.insert(p.entries[ei].path.clone());
                }
            }
            p.cursor = vi;
        }
    } else if unmodified && hovered && (began || !row_drag) {
        // `!row_drag` keeps a press that began on empty space selecting from
        // the first row it sweeps into; a press that began on a row of the
        // other pane must not select here.
        p.drag_from = Some(vi);
        p.anchor = Some(vi);
        p.sweep_kept_base = p.selected.contains(path);
        if !p.sweep_kept_base {
            p.selected.clear();
            p.selected.insert(path.to_path_buf());
        }
        p.sweep_base = p.selected.clone();
        p.cursor = vi;
    }
    began
}

/// Whether two paths denote the same folder (compared through canonical
/// paths, so aliases like `/var` and `/private/var` match).
fn same_folder(a: &Path, b: &Path) -> bool {
    match (std::fs::canonicalize(a), std::fs::canonicalize(b)) {
        (Ok(a), Ok(b)) => a == b,
        _ => a == b,
    }
}

/// One persisted folder tab (path + optional custom name).
#[derive(serde::Serialize, serde::Deserialize)]
struct PersistTab {
    path: String,
    name: Option<String>,
}

/// A pinned folder shown in the Favorites menu.
#[derive(serde::Serialize, serde::Deserialize, Clone)]
struct Bookmark {
    name: String,
    path: String,
}

/// Persisted preferences, loaded at startup and written on shutdown.
#[derive(serde::Serialize, serde::Deserialize)]
#[serde(default)]
struct Prefs {
    dark: bool,
    show_hidden: bool,
    pane_width: Option<f32>,
    /// Folder tabs per pane (paths + names); entries reload on startup.
    tabs: Vec<Vec<PersistTab>>,
    bookmarks: Vec<Bookmark>,
}

impl Default for Prefs {
    fn default() -> Self {
        Self {
            dark: true,
            show_hidden: false,
            pane_width: None,
            tabs: Vec::new(),
            bookmarks: Vec::new(),
        }
    }
}
