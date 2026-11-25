# Rust Commander

<p align="center">
  <img src="resources/icon.png" alt="Rust Commander icon" width="128" />
</p>

A **Total Commander-style dual-pane file manager** with a built-in **utility toolbox**, written in 100% Rust. Runs natively on **Windows** and **macOS** (and builds on Linux too).

[![CI](https://github.com/CJ-1981/rust-commander/actions/workflows/ci.yml/badge.svg)](https://github.com/CJ-1981/rust-commander/actions/workflows/ci.yml)

## Features

### File navigation (dual pane)

- Two independent directory panels with an active-panel highlight
- Single click selects, Ctrl+Click adds to the selection, Shift+Click selects a range, Insert/Space toggles selection at the cursor
- Sort by name, size, modification date or extension - click the column headers. The name column fills the pane; the Size column can be dragged wider
- Editable path bar, Home button, drive/volume picker (Windows drive letters, macOS `/Volumes`), refresh, hidden-file toggle
- Per-pane **folder tabs**: `+` opens a tab at the current folder, `Ctrl+T`/`Ctrl+W` add/close, double-click or right-click a tab to rename it; tabs survive restarts
- **Safer file operations**: deletes go to the Trash (permanent option available), and copying/moving onto existing files asks — Overwrite, Skip, or remember the choice for the rest of the batch
- **Navigation**: per-tab back/forward history (`Alt+←`/`Alt+→`), Favorites menu for pinned folders, live name filter (`Ctrl+F`)
- **Power tools**: recursive folder sizes with item counts, open-a-terminal-here button, checksum verification in the hash tool, and a two-file text comparison with a colored unified diff
- Built-in text viewer (F3) and editor (F4) with save (Ctrl+S)
- Open files in the built-in viewer (double-click, F3) or with the system default application (toolbar button, right-click menu)
- **Drag and drop**: press a file - or a whole selection - and carry it into the other pane to copy or move it. Release and choose, or hold Shift (move) or Ctrl / Command (copy) to skip the menu. Drop onto a folder row to target that folder. Files dropped from Finder or Explorer onto a pane get the same choice. Dragging inside a pane still range-selects.
- Active panel is operated with Total Commander-style keyboard shortcuts

### File operations (background threads with progress + cancel)

- **F5** Copy selection to the opposite panel
- **F6** Move selection to the opposite panel, **Shift+F6** rename
- **F7** New folder
- **F8/Delete** Move selection to the Trash; the same dialog offers *Delete permanently*
- Recursive copy/move/delete of folder trees; fast rename on the same volume, copy+delete fallback across volumes
- All long operations run on worker threads with live progress in the status bar and a Stop button

### Utility toolbox

- **Hash calculator**: MD5 + SHA-256 streamed in chunks, with progress bar and copy-to-clipboard
- **File search**: recursive name search (case-insensitive substring), live progress, capped results
- **Duplicate finder**: content-based duplicate detection (group by size, then hash), wasted-space summary
- **Multi rename**: rename many files at once with `*` (keep the original part) and `[N]` (counter) masks, with a live preview
- **Compare files**: side-by-side diff of two text files
- **System info**: OS, kernel, CPU, memory and all mounted disks with free/total space

## Keyboard shortcuts

| Key | Action |
| --- | --- |
| Tab | Switch active panel |
| Up / Down | Move cursor |
| Shift + Up / Down | Extend the selection |
| Home / End | First / last entry |
| Shift + Home / End | Extend the selection to the first / last entry |
| Enter | Open folder / view file |
| Backspace | Go to parent folder |
| Insert / Space | Toggle selection at cursor |
| Ctrl+Click | Add to selection |
| Shift+Click | Range selection |
| Press + Drag | Range selection with the mouse |
| Drag to other pane | Copy / move the dragged files (choose on drop) |
| Double-click | Enter folder / view file |
| F3 | View file (read-only) |
| F4 | Edit file |
| F5 | Copy to opposite panel |
| F6 | Move to opposite panel |
| Shift+F6 | Rename |
| F7 | New folder |
| F8 / Delete | Move selection to the Trash |
| Ctrl+S | Save file in editor |
| Tab (in the editor) | Indent the line or selection; Shift+Tab removes the indentation |
| Ctrl+N | New empty file |
| Ctrl+T / Ctrl+W | New / close folder tab |
| Ctrl+F | Focus the folder filter |
| Alt+Left / Right | Back / forward in folder history |
| Esc | Close dialog |

## Build

Prerequisites: [Rust](https://rustup.rs) 1.77 or newer.

```sh
cargo build --release
```

The binary is written to `target/release/rust-commander` (`rust-commander.exe` on Windows).

## Tests

```sh
cargo test
```

Besides unit tests for the file operations and tools, this runs headless UI tests (`src/app_tests.rs`): the real interface driven by simulated mouse events, with no window. They cover drag-and-drop, range selection and the pane layout.

Run it directly during development:

```sh
cargo run --release
```

## Platform notes

- **Windows**: drive letters A-Z are auto-detected in the Drives menu.
- **macOS**: mounted volumes appear via `/Volumes`. The `.app` bundles produced by CI are ad-hoc signed (no Apple Developer ID), so macOS shows an "unidentified developer" warning on first launch. Right-click > Open once, or approve under System Settings > Privacy & Security > "Open Anyway". If macOS reports the app as "damaged", clear the download quarantine flag:
  ```sh
  xattr -cr "Rust Commander.app"
  ```
- **Deletes go to the Trash** by default. *Delete permanently* (in the delete dialog and the right-click menu) is immediate and cannot be undone.

## CI / Releases

GitHub Actions (`.github/workflows/ci.yml`) builds release binaries on every push and pull request for:

| Target | Runner | Artifact |
| --- | --- | --- |
| `x86_64-pc-windows-msvc` | `windows-latest` | `RustCommander-windows-x64.zip` |
| `aarch64-apple-darwin` | `macos-latest` | `RustCommander-macos-arm64.zip` (`.app` bundle) |
| `x86_64-apple-darwin` | `macos-latest` | `RustCommander-macos-x64.zip` (`.app` bundle) |

Each CI run also runs the test suite on macOS and on Windows. Artifacts are attached to every CI run; pushing a tag like `v0.1.0` additionally publishes a GitHub Release with all three artifacts attached (the upload is retried, and the run fails if fewer than three files end up on the release).

### Cut a new release

Releases are fully automated - one command publishes new binaries:

```sh
git tag v0.1.8
git push origin v0.1.2
```

The pipeline then builds all three targets, packages them (macOS `.app` bundles with an auto-generated icon, Windows zip), and creates the GitHub Release with the binaries attached. The version inside the macOS bundle metadata is synced automatically from the pushed tag.

## License

MIT for the project code. Bundled fonts have their own licenses: Noto Sans KR (resources/fonts) is distributed under the [SIL Open Font License 1.1](https://openfontlicense.org/).

## License

MIT - see [LICENSE](LICENSE).
