use std::fs;
use std::io;
use std::path::Path;

/// Whether an existing target should be replaced or left alone.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Overwrite {
    Yes,
    No,
}

/// Result of a single transfer item.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TransferOutcome {
    Copied,
    Skipped,
}

/// Copy a file or directory tree into `dst_dir`, keeping the source name.
/// When the target already exists, `ask` decides whether to overwrite
/// (`Overwrite::Yes`) or skip (`Overwrite::No`); directories are merged.
pub fn copy_path(
    src: &Path,
    dst_dir: &Path,
    mut ask: impl FnMut(&Path, bool) -> Overwrite,
) -> io::Result<TransferOutcome> {
    let name = src.file_name().ok_or_else(|| {
        io::Error::new(io::ErrorKind::InvalidInput, "invalid source path")
    })?;
    let dst = dst_dir.join(name);
    if dst.exists() && ask(&dst, src.is_dir()) == Overwrite::No {
        return Ok(TransferOutcome::Skipped);
    }
    copy_into(src, &dst)?;
    Ok(TransferOutcome::Copied)
}

fn copy_into(src: &Path, dst: &Path) -> io::Result<()> {
    if src.is_dir() {
        fs::create_dir_all(dst)?;
        for entry in fs::read_dir(src)? {
            let entry = entry?;
            copy_into(&entry.path(), &dst.join(entry.file_name()))?;
        }
        Ok(())
    } else {
        if let Some(parent) = dst.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::copy(src, dst).map(|_| ())
    }
}

/// Move a file or directory. Uses a fast rename when possible and falls
/// back to copy + delete across devices/volumes. `ask` handles existing
/// targets the same way as `copy_path` (rename would silently replace them).
pub fn move_path(
    src: &Path,
    dst_dir: &Path,
    mut ask: impl FnMut(&Path, bool) -> Overwrite,
) -> io::Result<TransferOutcome> {
    let name = src.file_name().ok_or_else(|| {
        io::Error::new(io::ErrorKind::InvalidInput, "invalid source path")
    })?;
    let dst = dst_dir.join(name);
    if dst.exists() && ask(&dst, src.is_dir()) == Overwrite::No {
        return Ok(TransferOutcome::Skipped);
    }
    match fs::rename(src, &dst) {
        Ok(()) => Ok(TransferOutcome::Copied),
        Err(_) => {
            copy_into(src, &dst)?;
            remove_path(src)?;
            Ok(TransferOutcome::Copied)
        }
    }
}

/// Permanently delete a file or directory tree.
pub fn remove_path(src: &Path) -> io::Result<()> {
    if src.is_dir() {
        fs::remove_dir_all(src)
    } else {
        fs::remove_file(src)
    }
}

/// Move a file or directory to the platform's trash.
pub fn trash_path(src: &Path) -> io::Result<()> {
    trash::delete(src).map_err(|e| match e {
        trash::Error::Unknown { description } => io::Error::other(description),
        other => io::Error::other(other.to_string()),
    })
}

/// Open a terminal window working in `dir`.
pub fn open_terminal(dir: &Path) -> io::Result<()> {
    #[cfg(target_os = "macos")]
    {
        std::process::Command::new("open")
            .args(["-a", "Terminal", &dir.display().to_string()])
            .spawn()?;
    }
    #[cfg(target_os = "windows")]
    {
        std::process::Command::new("cmd")
            .args(["/C", "start", "", "cmd"])
            .current_dir(dir)
            .spawn()?;
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        let candidates: Vec<std::process::Command> = {
            let mut v = Vec::new();
            if let Ok(term) = std::env::var("TERMINAL") {
                let mut c = std::process::Command::new(term);
                c.arg("-e").arg("sh");
                v.push(c);
            }
            let mut c = std::process::Command::new("gnome-terminal");
            c.arg(format!("--working-directory={}", dir.display()));
            v.push(c);
            let mut c = std::process::Command::new("konsole");
            c.arg("--workdir").arg(dir);
            v.push(c);
            let mut c = std::process::Command::new("x-terminal-emulator");
            c.arg(format!("--working-directory={}", dir.display()));
            v.push(c);
            v
        };
        let mut last_err = None;
        for mut c in candidates {
            c.current_dir(dir);
            match c.spawn() {
                Ok(_) => return Ok(()),
                Err(e) => last_err = Some(e),
            }
        }
        return Err(last_err.unwrap_or_else(|| {
            io::Error::new(io::ErrorKind::NotFound, "no terminal emulator found")
        }));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    fn tmp_dir(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("rc-ops-test-{}", tag));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn copy_skips_existing_when_asked() {
        let src_dir = tmp_dir("copy-skip-src");
        let dst_dir = tmp_dir("copy-skip-dst");
        fs::write(src_dir.join("a.txt"), "new").unwrap();
        fs::write(dst_dir.join("a.txt"), "old").unwrap();

        let out = copy_path(&src_dir.join("a.txt"), &dst_dir, |_, _| Overwrite::No).unwrap();
        assert_eq!(out, TransferOutcome::Skipped);
        assert_eq!(fs::read_to_string(dst_dir.join("a.txt")).unwrap(), "old");
        let _ = fs::remove_dir_all(&src_dir);
        let _ = fs::remove_dir_all(&dst_dir);
    }

    #[test]
    fn copy_overwrites_when_allowed() {
        let src_dir = tmp_dir("copy-ow-src");
        let dst_dir = tmp_dir("copy-ow-dst");
        fs::write(src_dir.join("a.txt"), "new").unwrap();
        fs::write(dst_dir.join("a.txt"), "old").unwrap();

        let out = copy_path(&src_dir.join("a.txt"), &dst_dir, |_, _| Overwrite::Yes).unwrap();
        assert_eq!(out, TransferOutcome::Copied);
        assert_eq!(fs::read_to_string(dst_dir.join("a.txt")).unwrap(), "new");
        let _ = fs::remove_dir_all(&src_dir);
        let _ = fs::remove_dir_all(&dst_dir);
    }

    #[test]
    fn move_skip_leaves_both() {
        let src_dir = tmp_dir("move-skip-src");
        let dst_dir = tmp_dir("move-skip-dst");
        fs::write(src_dir.join("a.txt"), "data").unwrap();
        fs::write(dst_dir.join("a.txt"), "existing").unwrap();

        let out = move_path(&src_dir.join("a.txt"), &dst_dir, |_, _| Overwrite::No).unwrap();
        assert_eq!(out, TransferOutcome::Skipped);
        assert!(src_dir.join("a.txt").exists());
        assert_eq!(fs::read_to_string(dst_dir.join("a.txt")).unwrap(), "existing");
        let _ = fs::remove_dir_all(&src_dir);
        let _ = fs::remove_dir_all(&dst_dir);
    }

    #[test]
    fn fresh_copy_asks_nothing() {
        let src_dir = tmp_dir("copy-fresh-src");
        let dst_dir = tmp_dir("copy-fresh-dst");
        fs::write(src_dir.join("b.txt"), "hello").unwrap();

        let mut asked = 0;
        let out = copy_path(&src_dir.join("b.txt"), &dst_dir, |_, _| {
            asked += 1;
            Overwrite::No
        })
        .unwrap();
        assert_eq!(out, TransferOutcome::Copied);
        assert_eq!(asked, 0);
        let _ = fs::remove_dir_all(&src_dir);
        let _ = fs::remove_dir_all(&dst_dir);
    }
}
