use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use chrono::{DateTime, Local};
use unicode_normalization::UnicodeNormalization;

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
    /// Entry for a path that did not come from a panel listing (a file
    /// dropped from the OS). `path` keeps the on-disk bytes; only the shown
    /// name is normalized.
    pub fn from_path(path: &Path) -> Self {
        let name = nfc(&path.file_name().map(|n| n.to_string_lossy()).unwrap_or_default());
        Self {
            name,
            path: path.to_path_buf(),
            is_dir: path.is_dir(),
            size: 0,
            modified: None,
        }
    }

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
    /// Request to scroll the (keyboard-moved) cursor row into view; consumed
    /// by the table pass. Never set by mouse scrolling, so wheel scrolling is
    /// not fought by the auto-scroll.
    pub scroll_needed: bool,
    /// Previously visited folders of this tab (most recent last).
    pub back: Vec<PathBuf>,
    /// Folders revisited after going back (most recent last).
    pub fwd: Vec<PathBuf>,
    /// Case-insensitive name filter narrowing the listing.
    pub filter: String,
    /// Selection as it was when the current press-drag began; the swept
    /// range is added on top of it.
    pub sweep_base: HashSet<PathBuf>,
    /// The drag began on a row that was already selected, so it extends the
    /// selection instead of replacing it.
    pub sweep_kept_base: bool,
    /// Visible index of the row the current mouse press began on.
    pub press_row: Option<usize>,
    /// Range selection is suspended until the button is released (set once a
    /// drag has turned into a drop into the other pane).
    pub sweep_locked: bool,
    /// Row (index into `visible`) a range selection starts from: the row of
    /// the last plain click or arrow-key move. Cleared when the rows are
    /// re-sorted or the folder changes.
    pub anchor: Option<usize>,
    /// The row clicked last, and when. A second click on that same row within
    /// the double-click interval opens it.
    pub last_click: Option<(PathBuf, f64)>,
}

/// Seconds within which a second click on the same row counts as a
/// double-click.
pub const DOUBLE_CLICK_SECS: f64 = 0.35;

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
            scroll_needed: false,
            back: Vec::new(),
            fwd: Vec::new(),
            filter: String::new(),
            sweep_base: HashSet::new(),
            sweep_kept_base: false,
            press_row: None,
            sweep_locked: false,
            anchor: None,
            last_click: None,
        };
        p.set_path(start);
        p
    }

    /// Navigate to a directory and (re)load its contents.
    pub fn set_path(&mut self, path: PathBuf) {
        self.navigate(path, true);
    }

    /// Navigate, optionally recording the move in the back history
    /// (traversal via back/forward must not push new entries).
    fn navigate(&mut self, path: PathBuf, record: bool) {
        let path = if path.exists() {
            fs::canonicalize(&path).unwrap_or(path)
        } else {
            // Misspelling check: try the other normalization form before
            // giving up (NFC typed text vs NFD names on disk).
            path.to_str()
                .map(resolve_typed_path)
                .unwrap_or_else(|| path.clone())
        };
        if record && self.path != path {
            self.back.push(self.path.clone());
            self.fwd.clear();
        }
        self.path = path;
        self.path_edit = nfc(&self.path.display().to_string());
        self.cursor = 0;
        self.selected.clear();
        self.drag_from = None;
        self.anchor = None;
        self.last_click = None;
        self.reload();
    }

    /// Go to the previously visited folder; returns true on success.
    pub fn go_back(&mut self) -> bool {
        if let Some(prev) = self.back.pop() {
            let cur = self.path.clone();
            self.navigate(prev, false);
            self.fwd.push(cur);
            true
        } else {
            false
        }
    }

    /// Go to the folder visited before the last "back"; returns true on success.
    pub fn go_forward(&mut self) -> bool {
        if let Some(next) = self.fwd.pop() {
            let cur = self.path.clone();
            self.navigate(next, false);
            self.back.push(cur);
            true
        } else {
            false
        }
    }

    /// Re-read the current directory, keeping the selection set.
    pub fn reload(&mut self) {
        self.entries.clear();
        self.load_error = None;
        match fs::read_dir(&self.path) {
            Ok(rd) => {
                for e in rd.flatten() {
                    let path = e.path();
                    // Display name in NFC; `path` keeps the raw on-disk form
                    // for every filesystem operation.
                    let name = nfc(&e.file_name().to_string_lossy());
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
        self.apply_filter_sort_with(&std::collections::HashMap::new());
    }

    /// Same as `apply_filter_sort`, but sorts folders by their cached
    /// recursive size (path -> (bytes, items), from the folder-size tool).
    pub fn apply_filter_sort_with(
        &mut self,
        dir_sizes: &std::collections::HashMap<String, (u64, u64)>,
    ) {
        let needle = nfc(&self.filter).to_lowercase();
        let mut idx: Vec<usize> = (0..self.entries.len())
            .filter(|&i| self.show_hidden || !is_hidden_name(&self.entries[i].name))
            .filter(|&i| {
                needle.is_empty() || self.entries[i].name.to_lowercase().contains(&needle)
            })
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
                SortKey::Size => {
                    let sa = if a.is_dir {
                        dir_sizes
                            .get(&a.path.display().to_string())
                            .map(|s| s.0)
                            .unwrap_or(0)
                    } else {
                        a.size
                    };
                    let sb = if b.is_dir {
                        dir_sizes
                            .get(&b.path.display().to_string())
                            .map(|s| s.0)
                            .unwrap_or(0)
                    } else {
                        b.size
                    };
                    sa.cmp(&sb)
                }
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
        self.anchor = None;
        self.last_click = None;
        self.scroll_needed = true;
        if self.cursor >= self.visible.len() {
            self.cursor = self.visible.len().saturating_sub(1);
        }
    }

    pub fn visible_entry(&self, vis_idx: usize) -> Option<&FileEntry> {
        self.visible.get(vis_idx).map(|&i| &self.entries[i])
    }

    /// Path of the row at visible index `vi`, if it is within the listing.
    fn row_path(&self, vi: usize) -> Option<PathBuf> {
        self.visible.get(vi).map(|&ei| self.entries[ei].path.clone())
    }

    /// Select only row `vi` and put the cursor there. The row becomes the
    /// start of the next range selection. Does nothing if `vi` is out of range.
    pub fn select_only(&mut self, vi: usize) {
        if let Some(path) = self.row_path(vi) {
            self.selected.clear();
            self.selected.insert(path);
            self.cursor = vi;
            self.anchor = Some(vi);
        }
    }

    /// Select the rows from the selection's start to row `vi` and move the
    /// cursor there. The start is the row of the last plain click or arrow-key
    /// move, or the cursor if there is none yet.
    pub fn extend_to(&mut self, vi: usize) {
        if self.row_path(vi).is_none() {
            return;
        }
        let start = self.anchor.unwrap_or(self.cursor);
        self.selected.clear();
        for k in start.min(vi)..=start.max(vi) {
            if let Some(path) = self.row_path(k) {
                self.selected.insert(path);
            }
        }
        self.cursor = vi;
    }

    /// Move the cursor by `delta` rows. With `extend`, the rows from the
    /// selection's start to the new cursor position are selected; without it
    /// the selection collapses onto the cursor.
    pub fn move_cursor(&mut self, delta: i32, extend: bool) {
        if self.visible.is_empty() {
            return;
        }
        let last = self.visible.len() as i32 - 1;
        let target = (self.cursor as i32 + delta).clamp(0, last) as usize;
        self.go_to(target, extend);
    }

    /// Put the cursor on row `vi`, selecting as in [`Self::move_cursor`].
    pub fn go_to(&mut self, vi: usize, extend: bool) {
        if extend {
            self.extend_to(vi);
        } else {
            self.select_only(vi);
        }
        self.scroll_needed = true;
    }

    /// Register a plain click on `path` at time `now` (seconds). Returns true
    /// when this is the second click on that same row within the
    /// double-click interval. egui's own detection is not tied to a widget, so
    /// two quick clicks on different rows would otherwise open the second one.
    pub fn clicked_at(&mut self, path: &Path, now: f64) -> bool {
        let again = matches!(&self.last_click,
            Some((clicked, at)) if clicked == path && now - *at <= DOUBLE_CLICK_SECS);
        self.last_click = if again {
            None
        } else {
            Some((path.to_path_buf(), now))
        };
        again
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

    /// Listed entries whose paths are in `paths`, in listing order.
    pub fn entries_matching(&self, paths: &HashSet<PathBuf>) -> Vec<FileEntry> {
        self.visible
            .iter()
            .filter_map(|&vi| self.entries.get(vi))
            .filter(|e| paths.contains(&e.path))
            .cloned()
            .collect()
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
    /// The tab clicked last, and when: a second click on the same tab within
    /// the double-click interval renames it.
    last_tab_click: Option<(usize, f64)>,
}

impl Pane {
    pub fn new(start: PathBuf) -> Self {
        Self {
            tabs: vec![FilePanel::new(start)],
            active: 0,
            last_tab_click: None,
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

    /// Register a click on tab `ti` at time `now` (seconds). Returns true when
    /// this is the second click on that same tab within the double-click
    /// interval (egui's own detection would also fire for a quick click on a
    /// different tab).
    pub fn tab_clicked_at(&mut self, ti: usize, now: f64) -> bool {
        let again = matches!(&self.last_tab_click,
            Some((clicked, at)) if *clicked == ti && now - *at <= DOUBLE_CLICK_SECS);
        self.last_tab_click = if again {
            None
        } else {
            Some((ti, now))
        };
        again
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
        .map(|n| nfc(&n.to_string_lossy()))
        .unwrap_or_else(|| path.display().to_string())
}

/// Normalize a display string to Unicode NFC (composed form).
///
/// macOS keeps Finder-created names in decomposed NFD (each Hangul syllable
/// split into conjoining jamo), and egui has no text shaping to re-compose
/// them, so NFD names render as separated jamo characters. Normalizing
/// display strings to NFC makes them readable; filesystem operations keep
/// using raw `PathBuf`s, so on-disk bytes are never rewritten through this.
pub fn nfc(s: &str) -> String {
    s.nfc().collect()
}

/// Resolve a user-entered path string to an existing path.
///
/// The typed text may use the other Unicode normalization form than the
/// names on disk (e.g. NFC text against NFD macOS names — or the reverse on
/// Linux). Exact lookup is tried first; if it misses, the NFD spelling is
/// tried so typed paths still resolve on normalization-sensitive systems.
pub fn resolve_typed_path(s: &str) -> PathBuf {
    let p = PathBuf::from(s);
    if p.exists() || p.as_os_str().is_empty() {
        return p;
    }
    for alt in [s.nfd().collect::<String>(), s.nfc().collect::<String>()] {
        if alt != s {
            let q = PathBuf::from(&alt);
            if q.exists() {
                return q;
            }
        }
    }
    p
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nfc_composes_decomposed_hangul() {
        // Jamo sequence for "한글": HAN = ᄒ+ᅡ+ᆫ, GEUL = ᄀ+ᅳ+ᆯ.
        let nfd = "\u{1112}\u{1161}\u{11AB}\u{1100}\u{1173}\u{11AF}";
        assert_eq!(nfc(nfd), "한글");
        assert_eq!(nfc("한글"), "한글");
    }

    #[test]
    fn resolve_accepts_other_normalization_form() {
        let tmp = std::env::temp_dir().join("rc-nfd-resolve");
        let _ = fs::remove_dir_all(&tmp);
        fs::create_dir_all(&tmp).unwrap();
        // Create the file with a decomposed (NFD) name, as Finder does.
        let nfd: String = "한글.txt".nfd().collect();
        let f = tmp.join(&nfd);
        fs::write(&f, b"x").unwrap();
        // Composed text must resolve to the file whichever form is returned.
        let got = resolve_typed_path(tmp.join("한글.txt").to_str().unwrap());
        assert!(got.is_file());
        let _ = fs::remove_dir_all(&tmp);
    }
}
