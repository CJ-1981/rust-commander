# Rust Commander

A dual-pane file manager for macOS and Windows, in the spirit of Total Commander: two file lists side by side, driven by the keyboard, with a viewer, an editor and a set of tools — search, multi-rename, duplicate finder, checksums, file comparison — built in.

The name has stayed, although this version is no longer written in Rust: it is C++20 with Qt 6. The earlier Rust/egui version is on the `main` branch, and the releases up to and including v0.1.9 are that version.

## Status

The commander core ("Milestone 1") lives on the `qt` branch. **The Qt version has not been released.** There is nothing to download yet, and the builds on the [releases page](https://github.com/CJ-1981/rust-commander/releases) are the older Rust/egui version. To try this version, build it from the `qt` branch as described below.

## Features

### Two panes

- Two file panes side by side, with a divider that can be dragged. A double-click on the divider gives both panes the same width; `Ctrl+U` swaps them.
- Folder tabs in each pane: `+` or `Ctrl+T` opens a tab on the folder shown, tabs can be dragged into order, a double-click on a tab gives it a name, and `×` closes it (the last one stays). The tabs of both panes are back at the next start.
- A line over the tabs marks the active pane, which is the one the shortcuts act on. Clicking in a pane, or pressing `Tab`, makes it the active one.
- `Alt+Left` and `Alt+Right` go back and forward through the folder history of the tab. Coming back to a folder brings back its cursor and selection, as far as they still exist.

### Selecting

- With the mouse: a click selects one row, `Ctrl`+click adds or removes a row, and `Shift`+click selects a range. Pressing an unselected row and moving the pointer selects the rows in between; pressing empty space draws a rubber band over the rows it touches.
- With the keyboard: the arrow keys, `Shift`+arrows, `Ctrl`+arrows, `Insert` or `Space`, and `Ctrl+A`. The reference below has them all.

### Copying, moving and deleting

- Drag and drop between the panes, in both directions, to copy or to move. Release and choose *Copy here* or *Move here*, or hold `Ctrl` to copy and `Shift` or `Alt` to move. Dropping onto a folder row puts the files into that folder.
- Files dragged in from Finder or Explorer are copied. Dragging out to Finder or Explorer copies too: nothing is ever taken away from where it was. A folder cannot be dropped into itself or into its own contents.
- `F5` copies and `F6` moves the selection to the other pane. `F7` creates a folder, `Ctrl+N` a file, and `F2` renames.
- File operations run on a worker thread, with a progress window that has a *Stop* button. One operation runs at a time and the others wait their turn. When something with that name is already there, the choice is *Replace*, *Skip*, *Replace all*, *Skip all* or *Cancel*. Copying a folder onto a folder of the same name merges them. A move within one volume is a rename; across volumes it is a copy and a delete.
- `F8` or `Del` moves the selection to the Trash, after a confirmation. `Shift+Del` deletes it for good, also after a confirmation. The home folder, and everything above it up to the root of the volume, are never deleted.
- When an operation is over, both panes show what is there now.

### Looking at files

- `F3` opens the item under the cursor in the built-in viewer, read-only. `F4` opens it in the editor, where `Ctrl+S` saves and closing with unsaved changes asks first. `Tab` indents in the editor. Files are saved in the encoding they had (a file that isn't UTF-8 is taken byte for byte, so nothing is replaced), with the line endings they had. Text typed in an encoding that can't hold it turns the file into UTF-8, and the window says so. A file that is not text is not shown; a button opens it with its application instead.
- `Enter` or a double-click enters a folder. A file opens with the application the system uses for it.
- Columns for name, size, type and date modified; click a header to sort. `Ctrl+H` shows hidden files. `Ctrl+L` computes the sizes of folders in the background.
- A live filter (`Ctrl+F`): only the names containing all the typed words are listed.
- The path bar shows the folder as buttons to click through. Clicking empty space in it, or typing, turns it into a field: `~` and paths relative to the folder shown are understood, folder names are completed while typing, `Enter` goes there and `Esc` gives up.
- A folder refreshes by itself when other programs change it.

### The tools

The File menu gathers the tools. Each works on the folder shown and the selection of the active pane.

- **Search files** (`Ctrl+F3`): finds files and folders by name, and on request by what is in them — plain text or a regular expression — with limits on size and on the date modified, hidden files included or not, and a cap on how much is found. The search runs on a worker thread; results arrive while it goes. From the results: open, show the file in its folder, copy.
- **Multi-rename** (`Ctrl+M`): renames many files at once. A mask says what the new name is made of — the old name and extension in ranges (`[N2-5]`), a counter (`[C10+5:3]`), the parts of the file's date (`[Y][M][D]`), and upper, lower or capitalized from a point on (`[U]`, `[L]`, `[F]`) — then find and replace (regular expressions with groups), underscores, and the case. A preview lists what every name will become before anything is applied.
- **Find duplicate files** (`Ctrl+Shift+D`): files with the same content are grouped, told apart first by size, then by a taste of their bytes, then by a full hash. Marking chooses what stays; the marked files are removed, to the Trash or for good, and only while they are still what was scanned.
- **Checksums**: hashes files and folders — MD5, SHA-1, SHA-256, SHA-512 or CRC32 — and checks files against a checksum file in the shape `sha256sum` writes: the sum, two spaces, the name.
- **Compare files**: two texts side by side with the lines that differ tinted and the parts that differ marked. *Next* and *Previous difference* walk the changes, the sides can be swapped, and spaces at the end of a line, line endings and blank lines can be ignored. Two files selected in a pane are compared right away; with less chosen, the window opens with the fields to fill in.

### Around it

- A light theme, a dark theme, or whichever the system uses (View → Theme); the last one follows the system when it changes.
- Favorites (Go → Favorites; `Ctrl+D` adds the folder shown) and the volumes of the computer (Go → Volumes).
- A terminal in the folder shown: `Ctrl+Alt+T` or the *Terminal* button. On Windows it starts Windows Terminal when that is installed, and Command Prompt otherwise.
- The function keys as buttons along the bottom of the window, for anyone who would rather click: `F3` View, `F4` Edit, `F5` Copy, `F6` Move, `F7` New folder, `Shift+F6` Rename, `F8` Delete and `Shift+F8` Delete for good.
- The window size and position, the divider, the theme, the favorites and the tabs are remembered between starts.

## Keyboard reference

The shortcuts act on the active pane. `Ctrl` below is the Command key on macOS. Hidden files are `Ctrl+Shift+H` there, because `Ctrl+H` hides the app. The tab keys are `Ctrl+Alt+→` and `Ctrl+Alt+←` there (and `Ctrl+Alt+Space` marks the row under the cursor), because the system keeps `Ctrl+Tab` and `Ctrl+Space` for itself.

### In the file list

| Key | Action |
| --- | --- |
| `Up`, `Down` | Move to the row above or below, which becomes the selection |
| `Shift`+`Up`, `Shift`+`Down` | Extend the selection |
| `Ctrl`+`Up`, `Ctrl`+`Down` | Move the cursor without changing the selection |
| `PgUp`, `PgDn` | One page up or down |
| `Home`, `End` | First or last row |
| `Insert`, `Space` | Mark the row under the cursor and move down one row; on a marked row, unmark it |
| `Ctrl`+`Space` | Mark or unmark the row under the cursor, and stay on it |
| `Enter` | Enter the folder, or open the file with its application |
| `Backspace` | Go to the parent folder |
| `F2`, `Shift`+`F6` | Rename the item under the cursor |
| `F5` | Copy the selection to the other pane |
| `F6` | Move the selection to the other pane |
| `F7` | New folder |
| `Ctrl`+`N` | New file |
| `F8`, `Del` | Move the selection to the Trash, after asking |
| `Shift`+`F8`, `Shift`+`Del` | Delete the selection for good, after asking |
| `Ctrl`+`A` | Select everything |
| Menu key, right-click | Context menu |

### Going places

| Key | Action |
| --- | --- |
| `Alt`+`Left`, `Alt`+`Right` | Back and forward in the history of the tab |
| `Alt`+`Up` | To the parent folder |
| `Alt`+`Home` | To the home folder |
| `Ctrl`+`G` | Type a path: the path bar becomes a field with the whole path selected |
| `Ctrl`+`R` | Refresh |

In the path field, `Enter` goes to the typed path and `Esc` returns to the folder buttons, dropping what was typed.

### Tabs, panes and the view

| Key | Action |
| --- | --- |
| `Ctrl`+`T` | New tab, on the folder shown |
| `Ctrl`+`Tab`, `Ctrl`+`Shift`+`Tab` | Next tab, previous tab |
| `Ctrl`+`W` | Close the tab |
| `Tab`, `Shift`+`Tab` | Switch to the other pane. Not in a text field, where `Tab` moves on to the next control. |
| `Ctrl`+`U` | Swap the two panes |
| Double-click on the divider | Give both panes the same width |
| `Ctrl`+`F` | Go to the filter |
| `Ctrl`+`H` | Show or hide hidden files |
| `Ctrl`+`L` | Show or hide the sizes of folders |
| `Ctrl`+`D` | Add the folder shown to the favorites |
| `Ctrl`+`Alt`+`T` | Open a terminal in the folder shown |
| `F3`, `F4` | View, edit: the item under the cursor |
| `Ctrl`+`S` | Save in the editor |
| `Ctrl`+`F3` | Search files |
| `Ctrl`+`M` | Multi-rename the selection |
| `Ctrl`+`Shift`+`D` | Find duplicate files in the folder shown |
| `Ctrl`+`Q` | Quit |

### With the mouse

| Mouse | Action |
| --- | --- |
| Click | Select one row |
| `Ctrl`+click | Add a row to the selection, or remove it |
| `Shift`+click | Select a range |
| Press an unselected row and move the pointer | Select the rows in between |
| Press empty space and move the pointer | Rubber band over the rows it touches |
| Press a selected row and move the pointer | Drag the selection |
| `Ctrl`, or `Shift` or `Alt`, held when dropping | Copy, or move, without asking |
| Double-click | Enter the folder, or open the file |

## Building from source

### Requirements

- CMake 3.21 or newer, and Ninja.
- Qt 6.10 with its base (which has Widgets, Concurrent and Test) and `qtsvg`. The CMake files accept Qt 6.5 or newer, but the project is tested with Qt 6.10.3.
- On macOS, the Xcode command line tools. On Windows, Visual Studio 2022 with its C++ tools (MSVC).

### Getting Qt

Qt can be fetched with [aqtinstall](https://github.com/miurahr/aqtinstall):

```sh
python3 -m pip install aqtinstall
aqt install-qt mac desktop 6.10.3 clang_64 --outputdir ~/Qt                 # into ~/Qt/6.10.3/macos
aqt install-qt windows desktop 6.10.3 win64_msvc2022_64 --outputdir C:\Qt   # into C:\Qt\6.10.3\msvc2022_64
```

`qtsvg` is among what these commands install by default; `--archives qtbase qtsvg` limits the download to what this project needs, which is what CI does. Qt's package for macOS is universal, for Apple Silicon and Intel.

### macOS

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.3/macos"
cmake --build build
open build/bin/rust-commander.app
```

The result is `build/bin/rust-commander.app`, which needs macOS 13 or newer. Adding `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` builds it for both kinds of Mac at once, as CI does.

### Windows

In a prompt where the MSVC tools are set up, such as *x64 Native Tools Command Prompt for VS 2022*:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=C:\Qt\6.10.3\msvc2022_64"
cmake --build build
.\build\bin\rust-commander.exe
```

The result is `build\bin\rust-commander.exe`. A build uses the Qt it was built with, so on Windows Qt's `bin` folder has to be on the `PATH` to start it from the build folder.

To give a build to someone else, let Qt's tools put the libraries next to it: `macdeployqt build/bin/rust-commander.app` on macOS, `windeployqt build\bin\rust-commander.exe` on Windows.

## Tests

```sh
ctest --test-dir build --output-on-failure --timeout 120
```

The tests run on Qt's offscreen platform, which CMake sets for them, so no window appears. There are nine test programs, for the file model, the file view, the file operations, the system hooks, the operation runner, the path bar, the pane, the main window and the viewer.

One test, `TestFileOps::trash`, is left out unless `RC_TEST_TRASH=1` is set, because it puts files into the real Trash (the Recycle Bin on Windows). CI sets it:

```sh
RC_TEST_TRASH=1 ctest --test-dir build --output-on-failure --timeout 120
```

`--render <file.png> [--theme light|dark] [--path <folder>]` makes the program draw its window to a PNG file and exit, with nothing appearing on screen. It uses the offscreen platform and keeps its settings apart from the real ones, so it is safe to run from scripts.

## Continuous integration

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) builds, tests and packages on every push and pull request: a macOS app bundle for Apple Silicon and Intel, and a Windows folder with its Qt libraries, both as zip archives attached to the run. On macOS the packaged program is also started from its archive, without a window, to check that it runs with only what the package holds.

A GitHub Release with the two archives is created only when a tag of the form `v<number>` is pushed. Pushing a branch publishes nothing.

## License

The application is under the MIT license; see [LICENSE](LICENSE).

Qt is linked dynamically and used under the GNU LGPL v3. The Qt libraries are not part of this repository's source; the packaging puts them next to the program.

The icons come from [Lucide](https://lucide.dev), under the ISC license.
