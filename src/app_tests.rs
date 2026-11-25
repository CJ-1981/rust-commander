//! Headless UI tests: the real frame code driven by simulated pointer events,
//! so mouse behaviour (drag-and-drop, range selection, drops) and the pane
//! layout are verified without a window or a physical mouse.

use super::*;
use egui_kittest::{Harness, kittest::Queryable};

type AppHarness = Harness<'static, CommanderApp>;

/// Scratch folder tree, removed when the test ends.
struct Scratch(PathBuf);

impl Scratch {
    fn new(tag: &str) -> Self {
        let dir = std::env::temp_dir().join(format!(
            "rc-ui-test-{}-{}",
            tag,
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("src")).unwrap();
        std::fs::create_dir_all(dir.join("dst")).unwrap();
        Scratch(dir)
    }

    fn src(&self) -> PathBuf {
        self.0.join("src")
    }

    fn dst(&self) -> PathBuf {
        self.0.join("dst")
    }

    fn write(&self, path: impl AsRef<Path>, contents: &str) {
        std::fs::write(path.as_ref(), contents).unwrap();
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// The app with no saved state, its two panes showing `src` and `dst`.
fn app_in(src: &Path, dst: &Path) -> CommanderApp {
    CommanderApp {
        panes: [Pane::new(src.to_path_buf()), Pane::new(dst.to_path_buf())],
        active: 0,
        dark: true,
        pane_width: None,
        dir_sizes: Default::default(),
        bookmarks: Vec::new(),
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
    }
}

/// The app running in a window of `size`, after a few frames.
fn harness_sized(size: egui::Vec2, src: &Path, dst: &Path) -> AppHarness {
    let mut harness = Harness::builder()
        .with_size(size)
        .build_state(|ctx, app: &mut CommanderApp| app.tick(ctx), app_in(src, dst));
    harness.run();
    harness
}

fn harness(src: &Path, dst: &Path) -> AppHarness {
    harness_sized(egui::vec2(1200.0, 700.0), src, dst)
}

/// Center of the widget carrying `label`.
fn center(harness: &AppHarness, label: &str) -> egui::Pos2 {
    let r = harness.get_by_label(label).raw_bounds().expect("widget has bounds");
    egui::pos2(
        ((r.x0 + r.x1) / 2.0) as f32,
        ((r.y0 + r.y1) / 2.0) as f32,
    )
}

fn pointer_moved(harness: &mut AppHarness, pos: egui::Pos2) {
    harness.input_mut().events.push(egui::Event::PointerMoved(pos));
    harness.step();
}

fn button(harness: &mut AppHarness, pos: egui::Pos2, pressed: bool) {
    harness.input_mut().events.push(egui::Event::PointerButton {
        pos,
        button: egui::PointerButton::Primary,
        pressed,
        modifiers: Default::default(),
    });
    harness.step();
}

/// Center of the `n`-th widget carrying `label` (rows repeat across panes).
fn center_of(harness: &AppHarness, label: &str, n: usize) -> egui::Pos2 {
    let r = harness
        .get_all_by_label(label)
        .nth(n)
        .expect("widget has bounds")
        .raw_bounds()
        .expect("widget has bounds");
    egui::pos2(
        ((r.x0 + r.x1) / 2.0) as f32,
        ((r.y0 + r.y1) / 2.0) as f32,
    )
}

/// Press on `from`, sweep across to `to`, and keep the button held.
fn drag_held(harness: &mut AppHarness, from: egui::Pos2, to: egui::Pos2) {
    pointer_moved(harness, from);
    button(harness, from, true);
    for t in [0.25, 0.5, 0.75] {
        pointer_moved(harness, from.lerp(to, t));
    }
    pointer_moved(harness, to);
}

/// Names of the files in `folder`, sorted.
fn names(folder: &Path) -> Vec<String> {
    let mut names: Vec<String> = std::fs::read_dir(folder)
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    names
}

/// Selected names in pane `pane`, sorted.
fn selected(harness: &AppHarness, pane: usize) -> Vec<String> {
    let panel = harness.state().panes[pane].cur_ref();
    let mut names: Vec<String> = panel
        .entries_matching(&panel.selected)
        .iter()
        .map(|e| e.name.clone())
        .collect();
    names.sort();
    names
}

/// Run frames until the background job has produced `done`, then settle.
fn settle(harness: &mut AppHarness, done: impl Fn() -> bool) {
    for _ in 0..500 {
        if done() {
            break;
        }
        harness.step();
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    harness.run();
}

fn menu_items(harness: &AppHarness) -> Vec<String> {
    let mut names: Vec<String> = harness
        .state()
        .drop_menu
        .as_ref()
        .expect("the drop menu is open")
        .items
        .iter()
        .map(|e| e.name.clone())
        .collect();
    names.sort();
    names
}

#[test]
fn dragging_a_file_onto_the_other_pane_offers_copy_or_move() {
    let scratch = Scratch::new("dnd-copy");
    scratch.write(scratch.src().join("alpha.txt"), "alpha");

    let mut h = harness(&scratch.src(), &scratch.dst());
    let from = center(&h, "alpha.txt");
    let to = h.state().pane_rects[1].center();
    drag_held(&mut h, from, to);

    let drag = h.state().row_drag.as_ref().expect("drag in progress");
    assert!(drag.committed, "entering the other pane turns the drag into a drop");
    assert_eq!(drag.items.len(), 1);

    button(&mut h, to, false);
    assert_eq!(menu_items(&h), ["alpha.txt"]);
    let target = std::fs::canonicalize(scratch.dst()).unwrap();
    assert_eq!(h.state().drop_menu.as_ref().unwrap().target, target);
    // The drag must not have selected rows in the pane it was dropped on.
    assert!(selected(&h, 1).is_empty());

    h.get_by_label("Copy here").click();
    settle(&mut h, || scratch.dst().join("alpha.txt").exists());

    assert_eq!(names(&scratch.dst()), ["alpha.txt"]);
    assert_eq!(names(&scratch.src()), ["alpha.txt"], "a copy leaves the source");
}

#[test]
fn dropping_with_shift_moves_without_asking() {
    let scratch = Scratch::new("dnd-shift");
    scratch.write(scratch.src().join("alpha.txt"), "alpha");

    let mut h = harness(&scratch.src(), &scratch.dst());
    let from = center(&h, "alpha.txt");
    let to = h.state().pane_rects[1].center();
    drag_held(&mut h, from, to);

    // Release with Shift held.
    h.input_mut().modifiers = egui::Modifiers::SHIFT;
    button(&mut h, to, false);

    assert!(h.state().drop_menu.is_none(), "Shift answers the question");
    settle(&mut h, || scratch.dst().join("alpha.txt").exists());

    assert_eq!(names(&scratch.dst()), ["alpha.txt"]);
    assert!(names(&scratch.src()).is_empty(), "a move takes the source");
}

#[test]
fn dragging_within_a_pane_still_range_selects() {
    let scratch = Scratch::new("dnd-sweep");
    for name in ["alpha.txt", "beta.txt", "gamma.txt", "zeta.txt"] {
        scratch.write(scratch.src().join(name), name);
    }

    let mut h = harness(&scratch.src(), &scratch.dst());
    let from = center(&h, "alpha.txt");
    let to = center(&h, "gamma.txt");
    drag_held(&mut h, from, to);

    assert_eq!(
        selected(&h, 0),
        ["alpha.txt", "beta.txt", "gamma.txt"],
        "rows swept over are selected while the button is held"
    );
    assert!(
        !h.state().row_drag.as_ref().unwrap().committed,
        "staying inside the pane keeps it a selection"
    );

    button(&mut h, to, false);
    assert_eq!(
        selected(&h, 0),
        ["alpha.txt", "beta.txt", "gamma.txt"]
    );
    assert!(h.state().drop_menu.is_none(), "nothing was dropped");
    assert_eq!(names(&scratch.dst()), Vec::<String>::new());
}

#[test]
fn dragging_from_a_selected_row_carries_the_whole_selection() {
    let scratch = Scratch::new("dnd-multi");
    for name in ["alpha.txt", "beta.txt", "gamma.txt"] {
        scratch.write(scratch.src().join(name), name);
    }

    let mut h = harness(&scratch.src(), &scratch.dst());
    // Select the three files by sweeping, then let go.
    let from = center(&h, "alpha.txt");
    let to = center(&h, "gamma.txt");
    drag_held(&mut h, from, to);
    button(&mut h, to, false);
    assert_eq!(selected(&h, 0).len(), 3);

    // Pick the selection up again from a row that is already selected.
    let again = center(&h, "beta.txt");
    let drop_at = h.state().pane_rects[1].center();
    drag_held(&mut h, again, drop_at);
    assert_eq!(h.state().row_drag.as_ref().unwrap().items.len(), 3);

    button(&mut h, drop_at, false);
    assert_eq!(
        menu_items(&h),
        ["alpha.txt", "beta.txt", "gamma.txt"]
    );
    h.get_by_label("Move here").click();
    settle(&mut h, || !scratch.src().join("gamma.txt").exists());

    assert_eq!(names(&scratch.dst()), ["alpha.txt", "beta.txt", "gamma.txt"]);
    assert!(names(&scratch.src()).is_empty());
}

#[test]
fn sweeping_from_a_selected_row_extends_instead_of_replacing() {
    let scratch = Scratch::new("dnd-extend");
    for name in ["alpha.txt", "beta.txt", "gamma.txt", "zeta.txt"] {
        scratch.write(scratch.src().join(name), name);
    }

    let mut h = harness(&scratch.src(), &scratch.dst());
    let alpha = center(&h, "alpha.txt");
    let beta = center(&h, "beta.txt");
    drag_held(&mut h, alpha, beta);
    button(&mut h, beta, false);

    let zeta = center(&h, "zeta.txt");
    drag_held(&mut h, beta, zeta);
    button(&mut h, zeta, false);

    assert_eq!(
        selected(&h, 0),
        ["alpha.txt", "beta.txt", "gamma.txt", "zeta.txt"],
        "the earlier selection is kept and the new range added"
    );
}

#[test]
fn dropping_onto_a_folder_row_targets_that_folder() {
    let scratch = Scratch::new("dnd-folder");
    scratch.write(scratch.src().join("alpha.txt"), "alpha");
    std::fs::create_dir(scratch.dst().join("inner")).unwrap();

    let mut h = harness(&scratch.src(), &scratch.dst());
    let from = center(&h, "alpha.txt");
    let onto = center(&h, "inner");
    drag_held(&mut h, from, onto);

    button(&mut h, onto, false);
    let expected = std::fs::canonicalize(scratch.dst().join("inner")).unwrap();
    assert_eq!(h.state().drop_menu.as_ref().unwrap().target, expected);

    h.get_by_label("Copy here").click();
    settle(&mut h, || scratch.dst().join("inner/alpha.txt").exists());

    assert!(scratch.dst().join("inner/alpha.txt").exists());
    assert!(!scratch.dst().join("alpha.txt").exists());
}

#[test]
fn cancelling_the_drop_changes_nothing() {
    let scratch = Scratch::new("dnd-cancel");
    scratch.write(scratch.src().join("alpha.txt"), "alpha");

    let mut h = harness(&scratch.src(), &scratch.dst());
    let from = center(&h, "alpha.txt");
    let to = h.state().pane_rects[1].center();
    drag_held(&mut h, from, to);
    button(&mut h, to, false);

    h.get_by_label("Cancel").click();
    h.run();

    assert!(h.state().drop_menu.is_none());
    assert_eq!(names(&scratch.dst()), Vec::<String>::new());
    assert_eq!(names(&scratch.src()), ["alpha.txt"]);
}

#[test]
fn files_dropped_from_the_os_offer_copy_or_move_into_the_pane() {
    let scratch = Scratch::new("dnd-os");
    let outside = scratch.0.join("from-finder.txt");
    scratch.write(&outside, "finder");

    let mut h = harness(&scratch.src(), &scratch.dst());
    let over = h.state().pane_rects[1].center();
    pointer_moved(&mut h, over);
    h.input_mut().dropped_files.push(egui::DroppedFile {
        path: Some(outside.clone()),
        ..Default::default()
    });
    h.step();

    assert_eq!(menu_items(&h), ["from-finder.txt"]);
    assert_eq!(
        h.state().drop_menu.as_ref().unwrap().target,
        std::fs::canonicalize(scratch.dst()).unwrap()
    );
    h.get_by_label("Copy here").click();
    settle(&mut h, || scratch.dst().join("from-finder.txt").exists());

    assert!(scratch.dst().join("from-finder.txt").exists());
    assert!(outside.exists(), "a copy leaves the original");
}

#[test]
fn a_plain_click_still_selects_one_row() {
    let scratch = Scratch::new("dnd-click");
    for name in ["alpha.txt", "beta.txt"] {
        scratch.write(scratch.src().join(name), name);
    }

    let mut h = harness(&scratch.src(), &scratch.dst());
    let on = center(&h, "beta.txt");
    pointer_moved(&mut h, on);
    button(&mut h, on, true);
    button(&mut h, on, false);

    assert_eq!(selected(&h, 0), ["beta.txt"]);
    assert!(h.state().row_drag.is_none());
    assert!(h.state().drop_menu.is_none());
}

#[test]
fn a_drop_in_the_blank_area_of_a_short_pane_still_counts_as_that_pane() {
    let scratch = Scratch::new("dnd-blank");
    scratch.write(scratch.src().join("alpha.txt"), "alpha");

    let mut h = harness(&scratch.src(), &scratch.dst());
    let from = center(&h, "alpha.txt");
    // A point fixed in the window, in the right half but far below anything
    // the empty destination draws.
    let blank = egui::pos2(900.0, 560.0);
    drag_held(&mut h, from, blank);

    assert!(
        h.state().row_drag.as_ref().unwrap().committed,
        "the blank part of a pane is still that pane"
    );
    button(&mut h, blank, false);
    assert_eq!(menu_items(&h), ["alpha.txt"]);
}

#[test]
fn each_pane_stays_inside_its_allotted_rect() {
    let scratch = Scratch::new("dnd-fit");
    scratch.write(scratch.src().join("alpha.txt"), "alpha");

    let h = harness(&scratch.src(), &scratch.dst());
    let [left, right] = h.state().pane_rects;

    assert!(
        (right.left() - left.right() - 10.0).abs() < 0.5,
        "exactly the 10px divider between the panes: {} .. {}",
        left.right(),
        right.left()
    );
    assert!(
        right.right() <= h.ctx.screen_rect().right(),
        "the right pane ends inside the window"
    );
    // The last controls of each pane must be on screen, inside their pane.
    let hidden: Vec<_> = h.get_all_by_label("Hidden").collect();
    let ext: Vec<_> = h.get_all_by_label("Ext").collect();
    assert_eq!(hidden.len(), 2);
    assert_eq!(ext.len(), 2);
    for widget in [hidden[1], ext[1]] {
        let r = widget.raw_bounds().expect("widget has bounds");
        assert!(
            (r.x1 as f32) <= right.right(),
            "a right-edge control of the right pane ends at {:.0}, past the pane ({:.0})",
            r.x1,
            right.right()
        );
    }
}

#[test]
fn a_path_longer_than_the_path_bar_does_not_push_go_away() {
    let scratch = Scratch::new("dnd-long-path");
    // A deep folder, so the path is far wider than the path bar.
    let mut deep = scratch.src();
    for i in 0..8 {
        deep.push(format!("a-rather-long-folder-name-{i}"));
        std::fs::create_dir_all(&deep).unwrap();
    }
    scratch.write(deep.join("alpha.txt"), "alpha");

    let h = harness_sized(egui::vec2(1100.0, 520.0), &deep, &scratch.dst());
    let [left, right] = h.state().pane_rects;

    // The field takes the pane's width minus 50; "Go" follows right after it.
    let gos: Vec<_> = h.get_all_by_label("Go").collect();
    assert_eq!(gos.len(), 2);
    for (go, pane) in gos.iter().zip([left, right]) {
        let r = go.raw_bounds().expect("Go has bounds");
        let after_field = pane.right() - 50.0;
        assert!(
            (r.x0 as f32 - after_field).abs() <= 16.0,
            "Go sits at {:.0}; its field ends at {:.0}",
            r.x0,
            after_field
        );
    }
}

#[test]
fn keys_do_not_reach_the_panels_behind_the_drop_menu() {
    let scratch = Scratch::new("dnd-menu-keys");
    scratch.write(scratch.src().join("alpha.txt"), "alpha");

    let mut h = harness(&scratch.src(), &scratch.dst());
    let from = center(&h, "alpha.txt");
    let to = h.state().pane_rects[1].center();
    drag_held(&mut h, from, to);
    button(&mut h, to, false);
    assert!(h.state().drop_menu.is_some());

    // Enter would open the file under the cursor if it got through.
    h.press_key(egui::Key::Enter);
    h.run();

    assert!(h.state().viewer.is_none(), "Enter did not open a file");
    assert!(h.state().drop_menu.is_some(), "the menu stays open");

    // Escape closes it.
    h.press_key(egui::Key::Escape);
    h.run();
    assert!(h.state().drop_menu.is_none());
}

#[test]
fn dropping_files_into_the_folder_they_are_in_does_nothing() {
    let scratch = Scratch::new("dnd-same-folder");
    scratch.write(scratch.src().join("alpha.txt"), "alpha");

    // Both panes show the same folder.
    let mut h = harness(&scratch.src(), &scratch.src());
    let from = center_of(&h, "alpha.txt", 0);
    let to = h.state().pane_rects[1].center();
    drag_held(&mut h, from, to);
    button(&mut h, to, false);

    h.get_all_by_label("Copy here").next().expect("menu").click();
    h.run();
    h.run();

    // No transfer was started, so there is no overwrite prompt for the
    // file running into itself, and nothing changed on disk.
    assert!(h.state().modal.is_none(), "no overwrite prompt");
    assert!(h.state().jobs.is_empty(), "no transfer started");
    assert_eq!(names(&scratch.src()), ["alpha.txt"]);
    assert_eq!(
        std::fs::read_to_string(scratch.src().join("alpha.txt")).unwrap(),
        "alpha"
    );
}


// ------------------------------------------------------------ keyboard

/// Press `key` with `modifiers` held.
fn key(harness: &mut AppHarness, key: egui::Key, modifiers: egui::Modifiers) {
    for pressed in [true, false] {
        // The app reads held modifiers from the keyboard state.
        harness.input_mut().modifiers = modifiers;
        harness.input_mut().events.push(egui::Event::Key {
            key,
            pressed,
            modifiers,
            repeat: false,
            physical_key: None,
        });
        harness.step();
    }
}

fn click_at(harness: &mut AppHarness, pos: egui::Pos2, modifiers: egui::Modifiers) {
    pointer_moved(harness, pos);
    // The app reads held modifiers from the keyboard state.
    harness.input_mut().modifiers = modifiers;
    harness.step();
    for pressed in [true, false] {
        harness.input_mut().modifiers = modifiers;
        harness.input_mut().events.push(egui::Event::PointerButton {
            pos,
            button: egui::PointerButton::Primary,
            pressed,
            modifiers,
        });
        harness.step();
    }
}

/// Names of the rows shown highlighted in pane `pane`: the selection plus
/// the cursor row.
fn highlighted(harness: &AppHarness, pane: usize) -> Vec<String> {
    let panel = harness.state().panes[pane].cur_ref();
    let mut names: Vec<String> = panel
        .visible
        .iter()
        .enumerate()
        .filter(|(vi, &ei)| panel.cursor == *vi || panel.selected.contains(&panel.entries[ei].path))
        .map(|(_, &ei)| panel.entries[ei].name.clone())
        .collect();
    names.sort();
    names
}

fn click_row(harness: &mut AppHarness, label: &str, modifiers: egui::Modifiers) {
    let at = center_of(harness, label, 0);
    click_at(harness, at, modifiers);
}

/// An app listing a few files, with the left pane active.
fn keyboard_harness(tag: &str) -> (Scratch, AppHarness) {
    let scratch = Scratch::new(tag);
    for name in ["alpha.txt", "beta.txt", "gamma.txt", "zeta.txt"] {
        scratch.write(scratch.src().join(name), name);
    }
    let harness = harness(&scratch.src(), &scratch.dst());
    (scratch, harness)
}

#[test]
fn tab_switches_panels_without_moving_focus_onto_toolbar_buttons() {
    let (_scratch, mut h) = keyboard_harness("keys-tab");

    let mut expect = 1;
    for _ in 0..6 {
        key(&mut h, egui::Key::Tab, egui::Modifiers::NONE);
        h.run();
        assert_eq!(h.state().active, expect, "Tab switches the active pane");
        assert!(
            !h.ctx.wants_keyboard_input(),
            "no widget keeps the keyboard focus afterwards"
        );
        expect = 1 - expect;
    }
}

#[test]
fn tab_while_typing_in_the_path_bar_leaves_the_field_and_switches() {
    let scratch = Scratch::new("keys-tab-typing");
    scratch.write(scratch.src().join("alpha.txt"), "alpha");
    let mut h = harness(&scratch.src(), &scratch.dst());

    let shown = crate::panel::nfc(&std::fs::canonicalize(scratch.src()).unwrap().display().to_string());
    // The field and its text node both carry the path as their value.
    let field = h
        .get_all_by_value(shown.as_str())
        .next()
        .expect("the path field shows the folder")
        .raw_bounds()
        .expect("path field has bounds");
    click_at(&mut h, egui::pos2((field.x0 + 40.0) as f32, ((field.y0 + field.y1) / 2.0) as f32), egui::Modifiers::NONE);
    h.run();
    assert!(h.ctx.wants_keyboard_input(), "the path field has focus");

    key(&mut h, egui::Key::Tab, egui::Modifiers::NONE);
    h.run();
    h.run();

    assert_eq!(h.state().active, 1, "Tab switched the pane");
    assert!(!h.ctx.wants_keyboard_input(), "the field lost focus");
}

#[test]
fn plain_arrow_keys_move_a_single_highlight() {
    let (_scratch, mut h) = keyboard_harness("keys-plain");
    click_row(&mut h, "beta.txt", egui::Modifiers::NONE);
    h.run();
    assert_eq!(highlighted(&h, 0), ["beta.txt"]);

    key(&mut h, egui::Key::ArrowDown, egui::Modifiers::NONE);
    assert_eq!(highlighted(&h, 0), ["gamma.txt"], "one highlight, on the cursor row");

    key(&mut h, egui::Key::ArrowUp, egui::Modifiers::NONE);
    key(&mut h, egui::Key::ArrowUp, egui::Modifiers::NONE);
    assert_eq!(highlighted(&h, 0), ["alpha.txt"]);

    key(&mut h, egui::Key::End, egui::Modifiers::NONE);
    assert_eq!(highlighted(&h, 0), ["zeta.txt"]);
}

#[test]
fn shift_arrow_keys_extend_the_selection_from_where_it_started() {
    let (_scratch, mut h) = keyboard_harness("keys-shift");
    click_row(&mut h, "beta.txt", egui::Modifiers::NONE);
    h.run();

    let shift = egui::Modifiers::SHIFT;
    key(&mut h, egui::Key::ArrowDown, shift);
    assert_eq!(highlighted(&h, 0), ["beta.txt", "gamma.txt"]);
    key(&mut h, egui::Key::ArrowDown, shift);
    assert_eq!(highlighted(&h, 0), ["beta.txt", "gamma.txt", "zeta.txt"]);

    // Back up over the start: the selection follows the cursor, the start
    // stays put.
    for _ in 0..3 {
        key(&mut h, egui::Key::ArrowUp, shift);
    }
    assert_eq!(highlighted(&h, 0), ["alpha.txt", "beta.txt"]);

    key(&mut h, egui::Key::ArrowDown, egui::Modifiers::NONE);
    assert_eq!(
        highlighted(&h, 0),
        ["beta.txt"],
        "a plain arrow key collapses the selection back to one row"
    );
}

#[test]
fn shift_click_extends_from_the_row_the_selection_started_at() {
    let (_scratch, mut h) = keyboard_harness("keys-shift-click");
    click_row(&mut h, "beta.txt", egui::Modifiers::NONE);
    h.run();

    click_row(&mut h, "zeta.txt", egui::Modifiers::SHIFT);
    assert_eq!(highlighted(&h, 0), ["beta.txt", "gamma.txt", "zeta.txt"]);

    // The start of the selection is still beta.
    click_row(&mut h, "alpha.txt", egui::Modifiers::SHIFT);
    assert_eq!(highlighted(&h, 0), ["alpha.txt", "beta.txt"]);
}

#[test]
fn the_divider_drags_all_the_way_to_the_left() {
    let scratch = Scratch::new("divider-left");
    scratch.write(scratch.src().join("alpha.txt"), "alpha");
    let mut h = harness_sized(egui::vec2(1280.0, 800.0), &scratch.src(), &scratch.dst());

    let [left, _right] = h.state().pane_rects;
    let y = left.center().y;
    let mut x = left.right() + 5.0;
    pointer_moved(&mut h, egui::pos2(x, y));
    button(&mut h, egui::pos2(x, y), true);

    // Sweep left in steps; the left pane must keep following the pointer.
    while x > 120.0 {
        x -= 20.0;
        pointer_moved(&mut h, egui::pos2(x, y));
        h.step();
        let width = h.state().pane_rects[0].width();
        assert!(
            width <= (x - left.left()).max(320.0) + 1.0,
            "at pointer x={:.0} the left pane is still {:.0} wide",
            x,
            width
        );
    }
    button(&mut h, egui::pos2(x, y), false);
    h.run();

    let [left, right] = h.state().pane_rects;
    assert_eq!(left.width(), 320.0, "the left pane can shrink to its minimum");
    assert!((right.left() - left.right() - 10.0).abs() < 0.5, "the panes stay adjacent");

    // Everything the narrow pane draws stays inside it.
    for label in ["Go", "Hidden", "Ext"] {
        let r = h.get_all_by_label(label).next().expect("left pane control").raw_bounds().expect("bounds");
        assert!(
            (r.x1 as f32) <= left.right() + 0.5,
            "the left pane's {} ends at {:.0}, outside the pane ({:.0})",
            label,
            r.x1,
            left.right()
        );
    }
}






#[test]
fn only_two_quick_clicks_on_the_same_row_count_as_a_double_click() {
    let (_scratch, mut h) = keyboard_harness("keys-double-click");

    // Clicking quickly from one row to the next just selects.
    click_row(&mut h, "beta.txt", egui::Modifiers::NONE);
    click_row(&mut h, "gamma.txt", egui::Modifiers::NONE);
    assert!(
        h.state().viewer.is_none(),
        "a quick second click on another row must not open it"
    );
    assert_eq!(highlighted(&h, 0), ["gamma.txt"]);

    // Two clicks on the same row open it.
    click_row(&mut h, "gamma.txt", egui::Modifiers::NONE);
    assert_eq!(
        h.state().viewer.as_ref().map(|v| v.name.clone()),
        Some("gamma.txt".to_string())
    );
}

/// Open the file under the cursor in the editor (F4) and click into it.
/// Returns whether the editor ended up with the keyboard focus.
fn open_editor(h: &mut AppHarness) {
    key(h, egui::Key::F4, egui::Modifiers::NONE);
    h.run();
    assert!(
        h.state().viewer.as_ref().is_some_and(|v| v.editing),
        "F4 opened the file for editing"
    );
    let editor = h
        .query_all(
            egui_kittest::kittest::By::new()
                .role(egui::accesskit::Role::MultilineTextInput)
                .recursive(true),
        )
        .next()
        .expect("the editor is a multiline text input")
        .raw_bounds()
        .expect("editor has bounds");
    click_at(
        h,
        // Below the first line, in the empty part of the field: the caret
        // goes to the end of the text.
        egui::pos2((editor.x0 + 20.0) as f32, ((editor.y0 + editor.y1) / 2.0) as f32),
        egui::Modifiers::NONE,
    );
    h.run();
}

fn editor_text(h: &AppHarness) -> String {
    h.state().viewer.as_ref().map(|v| v.text.clone()).unwrap_or_default()
}

#[test]
fn while_editing_the_arrow_keys_belong_to_the_text() {
    let scratch = Scratch::new("keys-editor");
    for name in ["alpha.txt", "beta.txt", "gamma.txt"] {
        scratch.write(scratch.src().join(name), "line\n");
    }
    let mut h = harness(&scratch.src(), &scratch.dst());
    click_row(&mut h, "beta.txt", egui::Modifiers::NONE);
    h.run();
    open_editor(&mut h);
    assert!(h.ctx.wants_keyboard_input(), "clicking the editor gives it focus");

    // The arrow keys move the text caret, not the file cursor.
    key(&mut h, egui::Key::ArrowDown, egui::Modifiers::NONE);
    assert_eq!(h.state().panes[0].cur_ref().cursor, 1);
    assert!(h.ctx.wants_keyboard_input(), "still editing");
}

#[test]
fn typing_in_the_editor_keeps_the_editing_focus() {
    let scratch = Scratch::new("keys-editor-typing");
    scratch.write(scratch.src().join("notes.txt"), "line one\nline two\n");
    let mut h = harness(&scratch.src(), &scratch.dst());
    open_editor(&mut h);

    // The first edit puts the "* " marker into the window title. The window
    // used to become a different window then, and the editor lost the focus,
    // so only one character could ever be typed at a time.
    for c in ["x", "y", "z"] {
        h.input_mut().events.push(egui::Event::Text(c.to_string()));
        h.step();
        h.run();
        assert!(h.ctx.wants_keyboard_input(), "still editing after typing '{c}'");
    }
    assert_eq!(editor_text(&h), "line one\nline two\nxyz");
}

#[test]
fn tab_in_the_editor_indents_and_does_not_switch_panes() {
    let scratch = Scratch::new("keys-editor-tab");
    scratch.write(scratch.src().join("notes.txt"), "line one\nline two\n");
    let mut h = harness(&scratch.src(), &scratch.dst());
    open_editor(&mut h);

    key(&mut h, egui::Key::Tab, egui::Modifiers::NONE);
    h.run();
    assert_eq!(editor_text(&h), "line one\nline two\n\t");
    assert!(h.ctx.wants_keyboard_input(), "the editor keeps the focus");
    assert_eq!(h.state().active, 0, "Tab did not switch panes");
    assert!(h.state().viewer.as_ref().is_some_and(|v| v.dirty), "the edit counts as a change");

    // Shift+Tab takes the indentation back out.
    key(&mut h, egui::Key::Tab, egui::Modifiers::SHIFT);
    h.run();
    assert_eq!(editor_text(&h), "line one\nline two\n");
    assert!(h.ctx.wants_keyboard_input(), "still editing");

    // Leaving the editor (Escape returns the keys to the panels), Tab
    // switches panes as usual.
    key(&mut h, egui::Key::Escape, egui::Modifiers::NONE);
    h.run();
    key(&mut h, egui::Key::Tab, egui::Modifiers::NONE);
    h.run();
    assert_eq!(h.state().active, 1);
}

#[test]
fn tab_inside_a_dialog_moves_between_its_controls_and_leaves_the_panes_alone() {
    let (_scratch, mut h) = keyboard_harness("keys-tab-dialog");
    key(&mut h, egui::Key::F7, egui::Modifiers::NONE);
    h.run();
    assert!(h.state().modal.is_some(), "F7 opened the new-folder dialog");

    key(&mut h, egui::Key::Tab, egui::Modifiers::NONE);
    h.run();
    h.run();

    assert_eq!(h.state().active, 0, "Tab in a dialog does not switch panes");
    assert!(h.state().modal.is_some(), "the dialog is still open");
}

#[test]
fn the_table_fills_the_pane_whichever_way_the_divider_moves() {
    let scratch = Scratch::new("divider-both-ways");
    scratch.write(scratch.src().join("alpha.txt"), "alpha");
    let mut h = harness_sized(egui::vec2(1280.0, 800.0), &scratch.src(), &scratch.dst());

    // Where the left pane's last column ends, relative to its pane.
    let last_column_gap = |h: &AppHarness| {
        let left = h.state().pane_rects[0];
        let ext = h.get_all_by_label("Ext").next().expect("column header").raw_bounds().expect("bounds");
        left.right() - ext.x1 as f32
    };

    let gap_before = last_column_gap(&h);
    assert!(gap_before < 30.0, "initially the table fills the pane (gap {:.0})", gap_before);

    let y = h.state().pane_rects[0].center().y;
    let drag_to = |h: &mut AppHarness, from: f32, to: f32| {
        pointer_moved(h, egui::pos2(from, y));
        button(h, egui::pos2(from, y), true);
        let mut x = from;
        while (x - to).abs() > 0.5 {
            // Steps of at most 20px, so the last one lands on the target.
            x += (to - x).signum() * (to - x).abs().min(20.0);
            pointer_moved(h, egui::pos2(x, y));
        }
        pointer_moved(h, egui::pos2(to, y));
        button(h, egui::pos2(to, y), false);
        h.run();
        h.run();
    };

    // Widen the left pane a lot.
    let divider_at = h.state().pane_rects[0].right() + 5.0;
    drag_to(&mut h, divider_at, 1000.0);
    let wide = h.state().pane_rects[0].width();
    assert!(wide > 900.0, "the divider moved right (left pane {:.0})", wide);
    let gap = last_column_gap(&h);
    assert!(gap < 30.0, "the table grew with the pane (gap {:.0})", gap);

    // Then shrink it to its minimum.
    let divider_at = h.state().pane_rects[0].right() + 5.0;
    drag_to(&mut h, divider_at, 100.0);
    let narrow = h.state().pane_rects[0].width();
    assert_eq!(narrow, 320.0);
    let gap = last_column_gap(&h);
    assert!((-0.5..=30.0).contains(&gap), "the table shrank with the pane (gap {:.0})", gap);
}

#[test]
fn table_columns_fit_their_content_and_fill_the_pane() {
    let scratch = Scratch::new("table-columns");
    for name in ["budget.xlsx", "CHANGELOG.md", "config.json", "notes.txt"] {
        let c = name.to_string();
        scratch.write(scratch.src().join(name), &c);
    }
    let mut h = harness_sized(egui::vec2(1180.0, 620.0), &scratch.src(), &scratch.dst());
    for _ in 0..4 {
        h.step();
    }

    // A column sizes itself to its content. If the content wrapped, the
    // column would come out too narrow for it and the dates would be split
    // over two lines.
    let dates: Vec<_> = h
        .query_all(
            egui_kittest::kittest::By::new()
                .label_contains("2026-")
                .recursive(true),
        )
        .collect();
    assert!(!dates.is_empty(), "the listing shows modification dates");
    for date in dates {
        let r = date.raw_bounds().expect("date cell has bounds");
        assert!(
            r.y1 - r.y0 <= 20.0,
            "the date {:?} is on one line ({:.0} tall)",
            date.label(),
            r.y1 - r.y0
        );
    }

    // The table reaches the right edge of its pane.
    let left = h.state().pane_rects[0];
    let ext = h.get_all_by_label("Ext").next().expect("column header").raw_bounds().expect("bounds");
    assert!(left.right() - (ext.x1 as f32) < 30.0);
}

