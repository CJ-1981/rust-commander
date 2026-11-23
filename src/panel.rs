use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use chrono::{DateTime, Local};

/// A single entry (file or directory) shown in a panel.
#[derive(Clone, Debug)]
pub struct FileEntry {
    pub name: String,
    pub path: PathBuf,
    pub is_dir: bool,
    pub size: u64,
    pub modified: Option<SystemTime>,
}

impl FileEntry {
    pub fn ext_lower(&self) -> String {
        self.path
            .extension()
            .map(|e| e.to_string_lossy().to_lowercase())
            .unwrap_or_default()
    }
}

/// Sort keys for the file table.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SortKey {
    Name,
    Size,
    Modified,
    Ext,
}

/// One half of the dual-pane view: a directory listing with selection state.
pub struct FilePanel {
    pub path: PathBuf,
    pub path_edit: String,
    pub entries: Vec<FileEntry>,
    /// Indices into `entries` after filtering (hidden files) and sorting.
    pub visible: Vec<usize>,
    /// Cursor position (index into `visible`), moved with the keyboard.
    pub cursor: usize,
    /// Explicitly selected entry paths (survives reloads).
    pub selected: HashSet<PathBuf>,
    pub show_hidden: bool,
    pub sort: SortKey,
    pub ascending: bool,
    /// Set when the last reload failed to read the directory.
    pub load_error: Option<String>,
    /// Anchor row (index into `visible`) of an in-progress press-drag
    /// multi-select; cleared on release, navigation and re-sorting.
    pub drag_from: Option<usize>,
    /// Custom tab name; `None` shows the folder's own name.
    pub name: Option<String>,
}

impl FilePanel {
    pub fn new(start: PathBuf) -> Self {
        let mut p = Self {
            path: PathBuf::new(),
            path_edit: String::new(),
            entries: Vec::new(),
            visible: Vec::new(),
            cursor: 0,
            selected: HashSet::new(),
            show_hidden: false,
            sort: SortKey::Name,
            ascending: true,
            load_error: None,
            drag_from: None,
            name: None,
        };
        p.set_path(start);
        p
    }

    /// Navigate to a directory and (re)load its contents.
    pub fn set_path(&mut self, path: PathBuf) {
        let path = if path.exists() {
            fs::canonicalize(&path).unwrap_or(path)
        } else {
            path
        };
        self.path = path;
        self.path_edit = self.path.display().to_string();
        self.cursor = 0;
        self.selected.clear();
        self.drag_from = None;
        self.reload();
    }

    /// Re-read the current directory, keeping the selection set.
    pub fn reload(&mut self) {
        self.entries.clear();
        self.load_error = None;
        match fs::read_dir(&self.path) {
            Ok(rd) => {
                for e in rd.flatten() {
                    let path = e.path();
                    let name = e.file_name().to_string_lossy().into_owned();
                    let meta = e.metadata().ok();
                    let (is_dir, size, modified) = match &meta {
                        Some(m) => (
                            m.is_dir(),
                            if m.is_dir() { 0 } else { m.len() },
                            m.modified().ok(),
                        ),
                        None => (false, 0, None),
                    };
                    self.entries.push(FileEntry {
                        name,
                        path,
                        is_dir,
                        size,
                        modified,
                    });
                }
            }
            Err(e) => self.load_error = Some(e.to_string()),
        }
        self.apply_filter_sort();
    }

    /// Re-apply hidden-file filtering and sorting to the visible list.
    pub fn apply_filter_sort(&mut self) {
        let mut idx: Vec<usize> = (0..self.entries.len())
            .filter(|&i| self.show_hidden || !is_hidden_name(&self.entries[i].name))
            .collect();
        idx.sort_by(|&a, &b| {
            let (a, b) = (&self.entries[a], &self.entries[b]);
            if a.is_dir != b.is_dir {
                return if a.is_dir {
                    std::cmp::Ordering::Less
                } else {
                    std::cmp::Ordering::Greater
                };
            }
            let ord = match self.sort {
                SortKey::Name => a.name.to_lowercase().cmp(&b.name.to_lowercase()),
                SortKey::Ext => a
                    .ext_lower()
                    .cmp(&b.ext_lower())
                    .then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase())),
                SortKey::Size => a.size.cmp(&b.size),
                SortKey::Modified => a.modified.cmp(&b.modified),
            };
            if self.ascending {
                ord
            } else {
                ord.reverse()
            }
        });
        self.visible = idx;
        self.drag_from = None;
        if self.cursor >= self.visible.len() {
            self.cursor = self.visible.len().saturating_sub(1);
        }
    }

    pub fn visible_entry(&self, vis_idx: usize) -> Option<&FileEntry> {
        self.visible.get(vis_idx).map(|&i| &self.entries[i])
    }

    /// Counts over the visible listing: (dirs, files, total bytes of files).
    pub fn stats(&self) -> (usize, usize, u64) {
        let (mut d, mut f, mut b) = (0usize, 0usize, 0u64);
        for &vi in &self.visible {
            let e = &self.entries[vi];
            if e.is_dir {
                d += 1;
            } else {
                f += 1;
                b += e.size;
            }
        }
        (d, f, b)
    }

    /// Selected stats: (items, bytes of selected files).
    pub fn selected_stats(&self) -> (usize, u64) {
        let (mut n, mut b) = (0usize, 0u64);
        for &vi in &self.visible {
            let e = &self.entries[vi];
            if self.selected.contains(&e.path) {
                n += 1;
                if !e.is_dir {
                    b += e.size;
                }
            }
        }
        (n, b)
    }
}

/// Names treated as hidden regardless of platform conventions.
pub fn is_hidden_name(name: &str) -> bool {
    name.starts_with('.') || name == "$RECYCLE.BIN" || name == "System Volume Information"
}

/// List available roots: drive letters on Windows, `/`, home and `/Volumes` on Unix.
pub fn list_roots() -> Vec<PathBuf> {
    let mut roots = Vec::new();
    if cfg!(windows) {
        for c in b'A'..=b'Z' {
            let p = PathBuf::from(format!("{}:\\", c as char));
            if p.is_dir() {
                roots.push(p);
            }
        }
    } else {
        if let Ok(home) = std::env::var("HOME") {
            let p = PathBuf::from(home);
            if p.is_dir() {
                roots.push(p);
            }
        }
        roots.push(PathBuf::from("/"));
        if let Ok(rd) = fs::read_dir("/Volumes") {
            for e in rd.flatten() {
                roots.push(e.path());
            }
        }
    }
    roots
}

/// Human readable byte size.
pub fn fmt_size(bytes: u64) -> String {
    const KB: f64 = 1024.0;
    const MB: f64 = KB * KB;
    const GB: f64 = MB * KB;
    let b = bytes as f64;
    if b < KB {
        format!("{} B", bytes)
    } else if b < MB {
        format!("{:.1} KB", b / KB)
    } else if b < GB {
        format!("{:.1} MB", b / MB)
    } else {
        format!("{:.2} GB", b / GB)
    }
}

/// Human readable modification time (`YYYY-MM-DD HH:MM`, local time).
pub fn fmt_time(t: Option<SystemTime>) -> String {
    match t {
        Some(t) => {
            let dt = DateTime::<Local>::from(t);
            dt.format("%Y-%m-%d %H:%M").to_string()
        }
        None => "-".to_string(),
    }
}

/// One half of the dual-pane view: a set of folder tabs, one of them active.
pub struct Pane {
    pub tabs: Vec<FilePanel>,
    pub active: usize,
}

impl Pane {
    pub fn new(start: PathBuf) -> Self {
        Self {
            tabs: vec![FilePanel::new(start)],
            active: 0,
        }
    }

    /// The active tab's panel (read-only).
    pub fn cur_ref(&self) -> &FilePanel {
        let i = self.active.min(self.tabs.len().saturating_sub(1));
        &self.tabs[i]
    }

    /// The active tab's panel.
    pub fn cur(&mut self) -> &mut FilePanel {
        let i = self.active.min(self.tabs.len().saturating_sub(1));
        &mut self.tabs[i]
    }

    /// Open a new tab showing `path` and make it active.
    pub fn open_tab(&mut self, path: PathBuf) {
        self.tabs.push(FilePanel::new(path));
        self.active = self.tabs.len() - 1;
    }

    /// Close tab `idx` (never the last one); keeps a sensible active index.
    pub fn close_tab(&mut self, idx: usize) {
        if self.tabs.len() <= 1 {
            return;
        }
        let was_active = self.active == idx;
        self.tabs.remove(idx);
        if was_active {
            self.active = self.active.min(self.tabs.len() - 1);
        } else if idx < self.active {
            self.active -= 1;
        }
    }

    /// Label shown on a tab: the user's name if set, else the folder name.
    pub fn tab_label(&self, idx: usize) -> String {
        let tab = &self.tabs[idx];
        tab.name
            .clone()
            .unwrap_or_else(|| display_name(&tab.path))
    }
}

/// The name a folder gets on a tab: its last path component (or `/`-style
/// path for roots).
pub fn display_name(path: &Path) -> String {
    path.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| path.display().to_string())
}
