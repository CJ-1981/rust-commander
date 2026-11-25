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
