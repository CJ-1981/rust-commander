#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

// Finding files whose content is the same, and choosing which of them to
// keep. Nothing here needs a window, and nothing here starts a thread: find()
// works until it is done or told to stop, so it belongs on a worker thread
// (see DuplicatesWindow).
namespace DuplicateFinder {

// How much of a file is read before all of it is.
constexpr qint64 kHeadSize = 64 * 1024;

// What to look for.
struct Options {
    // The folders that are scanned, one after the other. A folder that lies in
    // another one is scanned once.
    QStringList folders;
    // The file names that count, as QDir::match reads them: wild cards, and
    // several of them separated by spaces or semicolons, e.g. "*.jpg *.png".
    // Empty means every file.
    QString pattern;
    bool subfolders = true;
    // Files below this size are left out. It doesn't apply to empty files:
    // those are decided by includeEmpty alone, as no size can be asked of them.
    qint64 minSize = 1024;
    bool includeEmpty = false;
};

// One file of a group.
struct File {
    QString path;      // absolute
    QDateTime modified;
};

// Files with the same content, and so of the same size.
struct Group {
    qint64 size = 0;  // of each file
    QList<File> files;
    // What the copies beyond the first take up.
    qint64 wasted() const { return size * (files.size() - 1); }
};

// How far a scan is.
struct Progress {
    enum class Stage {
        Walking,    // the folders are read
        Comparing,  // the files that share a size are read
    };

    Stage stage = Stage::Walking;
    qint64 files = 0;       // that passed the filters so far
    qint64 bytesRead = 0;   // to compare them
    qint64 bytesTotal = 0;  // there is to read for that; 0 until the walk is over
};

struct Result {
    QList<Group> groups;  // the most wasted space first
    qint64 files = 0;     // that passed the filters
    qint64 unreadable = 0;  // folders that could not be read; the ones given included
    bool stopped = false;

    qint64 wasted() const;
};

// Called now and then from the thread find() runs on, and once more at the end.
using Report = std::function<void(const Progress &)>;

// Scans the folders for files with the same content. A file is compared by
// size first, then by the start of its content, and only those that are still
// alike by everything in them. Links are left alone, whether to files or to
// folders, so are folders deeper than 64 levels and folders that can't be
// read. When `stop` is set, find() returns soon with what it found by then.
Result find(const Options &options, const std::atomic<bool> &stop, const Report &report = {});

// How the copies of a group are chosen to stay.
struct Rule {
    enum class Kind {
        First,        // the first in the list
        Newest,
        Oldest,
        InFolder,     // the copies in `folder`; when there is none, all stay
        NotInFolder,  // all but the copies in `folder`; when all are in it, the first stays
    };

    Kind kind = Kind::First;
    // For the two folder rules. A file is in a folder when it lies in it or
    // below it.
    QString folder;
};

// The files of `group` that `rule` removes, as indexes into files(), in order.
// One file of the group always stays, whatever the rule says.
QList<int> toRemove(const Group &group, const Rule &rule);

// Whether `path` lies in `folder` or below it.
bool isInFolder(const QString &path, const QString &folder);

// Whether the file is still what it was when it was scanned: a file, not a
// link, of `size` bytes with the same date. A copy that changed since is no
// longer known to be a duplicate, so it is not one to remove.
bool unchanged(const File &file, qint64 size);

}  // namespace DuplicateFinder
