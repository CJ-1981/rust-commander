//! Prints which candidate toolbar emoji are present in egui's default fonts.
//! Run with: cargo run --example emoji_coverage

use eframe::egui;

fn main() {
    let ctx = egui::Context::default();
    ctx.begin_pass(egui::RawInput::default());
    // Mirror the app's Korean fallback font registration.
    let mut fonts = egui::FontDefinitions::default();
    fonts.font_data.insert(
        "noto_sans_kr".to_owned(),
        std::sync::Arc::new(egui::FontData::from_static(include_bytes!(
            "../resources/fonts/NotoSansKR.ttf"
        ))),
    );
    for family in [
        egui::FontFamily::Proportional,
        egui::FontFamily::Monospace,
        egui::FontFamily::Name("Button".into()),
    ] {
        fonts.families.entry(family).or_default().push("noto_sans_kr".to_owned());
    }
    ctx.set_fonts(fonts);
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
            ('🔧', "tools wrench"),
            ('한', "hangul syllable"),
            ('글', "hangul syllable 2"),
            ('🧰', "tools toolbox (alt)"),
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
