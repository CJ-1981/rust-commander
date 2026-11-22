//! Prints which candidate toolbar emoji are present in egui's default fonts.
//! Run with: cargo run --example emoji_coverage

use eframe::egui;

fn main() {
    let ctx = egui::Context::default();
    ctx.begin_pass(egui::RawInput::default());
    ctx.fonts(|fonts| {
        let candidates: &[(char, &str)] = &[
            ('🔍', "view / inspect"),
            ('👁', "view (alt)"),
            ('✏', "edit pencil"),
            ('📝', "edit memo (alt)"),
            ('📋', "copy clipboard"),
            ('⇄', "move / swap arrows"),
            ('🔁', "move (alt)"),
            ('📁', "new folder"),
            ('🗂', "folder lines (alt)"),
            ('🗒', "rename notepad"),
            ('🗑', "delete trash"),
            ('✖', "delete x (alt)"),
            ('↗', "open external"),
            ('⬈', "open external (alt)"),
            ('📄', "file"),
        ];
        for (c, what) in candidates {
            let body = egui::FontId::proportional(14.0);
            println!(
                "{}  U+{:05X}  {:<22}  {}",
                if fonts.has_glyph(&body, *c) { "OK " } else { "MISSING" },
                *c as u32,
                what,
                c
            );
        }
    });
}
