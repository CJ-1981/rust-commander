use std::fs;
use std::io;
use std::path::{Path, PathBuf};

/// Copy a file or directory tree into `dst_dir`, keeping the source name.
/// Existing files are overwritten; directories are merged.
pub fn copy_path(src: &Path, dst_dir: &Path) -> io::Result<PathBuf> {
    let name = src.file_name().ok_or_else(|| {
        io::Error::new(io::ErrorKind::InvalidInput, "invalid source path")
    })?;
    let dst = dst_dir.join(name);
    copy_into(src, &dst)?;
    Ok(dst)
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
/// back to copy + delete across devices/volumes.
pub fn move_path(src: &Path, dst_dir: &Path) -> io::Result<PathBuf> {
    let name = src.file_name().ok_or_else(|| {
        io::Error::new(io::ErrorKind::InvalidInput, "invalid source path")
    })?;
    let dst = dst_dir.join(name);
    match fs::rename(src, &dst) {
        Ok(()) => Ok(dst),
        Err(_) => {
            copy_into(src, &dst)?;
            remove_path(src)?;
            Ok(dst)
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
