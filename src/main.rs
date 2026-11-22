#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

//! Rust Commander - a Total Commander style dual-pane file manager
//! with a built-in utility toolbox. Works on Windows, macOS and Linux.

mod app;
mod jobs;
mod ops;
mod panel;
mod tools;
mod viewer;

use eframe::egui;

/// Application icon, embedded at compile time.
const ICON_PNG: &[u8] = include_bytes!("../resources/icon.png");

fn main() -> Result<(), eframe::Error> {
    let icon = load_icon(ICON_PNG);

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title(format!("Rust Commander v{}", env!("CARGO_PKG_VERSION")))
            .with_inner_size([1280.0, 820.0])
            .with_min_inner_size([940.0, 560.0])
            .with_icon(icon),
        ..Default::default()
    };

    eframe::run_native(
        "Rust Commander",
        options,
        Box::new(|_cc| Ok(Box::new(app::CommanderApp::new(_cc)))),
    )
}

/// Decode the embedded PNG into an RGBA icon usable by the window manager.
fn load_icon(bytes: &[u8]) -> egui::IconData {
    let img = image::load_from_memory(bytes).expect("failed to decode embedded icon");
    let rgba = img.to_rgba8();
    egui::IconData {
        width: rgba.width(),
        height: rgba.height(),
        rgba: rgba.into_raw(),
    }
}
