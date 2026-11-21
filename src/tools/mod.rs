pub mod dup;
pub mod hash;
pub mod search;
pub mod sysinfo;

use eframe::egui;

use crate::jobs::Job;
use dup::DupTool;
use hash::HashTool;
use search::SearchTool;
use sysinfo::SysTool;

#[derive(Clone, Copy, PartialEq, Eq, Default)]
pub enum ToolTab {
    #[default]
    Hash,
    Search,
    Duplicates,
    System,
}

/// The utility toolbox: one window with tabbed tools.
#[derive(Default)]
pub struct Toolbox {
    pub show: bool,
    pub tab: ToolTab,
    pub hash: HashTool,
    pub search: SearchTool,
    pub dup: DupTool,
    pub sys: SysTool,
}

impl Toolbox {
    pub fn ui(&mut self, ui: &mut egui::Ui, jobs: &mut Vec<Job>) {
        ui.horizontal(|ui| {
            let tabs = [
                (ToolTab::Hash, "Hash calculator"),
                (ToolTab::Search, "File search"),
                (ToolTab::Duplicates, "Duplicate finder"),
                (ToolTab::System, "System info"),
            ];
            for (t, label) in tabs {
                if ui.selectable_label(self.tab == t, label).clicked() {
                    self.tab = t;
                }
            }
        });
        ui.separator();
        egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            match self.tab {
                ToolTab::Hash => self.hash.ui(ui, jobs),
                ToolTab::Search => self.search.ui(ui, jobs),
                ToolTab::Duplicates => self.dup.ui(ui, jobs),
                ToolTab::System => self.sys.ui(ui),
            }
        });
    }
}
