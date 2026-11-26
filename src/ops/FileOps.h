#pragma once

#include <QDir>
#include <QString>
#include <QStringList>

// Copying, moving, renaming and deleting files and folders, without any user
// interface. These block while they work; long ones run on a worker thread
// (see OperationRunner) and report through `Progress` and ask about
// overwrites through `OverwritePrompt`, which the caller provides.
namespace FileOps {

// What to do with a target that already exists.
enum class Existing {
    Cancel,
    Skip,
    Overwrite,
    SkipAll,
    OverwriteAll,
};

// How a move is carried out.
struct MoveOptions {
    // Rename when source and target are on the same volume, which keeps a
    // file's dates and attributes as they are. Off, everything is copied and
    // the source deleted, as a move onto another volume has to be anyway.
    bool renameOnSameVolume = true;
};

// Reports as the work proceeds.
class Progress {
public:
    virtual ~Progress() = default;

    // Work done so far out of the work there is (bytes, plus one per file
    // and folder), and what is being worked on. Returns false to stop.
    virtual bool tick(qint64 done, qint64 total, const QString &current) = 0;
};

// Decides what happens to targets that already exist.
class OverwritePrompt {
public:
    virtual ~OverwritePrompt() = default;

    virtual Existing ask(const QString &target) = 0;
};

// How much was done.
struct Totals {
    int files = 0;
    int folders = 0;
    qint64 bytes = 0;
};

// Everything below clears `error` when called and sets it when it failed or
// was stopped. It stays empty when the user stopped it.
//
// Copying a folder into a folder of the same name merges them; each file
// that already exists is asked about, unless "all" was chosen before.

bool copy(const QStringList &sources, const QString &targetFolder, OverwritePrompt &prompt,
          Progress &progress, QString *error, Totals *totals = nullptr);
bool move(const QStringList &sources, const QString &targetFolder, OverwritePrompt &prompt,
          Progress &progress, QString *error, Totals *totals = nullptr,
          const MoveOptions &options = MoveOptions());

// Deletes for good. `home` is never deleted, nor is any folder above it up to
// the root of its volume; that is decided before anything else is done.
bool remove(const QStringList &paths, Progress &progress, QString *error, Totals *totals = nullptr,
            const QString &home = QDir::homePath());
bool trash(const QStringList &paths, QString *error, Totals *totals = nullptr,
           const QString &home = QDir::homePath());

// Quick operations, fine to call on the GUI thread.
bool makeFolder(const QString &path, QString *error);
bool makeFile(const QString &path, QString *error);
// `newName` is a plain name, in the same folder.
bool rename(const QString &path, const QString &newName, QString *error);

// Size of a file, or of everything in a folder (links are not followed).
qint64 sizeOf(const QString &path);

// Whether `name` can be given to a new file or folder, or used in a rename.
// The rules are the strictest of the systems the app runs on, so that files
// made on one stay valid on the others. If not, `reason` says why not.
bool isAllowedName(const QString &name, QString *reason = nullptr);

// What remove() and trash() refuse: `home`, any folder above it up to the
// root of its volume, and that root.
bool isProtected(const QString &path, const QString &home = QDir::homePath());

}  // namespace FileOps
