#include "DuplicateFinder.h"

#include <QCollator>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace {

using DuplicateFinder::kHeadSize;

constexpr int kMaxDepth = 64;  // as in FileOps: what lies deeper is left out
constexpr qint64 kChunkSize = 1024 * 1024;  // large files are read in pieces of this size
constexpr qint64 kEntriesPerReport = 64;   // entries between two looks at whether to report
// Not more often than this while only the numbers move, so that whoever shows
// them is not flooded.
constexpr auto kReportEvery = std::chrono::milliseconds(50);

// Paths differ in case only on Windows.
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

fs::path toPath(const QString &text)
{
#ifdef Q_OS_WIN
    return fs::path(text.toStdWString());
#else
    return fs::path(text.toStdString());
#endif
}

// The path as Qt writes it, with "/" for the separator. False for a name that
// is not UTF-8 on a Unix system: such a file could be neither shown nor
// opened by its name.
bool toQString(const fs::path &path, QString *text)
{
#ifdef Q_OS_WIN
    *text = QDir::fromNativeSeparators(QString::fromStdWString(path.native()));
    return true;
#else
    const std::string native = path.native();
    *text = QString::fromUtf8(native.data(), qsizetype(native.size()));
    return !text->contains(QChar::ReplacementCharacter);
#endif
}

// Whether `path` is `folder` itself or lies in it.
bool isAtOrInside(const QString &path, const QString &folder)
{
    if (path.compare(folder, kPathCase) == 0) {
        return true;
    }
    const QString within = folder.endsWith(QLatin1Char('/')) ? folder : folder + QLatin1Char('/');
    return path.startsWith(within, kPathCase);
}

// What tells a file from the same file under another name: on Unix the volume
// and the inode. Windows gives nothing of the kind that could be had here;
// distinct() makes do there without it.
QString identityOf(const QString &path)
{
#ifdef Q_OS_UNIX
    struct stat status;
    if (::stat(QFile::encodeName(path).constData(), &status) != 0) {
        return {};
    }
    return QStringLiteral("%1:%2").arg(quint64(status.st_dev)).arg(quint64(status.st_ino));
#else
    Q_UNUSED(path);
    return {};
#endif
}

// The files with those of them left out that are the same file as one before
// them: a hard link, or one file reached by two ways. Two names of one file
// are one copy. Counted as two, the group would show space that removing one
// frees nothing of, and the "copy" that stayed would be gone with the other.
QList<DuplicateFinder::File> distinct(const QList<DuplicateFinder::File> &files)
{
    QList<DuplicateFinder::File> result;
    QSet<QString> identities;
    QList<int> severalNames;  // indexes in result of files that may have another name
    for (const DuplicateFinder::File &file : files) {
        const QString identity = identityOf(file.path);
        if (identities.contains(identity)) {
            continue;
        }
        if (!identity.isEmpty()) {
            identities.insert(identity);
        } else {
            // Where nothing tells files apart, only a file that is known to
            // have several names is worth comparing.
            std::error_code ec;
            if (fs::hard_link_count(toPath(file.path), ec) > 1 && !ec) {
                severalNames.append(int(result.size()));
            }
        }
        result.append(file);
    }

    QList<int> twice;
    for (qsizetype a = 0; a < severalNames.size(); ++a) {
        for (qsizetype b = a + 1; b < severalNames.size(); ++b) {
            std::error_code ec;
            if (fs::equivalent(toPath(result.at(severalNames.at(a)).path),
                               toPath(result.at(severalNames.at(b)).path), ec)
                && !ec) {
                twice.append(severalNames.at(b));
            }
        }
    }
    if (twice.isEmpty()) {
        return result;
    }
    QList<DuplicateFinder::File> kept;
    for (qsizetype i = 0; i < result.size(); ++i) {
        if (!twice.contains(int(i))) {
            kept.append(result.at(i));
        }
    }
    return kept;
}

// The folders to walk: the ones given, without those that are no folder, that
// are given twice, or that lie in another one. The last is what keeps every
// file from being seen twice.
QStringList scanRoots(const QStringList &folders, qint64 *unreadable)
{
    QStringList paths;
    QStringList real;
    for (const QString &folder : folders) {
        const QFileInfo info(folder);
        // A link is followed here: a link to a folder is a way of giving it.
        if (!info.isDir()) {
            ++*unreadable;
            continue;
        }
        // As the file system spells it, so that two ways to one folder compare equal.
        QString canonical = QDir::cleanPath(info.canonicalFilePath());
        if (canonical.isEmpty()) {
            canonical = QDir::cleanPath(info.absoluteFilePath());
        }
        bool known = false;
        for (qsizetype i = real.size() - 1; i >= 0; --i) {
            // One given before may lie in this one now.
            if (isAtOrInside(real.at(i), canonical)) {
                real.removeAt(i);
                paths.removeAt(i);
            } else if (isAtOrInside(canonical, real.at(i))) {
                known = true;
            }
        }
        if (known) {
            continue;
        }
        real.append(canonical);
        paths.append(QDir::cleanPath(info.absoluteFilePath()));
    }
    return paths;
}

// Whether the filters let a file through.
bool wanted(const DuplicateFinder::Options &options, const QString &name, qint64 size)
{
    if (size == 0 ? !options.includeEmpty : size < options.minSize) {
        return false;
    }
    const QString pattern = options.pattern.trimmed();
    return pattern.isEmpty() || QDir::match(pattern, name);
}

// Files that share their size.
struct SizeGroup {
    qint64 size = 0;
    QList<DuplicateFinder::File> files;
};

// One scan, from the walk to the groups.
class Scan {
public:
    Scan(const DuplicateFinder::Options &options, const std::atomic<bool> &stop,
         const DuplicateFinder::Report &report)
        : m_options(options)
        , m_stop(stop)
        , m_report(report)
    {
    }

    DuplicateFinder::Result run();

private:
    void walk(const fs::path &folder, int depth);
    void lookAt(const fs::directory_entry &entry, int depth);
    void compare(const QList<SizeGroup> &candidates);
    // Shows the progress, at most every kReportEvery unless `now`.
    void report(bool now);
    // SHA-256 of the first `bytes` of the file, which must still have `size`
    // bytes. Empty when it doesn't, when it can't be read, or when stopped.
    QByteArray digest(const QString &path, qint64 size, qint64 bytes);

    const DuplicateFinder::Options &m_options;
    const std::atomic<bool> &m_stop;
    const DuplicateFinder::Report &m_report;
    QHash<qint64, QList<DuplicateFinder::File>> m_bySize;
    DuplicateFinder::Progress m_progress;
    DuplicateFinder::Result m_result;
    qint64 m_entries = 0;  // looked at since the progress was last asked to be shown
    std::chrono::steady_clock::time_point m_lastReport;
};

QByteArray Scan::digest(const QString &path, qint64 size, qint64 bytes)
{
    QFile file(path);
    // One that changed since it was listed is not compared.
    if (!file.open(QIODevice::ReadOnly) || file.size() != size) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(int(qMin<qint64>(kChunkSize, bytes)), Qt::Uninitialized);
    qint64 done = 0;
    while (done < bytes) {
        if (m_stop.load()) {
            return {};
        }
        const qint64 got = file.read(buffer.data(), int(qMin<qint64>(buffer.size(), bytes - done)));
        if (got <= 0) {
            // Shorter than it was, or not to be read.
            return {};
        }
        hash.addData(QByteArrayView(buffer.constData(), got));
        done += got;
        m_progress.bytesRead += got;
    }
    if (bytes == size && !file.atEnd()) {
        // Longer than it was.
        return {};
    }
    return hash.result();
}

void Scan::report(bool now)
{
    if (!m_report) {
        return;
    }
    const auto time = std::chrono::steady_clock::now();
    if (!now && time - m_lastReport < kReportEvery) {
        return;
    }
    m_lastReport = time;
    m_report(m_progress);
}

void Scan::lookAt(const fs::directory_entry &entry, int depth)
{
    std::error_code ec;
    // Neither counted nor followed: what a link points at is no business of this scan.
    if (entry.is_symlink(ec)) {
        return;
    }
    if (entry.is_directory(ec)) {
        if (m_options.subfolders && depth < kMaxDepth) {
            walk(entry.path(), depth + 1);
        }
        return;
    }
    if (!entry.is_regular_file(ec)) {
        return;
    }
    const auto size = entry.file_size(ec);
    if (ec) {
        return;
    }
    QString path;
    if (!toQString(entry.path(), &path) || !wanted(m_options, QFileInfo(path).fileName(), qint64(size))) {
        return;
    }
    m_bySize[qint64(size)].append(DuplicateFinder::File{path, QFileInfo(path).lastModified()});
    ++m_progress.files;
}

void Scan::walk(const fs::path &folder, int depth)
{
    std::error_code ec;
    fs::directory_iterator it(folder, ec);
    if (ec) {
        // Not to be read: left out, and said so in the result.
        ++m_result.unreadable;
        return;
    }
    while (it != fs::directory_iterator()) {
        // Looked at for every entry: once stopped, every folder on the way
        // out ends at once, wherever the one that noticed the stop was nested.
        if (m_stop.load()) {
            return;
        }
        const fs::directory_entry entry = *it;
        it.increment(ec);
        lookAt(entry, depth);
        if (++m_entries >= kEntriesPerReport) {
            m_entries = 0;
            report(false);
        }
        if (ec) {
            // The rest of this folder is not to be read either.
            ++m_result.unreadable;
            return;
        }
    }
}

void Scan::compare(const QList<SizeGroup> &candidates)
{
    m_progress.stage = DuplicateFinder::Progress::Stage::Comparing;
    for (const SizeGroup &candidate : candidates) {
        const qint64 whole = candidate.size > kHeadSize ? candidate.size : 0;
        m_progress.bytesTotal += candidate.files.size() * (qMin(candidate.size, kHeadSize) + whole);
    }
    report(true);

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);

    for (const SizeGroup &candidate : candidates) {
        // The start of each file tells most groups of a size apart, for little
        // reading.
        QHash<QByteArray, QList<int>> byHead;
        for (qsizetype i = 0; i < candidate.files.size(); ++i) {
            const QByteArray head =
                    digest(candidate.files.at(i).path, candidate.size, qMin(candidate.size, kHeadSize));
            if (m_stop.load()) {
                break;
            }
            if (!head.isEmpty()) {
                byHead[head].append(int(i));
            }
            report(false);
        }
        if (m_stop.load()) {
            break;
        }

        for (const QList<int> &alike : std::as_const(byHead)) {
            // A start that no other file of this size shares needs no whole
            // file read: that is what the first stage is for.
            if (alike.size() < 2) {
                continue;
            }
            QList<QList<int>> same;
            if (candidate.size > kHeadSize) {
                // What is alike so far is read whole.
                QHash<QByteArray, QList<int>> byContent;
                for (int i : alike) {
                    const QByteArray content = digest(candidate.files.at(i).path, candidate.size,
                                                      candidate.size);
                    if (m_stop.load()) {
                        break;
                    }
                    if (!content.isEmpty()) {
                        byContent[content].append(i);
                    }
                    report(false);
                }
                if (m_stop.load()) {
                    break;
                }
                for (const QList<int> &list : std::as_const(byContent)) {
                    if (list.size() >= 2) {
                        same.append(list);
                    }
                }
            } else {
                // The head was the whole file already.
                same.append(alike);
            }

            for (const QList<int> &list : same) {
                QList<DuplicateFinder::File> files;
                for (int i : list) {
                    files.append(candidate.files.at(i));
                }
                files = distinct(files);
                if (files.size() < 2) {
                    continue;
                }
                std::sort(files.begin(), files.end(),
                          [&collator](const DuplicateFinder::File &a, const DuplicateFinder::File &b) {
                              return collator.compare(a.path, b.path) < 0;
                          });
                m_result.groups.append(DuplicateFinder::Group{candidate.size, files});
            }
        }
    }
}

DuplicateFinder::Result Scan::run()
{
    const QStringList roots = scanRoots(m_options.folders, &m_result.unreadable);
    for (const QString &root : roots) {
        if (m_stop.load()) {
            break;
        }
        walk(toPath(root), 0);
    }

    // Only files that share their size can be alike.
    QList<SizeGroup> candidates;
    for (auto it = m_bySize.cbegin(); it != m_bySize.cend(); ++it) {
        if (it.value().size() >= 2) {
            candidates.append(SizeGroup{it.key(), it.value()});
        }
    }
    m_bySize.clear();
    // The most to gain comes first, so that a scan stopped early has found
    // the largest part of it.
    std::sort(candidates.begin(), candidates.end(), [](const SizeGroup &a, const SizeGroup &b) {
        const qint64 gain = a.size * (a.files.size() - 1) - b.size * (b.files.size() - 1);
        return gain != 0 ? gain > 0 : a.size > b.size;
    });

    compare(candidates);

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(m_result.groups.begin(), m_result.groups.end(),
              [&collator](const DuplicateFinder::Group &a, const DuplicateFinder::Group &b) {
                  if (a.wasted() != b.wasted()) {
                      return a.wasted() > b.wasted();
                  }
                  if (a.size != b.size) {
                      return a.size > b.size;
                  }
                  return collator.compare(a.files.first().path, b.files.first().path) < 0;
              });

    m_result.files = m_progress.files;
    m_result.stopped = m_stop.load();
    report(true);
    return m_result;
}

}  // namespace

namespace DuplicateFinder {

qint64 Result::wasted() const
{
    qint64 sum = 0;
    for (const Group &group : groups) {
        sum += group.wasted();
    }
    return sum;
}

Result find(const Options &options, const std::atomic<bool> &stop, const Report &report)
{
    return Scan(options, stop, report).run();
}

QList<int> toRemove(const Group &group, const Rule &rule)
{
    const qsizetype count = group.files.size();
    if (count < 2) {
        // Nothing to choose from.
        return {};
    }
    // What stays is chosen first, and something always does.
    QList<bool> stays(count, false);
    switch (rule.kind) {
    case Rule::Kind::First:
        stays[0] = true;
        break;
    case Rule::Kind::Newest:
    case Rule::Kind::Oldest: {
        const bool newest = rule.kind == Rule::Kind::Newest;
        qsizetype best = 0;
        for (qsizetype i = 1; i < count; ++i) {
            const bool better = newest ? group.files.at(i).modified > group.files.at(best).modified
                                       : group.files.at(i).modified < group.files.at(best).modified;
            if (better) {
                best = i;
            }
        }
        stays[best] = true;
        break;
    }
    case Rule::Kind::InFolder:
    case Rule::Kind::NotInFolder: {
        const bool inside = rule.kind == Rule::Kind::InFolder;
        bool any = false;
        for (qsizetype i = 0; i < count; ++i) {
            if (isInFolder(group.files.at(i).path, rule.folder) == inside) {
                stays[i] = true;
                any = true;
            }
        }
        if (!any) {
            // No copy is as the rule wants. To keep those in a folder where
            // none is can't be done: all stay. To remove those of a folder
            // they all are in would be all of them: the first stays.
            if (inside) {
                stays.fill(true);
            } else {
                stays[0] = true;
            }
        }
        break;
    }
    }

    QList<int> remove;
    qsizetype staying = 0;
    for (qsizetype i = 0; i < count; ++i) {
        if (stays.at(i)) {
            ++staying;
        } else {
            remove.append(int(i));
        }
    }
    return staying > 0 ? remove : QList<int>();
}

bool isInFolder(const QString &path, const QString &folder)
{
    if (folder.isEmpty()) {
        return false;
    }
    return isAtOrInside(path, QDir::cleanPath(QFileInfo(folder).absoluteFilePath()));
}

bool unchanged(const File &file, qint64 size)
{
    const QFileInfo info(file.path);
    return info.isFile() && !info.isSymLink() && info.size() == size && info.lastModified() == file.modified;
}

}  // namespace DuplicateFinder
