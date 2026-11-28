#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <optional>

// Finding files and folders by what they are called, how big they are, how
// recently they changed, and what is in them. None of this needs a window:
// search() works until it is through or stopped, so it belongs on a worker
// thread, and it calls back on that thread.
namespace FileSearch {

constexpr int kDefaultMaxResults = 100000;                  // a search stops here unless told otherwise
constexpr qint64 kDefaultContentLimit = 100 * 1024 * 1024;  // files up to this size are looked into

// What to look for. As it is, everything there is gets found.
struct Query {
    // Files, folders, or both.
    enum class What {
        Files,
        Folders,
        Both,
    };

    // To look inside files for text as well. Text is looked for as UTF-8.
    struct Content {
        QString text;          // what a file must have in it
        bool isRegex = false;  // `text` is a regular expression, in which ^ and $ are the ends of a line
        // Files up to this size in bytes are looked into; -1: whatever the size.
        qint64 maxSize = kDefaultContentLimit;
    };

    // Where to look: the contents of each of these folders.
    QStringList folders;

    // What the names must match. Several words, separated by spaces, must all
    // be in the name, as in the filter of the panes. A word without a wildcard
    // may be anywhere in the name; one with "*" or "?" must be the whole name:
    // "report" finds "2024-report.pdf", "*.jpg" finds jpg files but not
    // "photo.jpg.bak". Empty: every name.
    QString name;
    // `name` is a regular expression instead, which may match anywhere in the
    // name. It matches the way Unicode means it, so \b and \w know their way
    // around Korean and other letters that are not Latin.
    bool nameIsRegex = false;
    // For names and for text alike.
    bool caseSensitive = false;

    What what = What::Both;
    // Whether hidden files are looked at too: names that start with a dot, or
    // that have the hidden attribute on Windows. Hidden folders are not looked
    // into either unless this is set.
    bool includeHidden = false;

    // Not set: files are not looked into. Binary files, which have a zero
    // byte near the start, are always left alone.
    std::optional<Content> content;

    // How big files must be, in bytes; -1: no limit. Only files have a size
    // here, so a size range keeps every folder out.
    qint64 minSize = -1;
    qint64 maxSize = -1;

    // When it was last changed: from `modifiedAfter` up to `modifiedBefore`,
    // both included. An invalid date is no limit.
    QDateTime modifiedAfter;
    QDateTime modifiedBefore;

    // Whether the folders inside the folders are looked through too.
    bool recursive = true;
    // The search ends once this many results are found; 0 or less: never
    // because of that.
    int maxResults = kDefaultMaxResults;
};

// A stretch of text: from character `from` up to, but not including, `to`.
struct Span {
    int from = 0;
    int to = 0;
};

// Something that was found.
struct Match {
    QString path;        // where it is, absolute
    QString folder;      // the folder it is in
    // What it is called, in composed Unicode: `spans` counts in this. Disk
    // names may be decomposed (macOS keeps them as they were created), while
    // typed text is composed.
    QString name;
    qint64 size = -1;    // files only; -1 when it can't be told
    QDateTime modified;  // invalid when it can't be told
    bool isDir = false;
    bool hidden = false;
    // Where the name pattern matched. Nothing to show when no name was looked
    // for, or when what matched is empty.
    QList<Span> spans;
    // The line the text was found on first, counting from 1, and how that line
    // reads; 0 and empty when no text was looked for.
    int line = 0;
    QString lineText;
};

// How far a search has got.
struct Progress {
    qint64 folders = 0;  // folders looked into
    qint64 entries = 0;  // files and folders seen in them
    QString current;     // the folder being looked into
};

// Called on the thread search() runs on: with every match, and in between
// every so often with a null `match` to tell how far the search has got.
using Callback = std::function<void(const Match *match, const Progress &progress)>;

// How a search went.
struct Summary {
    QString error;          // why nothing was looked at; empty when all went well
    bool stopped = false;   // `stop` was set before everything was looked at
    bool capped = false;    // it ended at the result limit; there may be more
    qint64 folders = 0;     // looked into
    qint64 entries = 0;     // seen
    qint64 unreadable = 0;  // folders that could not be read
};

// What is wrong with `query`, for the user to read: a pattern that is no
// regular expression, a folder that is no folder, a range that is empty. Empty
// when it can be searched with.
QString validate(const Query &query);

// Whether an entry called `name` (a folder's when `isDir`, hidden when
// `hidden`) gets through everything but the text search. `size` and `modified`
// are what the entry has; -1 and an invalid date when they can't be told.
// Names match however they are spelled in Unicode: decomposed and composed
// forms of the same name are the same name.
bool matches(const Query &query, const QString &name, bool isDir, qint64 size,
             const QDateTime &modified, bool hidden = false);

// Looks through the folders of the query and reports what is found through
// `callback`, until all is seen, `stop` is set, or the result limit is
// reached. Links to folders are not followed, so that a circle of them can't
// lead round forever; a folder the query names is looked into even when it is
// a link, since it was asked for. Folders that can't be read are skipped.
// Returns how it went.
Summary search(const Query &query, Callback callback, std::atomic<bool> &stop);

}  // namespace FileSearch
