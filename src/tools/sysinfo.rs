use eframe::egui;
use sysinfo::{Disks, System};

use crate::panel::fmt_size;

/// System information panel: OS, CPU, memory and mounted disks.
#[derive(Default)]
pub struct SysTool {
    pub info: Vec<(String, String)>,
    pub disks: Vec<(String, String, String)>,
}

impl SysTool {
    pub fn ui(&mut self, ui: &mut egui::Ui) {
        ui.add_space(4.0);
        if self.info.is_empty() || ui.button("Refresh").clicked() {
            self.refresh();
        }

        egui::Grid::new("sys_info")
            .num_columns(2)
            .spacing([16.0, 4.0])
            .striped(true)
            .show(ui, |ui| {
                for (k, v) in &self.info {
                    ui.strong(k);
                    ui.label(v);
                    ui.end_row();
                }
            });

        ui.add_space(10.0);
        ui.strong("Disks");
        egui::Grid::new("sys_disks")
            .num_columns(3)
            .spacing([16.0, 4.0])
            .striped(true)
            .show(ui, |ui| {
                ui.strong("Mount point");
                ui.strong("Label");
                ui.strong("Space");
                ui.end_row();
                for (mount, label, space) in &self.disks {
                    ui.monospace(mount);
                    ui.label(label);
                    ui.label(space);
                    ui.end_row();
                }
            });
    }

    fn refresh(&mut self) {
        let sys = System::new_all();
        let cpus = sys.cpus();
        let cpu_model = cpus
            .first()
            .map(|c| c.brand().trim().to_string())
            .unwrap_or_default();
        let cpu_model = if cpu_model.is_empty() {
            "unknown".to_string()
        } else {
            cpu_model
        };

        let mut info: Vec<(String, String)> = vec![
            ("Operating system".to_string(), opt_str(System::name())),
            ("OS version".to_string(), opt_str(System::os_version())),
            ("Kernel".to_string(), opt_str(System::kernel_version())),
            ("Host name".to_string(), opt_str(System::host_name())),
            ("Architecture".to_string(), std::env::consts::ARCH.to_string()),
            ("CPU cores".to_string(), cpus.len().to_string()),
            ("CPU model".to_string(), cpu_model),
            ("Total memory".to_string(), fmt_size(sys.total_memory())),
            ("Available memory".to_string(), fmt_size(sys.available_memory())),
        ];
        if let Ok(cwd) = std::env::current_dir() {
            info.push(("Working directory".to_string(), cwd.display().to_string()));
        }
        self.info = info;

        self.disks = Disks::new_with_refreshed_list()
            .list()
            .iter()
            .map(|d| {
                (
                    d.mount_point().display().to_string(),
                    d.name().to_string_lossy().into_owned(),
                    format!(
                        "{} free / {} total",
                        fmt_size(d.available_space()),
                        fmt_size(d.total_space())
                    ),
                )
            })
            .collect();
    }
}

fn opt_str(v: Option<String>) -> String {
    v.unwrap_or_else(|| "unknown".to_string())
}
