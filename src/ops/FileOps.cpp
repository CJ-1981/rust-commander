#include "FileOps.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace {

constexpr qint64 kChunkSize = 1024 * 1024;          // large files are copied in pieces of this size
constexpr qint64 kFastCopyLimit = 8 * 1024 * 1024;  // up to this size a file is copied whole
constexpr int kMaxDepth = 64;                       // links to folders can't lead further than this

QString tr(const char *text)
{
    return QCoreApplication::translate("FileOps", text);
}

void clear(QString *error)
{
    if (error) {
        error->clear();
    }
}

// Records why an operation failed; an empty message means the user stopped it.
bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

bool fail(QString *error, const QString &path, const std::error_code &ec)
{
    return fail(error, QStringLiteral("%1: %2").arg(path, QString::fromStdString(ec.message())));
}

fs::path toPath(const QString &text)
{
#ifdef Q_OS_WIN
    return fs::path(text.toStdWString());
#else
    return fs::path(text.toStdString());
#endif
}

QString toQString(const fs::path &path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.native());
#else
    return QString::fromStdString(path.native());
#endif
}

bool samePath(const QString &a, const QString &b)
{
#ifdef Q_OS_WIN
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

bool tooDeep(const fs::path &path, QString *error)
{
    return fail(error, tr("%1: the folders are nested too deeply.").arg(toQString(path)));
}

// What there is to do for an item: its size, and how many files and folders
// it holds. Links are counted, what they point to is not.
struct Work {
    qint64 bytes = 0;
    qint64 files = 0;
    qint64 folders = 0;

    qint64 units() const { return bytes + files + folders; }
};

Work measure(const fs::path &path, int depth = 0)
{
    Work work;
    std::error_code ec;
    if (fs::is_symlink(path, ec)) {
        // Counted as a file; what it points to is not looked at.
        ++work.files;
        return work;
    }
    if (fs::is_directory(path, ec)) {
        ++work.folders;
        if (depth >= kMaxDepth) {
            return work;
        }
        std::error_code itError;
        fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, itError);
        for (const fs::directory_entry &entry : it) {
            const Work inside = measure(entry.path(), depth + 1);
            work.bytes += inside.bytes;
            work.files += inside.files;
            work.folders += inside.folders;
        }
    } else {
        ++work.files;
        const auto size = fs::file_size(path, ec);
        if (!ec) {
            work.bytes = qint64(size);
        }
    }
    return work;
}

bool sameVolume(const fs::path &from, const fs::path &to)
{
    // Asked for folders that exist, since the target may not yet.
    const QStorageInfo source(toQString(from.parent_path()));
    const QStorageInfo target(toQString(to.parent_path()));
    return source.device() == target.device() && source.rootPath() == target.rootPath();
}

// Does the work for copy(), move() and remove().
class Engine {
public:
    Engine(FileOps::OverwritePrompt *prompt, FileOps::Progress &progress)
        : m_prompt(prompt)
        , m_progress(progress)
    {
    }

    bool copy(const QStringList &sources, const QString &targetFolder, QString *error,
              FileOps::Totals *totals);
    bool move(const QStringList &sources, const QString &targetFolder,
              const FileOps::MoveOptions &options, QString *error, FileOps::Totals *totals);
    bool remove(const QStringList &paths, const QString &home, QString *error,
                FileOps::Totals *totals);

private:
    // Whether an item may be put onto a target path.
    enum class Placement {
        Go,      // the target is free, or something else
        Same,    // the target is the item itself: nothing to do
        Inside,  // the target lies inside a folder that is being copied
    };

    // What became of a target that was in the way.
    enum class Target {
        Free,     // it wasn't there, or it was removed: go ahead
        Skipped,  // left alone: on with the next item
        Stopped,  // the user cancelled
    };

    bool begin(const QStringList &paths, QString *error);
    static Placement placement(const fs::path &from, const fs::path &to);
    // What to do about `target`, which exists.
    FileOps::Existing decide(const fs::path &target, QString *error);
    // Asks about `to` and, when the answer is "overwrite", makes room.
    Target clearTarget(const fs::path &to, QString *error);
    bool copyEntry(const fs::path &from, const fs::path &to, int depth, QString *error);
    bool copyFile(const fs::path &from, const fs::path &to, QString *error);
    bool moveEntry(const fs::path &from, const fs::path &to, int depth,
                   const FileOps::MoveOptions &options, QString *error);
    bool tick(const QString &current, QString *error);
    void add(const Work &work);

    FileOps::OverwritePrompt *m_prompt;  // null when nothing can be asked
    FileOps::Progress &m_progress;
    bool m_skipAll = false;
    bool m_overwriteAll = false;
    qint64 m_done = 0;
    qint64 m_total = 0;
    FileOps::Totals m_totals;
};

bool Engine::begin(const QStringList &paths, QString *error)
{
    m_total = 0;
    for (const QString &path : paths) {
        m_total += measure(toPath(QDir::cleanPath(path))).units();
    }
    return tick({}, error);
}

bool Engine::tick(const QString &current, QString *error)
{
    if (m_progress.tick(m_done, m_total, current)) {
        return true;
    }
    // Stopped by the user: no error to show.
    return fail(error, {});
}

void Engine::add(const Work &work)
{
    m_done += work.units();
    m_totals.bytes += work.bytes;
    m_totals.files += int(work.files);
    m_totals.folders += int(work.folders);
}

Engine::Placement Engine::placement(const fs::path &from, const fs::path &to)
{
    std::error_code ec;
    if (fs::equivalent(from, to, ec)) {
        return Placement::Same;
    }
    const QString source = toQString(from);
    const QString target = toQString(to);
    if (samePath(source, target)) {
        return Placement::Same;
    }
    if (target.startsWith(source + QLatin1Char('/'))) {
        return Placement::Inside;
    }
    return Placement::Go;
}

FileOps::Existing Engine::decide(const fs::path &target, QString *error)
{
    if (m_overwriteAll) {
        return FileOps::Existing::Overwrite;
    }
    if (m_skipAll) {
        return FileOps::Existing::Skip;
    }
    if (!m_prompt) {
        return FileOps::Existing::Cancel;
    }
    const FileOps::Existing answer = m_prompt->ask(toQString(target));
    switch (answer) {
    case FileOps::Existing::OverwriteAll:
        m_overwriteAll = true;
        return FileOps::Existing::Overwrite;
    case FileOps::Existing::SkipAll:
        m_skipAll = true;
        return FileOps::Existing::Skip;
    case FileOps::Existing::Cancel:
        fail(error, {});
        return answer;
    default:
        return answer;
    }
}

Engine::Target Engine::clearTarget(const fs::path &to, QString *error)
{
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(to, ec))) {
        return Target::Free;
    }
    const FileOps::Existing decision = decide(to, error);
    if (decision != FileOps::Existing::Overwrite) {
        return decision == FileOps::Existing::Skip ? Target::Skipped : Target::Stopped;
    }
    fs::remove_all(to, ec);
    if (ec) {
        fail(error, toQString(to), ec);
        return Target::Stopped;
    }
    return Target::Free;
}

// Copies `from` onto `to`, which must not exist yet.
bool Engine::copyFile(const fs::path &from, const fs::path &to, QString *error)
{
    std::error_code ec;
    const auto size = fs::file_size(from, ec);
    const qint64 bytes = ec ? 0 : qint64(size);

    if (bytes <= kFastCopyLimit) {
        // One call: small enough that there is nothing to show in between.
        fs::copy_file(from, to, fs::copy_options::none, ec);
        if (ec) {
            return fail(error, toQString(from), ec);
        }
        // Not every system keeps the date and the permissions with the file.
        std::error_code meta;
        fs::last_write_time(to, fs::last_write_time(from, meta), meta);
        fs::permissions(to, fs::status(from, meta).permissions(), fs::perm_options::replace, meta);
        m_done += bytes;
        m_totals.bytes += bytes;
        return true;
    }

    // In pieces, so that progress can be seen and the copy stopped.
    QFile source(toQString(from));
    QFile target(toQString(to));
    if (!source.open(QIODevice::ReadOnly)) {
        return fail(error, QStringLiteral("%1: %2").arg(toQString(from), source.errorString()));
    }
    if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(error, QStringLiteral("%1: %2").arg(toQString(to), target.errorString()));
    }
    QByteArray buffer(int(kChunkSize), Qt::Uninitialized);
    while (true) {
        const qint64 read = source.read(buffer.data(), buffer.size());
        if (read < 0) {
            return fail(error, QStringLiteral("%1: %2").arg(toQString(from), source.errorString()));
        }
        if (read == 0) {
            break;
        }
        if (target.write(buffer.constData(), read) != read) {
            return fail(error, QStringLiteral("%1: %2").arg(toQString(to), target.errorString()));
        }
        m_done += read;
        if (!tick(toQString(to), error)) {
            target.close();
            fs::remove(to, ec);  // not half a file
            return false;
        }
    }
    target.setPermissions(source.permissions());
    target.setFileTime(source.fileTime(QFileDevice::FileModificationTime),
                       QFileDevice::FileModificationTime);
    m_totals.bytes += bytes;
    return true;
}

bool Engine::copyEntry(const fs::path &from, const fs::path &to, int depth, QString *error)
{
    if (depth > kMaxDepth) {
        return tooDeep(from, error);
    }
    std::error_code ec;
#ifdef Q_OS_UNIX
    if (fs::is_symlink(from, ec)) {
        // The link itself is copied, not what it points to.
        const fs::path linkTarget = fs::read_symlink(from, ec);
        if (ec) {
            return fail(error, toQString(from), ec);
        }
        switch (clearTarget(to, error)) {
        case Target::Free:
            break;
        case Target::Skipped:
            return true;
        case Target::Stopped:
            return false;
        }
        if (!QFile::link(toQString(linkTarget), toQString(to))) {
            return fail(error, tr("%1: the link could not be created.").arg(toQString(to)));
        }
        add(Work{.files = 1});
        return tick(toQString(to), error);
    }
#endif
    if (fs::is_directory(from, ec)) {
        bool merge = false;
        if (fs::exists(fs::symlink_status(to, ec))) {
            if (fs::is_directory(to, ec) && !fs::is_symlink(to, ec)) {
                // A folder of that name is there already: the contents go in.
                merge = true;
            } else {
                switch (clearTarget(to, error)) {
                case Target::Free:
                    break;
                case Target::Skipped:
                    return true;
                case Target::Stopped:
                    return false;
                }
            }
        }
        if (!merge) {
            fs::create_directory(to, ec);
            if (ec) {
                return fail(error, toQString(to), ec);
            }
        }
        add(Work{.folders = 1});
        if (!tick(toQString(to), error)) {
            return false;
        }
        std::error_code itError;
        fs::directory_iterator it(from, fs::directory_options::skip_permission_denied, itError);
        if (itError) {
            return fail(error, toQString(from), itError);
        }
        for (const fs::directory_entry &entry : it) {
            if (!copyEntry(entry.path(), to / entry.path().filename(), depth + 1, error)) {
                return false;
            }
        }
        // The folder's own date, last, after its contents changed it.
        std::error_code meta;
        fs::last_write_time(to, fs::last_write_time(from, meta), meta);
        return true;
    }

    // A file.
    switch (clearTarget(to, error)) {
    case Target::Free:
        break;
    case Target::Skipped:
        return true;
    case Target::Stopped:
        return false;
    }
    if (!copyFile(from, to, error)) {
        return false;
    }
    add(Work{.files = 1});
    return tick(toQString(to), error);
}

bool Engine::moveEntry(const fs::path &from, const fs::path &to, int depth,
                       const FileOps::MoveOptions &options, QString *error)
{
    if (depth > kMaxDepth) {
        return tooDeep(from, error);
    }
    std::error_code ec;
    const bool isLink = fs::is_symlink(from, ec);
    const bool isFolder = !isLink && fs::is_directory(from, ec);

    if (isFolder && fs::exists(to, ec) && fs::is_directory(to, ec)) {
        // A folder of that name is there already: the contents move in.
        add(Work{.folders = 1});
        if (!tick(toQString(from), error)) {
            return false;
        }
        std::error_code itError;
        fs::directory_iterator it(from, fs::directory_options::skip_permission_denied, itError);
        if (itError) {
            return fail(error, toQString(from), itError);
        }
        for (const fs::directory_entry &entry : it) {
            if (!moveEntry(entry.path(), to / entry.path().filename(), depth + 1, options, error)) {
                return false;
            }
        }
        // What was skipped stays behind, and so does the folder then.
        fs::remove(from, ec);
        return true;
    }

    switch (clearTarget(to, error)) {
    case Target::Free:
        break;
    case Target::Skipped:
        return true;
    case Target::Stopped:
        return false;
    }

    // A rename keeps a file's dates and attributes as they are.
    const Work work = measure(from);
    if (options.renameOnSameVolume) {
        const bool same = sameVolume(from, to);
        fs::rename(from, to, ec);
        if (!ec) {
            add(work);
            return tick(toQString(to), error);
        }
        if (same) {
            return fail(error, toQString(from), ec);
        }
        // Another volume after all: fall through to copying.
    }

    if (!copyEntry(from, to, depth, error)) {
        return false;
    }
    fs::remove_all(from, ec);
    if (ec) {
        return fail(error, toQString(from), ec);
    }
    return true;
}

bool Engine::copy(const QStringList &sources, const QString &targetFolder, QString *error,
                  FileOps::Totals *totals)
{
    if (!begin(sources, error)) {
        return false;
    }
    const fs::path folder = toPath(QDir::cleanPath(targetFolder));
    for (const QString &source : sources) {
        const fs::path from = toPath(QDir::cleanPath(source));
        const fs::path to = folder / from.filename();
        const Placement placement = Engine::placement(from, to);
        if (placement == Placement::Inside) {
            return fail(error, tr("%1 can't be copied into itself.").arg(source));
        }
        if (placement == Placement::Go && !copyEntry(from, to, 0, error)) {
            return false;
        }
    }
    if (totals) {
        *totals = m_totals;
    }
    return true;
}

bool Engine::move(const QStringList &sources, const QString &targetFolder,
                  const FileOps::MoveOptions &options, QString *error, FileOps::Totals *totals)
{
    if (!begin(sources, error)) {
        return false;
    }
    const fs::path folder = toPath(QDir::cleanPath(targetFolder));
    for (const QString &source : sources) {
        const fs::path from = toPath(QDir::cleanPath(source));
        const fs::path to = folder / from.filename();
        const Placement placement = Engine::placement(from, to);
        if (placement == Placement::Inside) {
            return fail(error, tr("%1 can't be moved into itself.").arg(source));
        }
        if (placement == Placement::Go && !moveEntry(from, to, 0, options, error)) {
            return false;
        }
    }
    if (totals) {
        *totals = m_totals;
    }
    return true;
}

bool Engine::remove(const QStringList &paths, const QString &home, QString *error,
                    FileOps::Totals *totals)
{
    // Looked at before anything else, so that a protected folder costs no
    // time and runs no risk, whatever else goes wrong.
    for (const QString &path : paths) {
        if (FileOps::isProtected(path, home)) {
            return fail(error, tr("%1 is never deleted.").arg(path));
        }
    }
    if (!begin(paths, error)) {
        return false;
    }
    for (const QString &path : paths) {
        const fs::path target = toPath(QDir::cleanPath(path));
        const Work work = measure(target);
        std::error_code ec;
        fs::remove_all(target, ec);
        if (ec) {
            return fail(error, path, ec);
        }
        add(work);
        if (!tick(path, error)) {
            return false;
        }
    }
    if (totals) {
        *totals = m_totals;
    }
    return true;
}

}  // namespace

namespace FileOps {

bool copy(const QStringList &sources, const QString &targetFolder, OverwritePrompt &prompt,
          Progress &progress, QString *error, Totals *totals)
{
    clear(error);
    Engine engine(&prompt, progress);
    return engine.copy(sources, targetFolder, error, totals);
}

bool move(const QStringList &sources, const QString &targetFolder, OverwritePrompt &prompt,
          Progress &progress, QString *error, Totals *totals, const MoveOptions &options)
{
    clear(error);
    Engine engine(&prompt, progress);
    return engine.move(sources, targetFolder, options, error, totals);
}

bool remove(const QStringList &paths, Progress &progress, QString *error, Totals *totals,
            const QString &home)
{
    clear(error);
    Engine engine(nullptr, progress);
    return engine.remove(paths, home, error, totals);
}

bool trash(const QStringList &paths, QString *error, Totals *totals, const QString &home)
{
    clear(error);
    // Before anything else, as in remove().
    for (const QString &path : paths) {
        if (isProtected(path, home)) {
            return fail(error, tr("%1 is never deleted.").arg(path));
        }
    }
    Totals done;
    for (const QString &path : paths) {
        // Asked before it is gone.
        const bool folder = QFileInfo(path).isDir();
        QString reason;
        if (!QFile::moveToTrash(path, &reason)) {
            return fail(error, tr("%1 could not be moved to the Trash: %2").arg(path, reason));
        }
        if (folder) {
            ++done.folders;
        } else {
            ++done.files;
        }
    }
    if (totals) {
        *totals = done;
    }
    return true;
}

bool makeFolder(const QString &path, QString *error)
{
    clear(error);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(toPath(path), ec))) {
        return fail(error, tr("%1 already exists.").arg(path));
    }
    fs::create_directory(toPath(path), ec);
    if (ec) {
        return fail(error, path, ec);
    }
    return true;
}

bool makeFile(const QString &path, QString *error)
{
    clear(error);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(toPath(path), ec))) {
        return fail(error, tr("%1 already exists.").arg(path));
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(error, QStringLiteral("%1: %2").arg(path, file.errorString()));
    }
    return true;
}

bool rename(const QString &path, const QString &newName, QString *error)
{
    clear(error);
    QString reason;
    if (!isAllowedName(newName, &reason)) {
        return fail(error, reason);
    }
    const QFileInfo info(path);
    const QString target = info.dir().filePath(newName);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(toPath(target), ec))
        && !fs::equivalent(toPath(info.absoluteFilePath()), toPath(target), ec)) {
        return fail(error, tr("%1 already exists.").arg(target));
    }
    fs::rename(toPath(info.absoluteFilePath()), toPath(target), ec);
    if (ec) {
        return fail(error, path, ec);
    }
    return true;
}

qint64 sizeOf(const QString &path)
{
    return measure(toPath(QDir::cleanPath(path))).bytes;
}

bool isAllowedName(const QString &name, QString *reason)
{
    const auto no = [reason](const QString &why) {
        if (reason) {
            *reason = why;
        }
        return false;
    };
    if (name.isEmpty()) {
        return no(tr("The name can't be empty."));
    }
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || QStringLiteral("/\\:*?\"<>|").contains(c)) {
            return no(tr("The name can't contain / \\ : * ? \" < > | or control characters."));
        }
    }
    if (name.startsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char(' '))
        || name.endsWith(QLatin1Char('.'))) {
        return no(tr("The name can't start with a space, or end with a space or a dot."));
    }
    // Windows keeps these for devices.
    const QString base = name.section(QLatin1Char('.'), 0, 0).toUpper();
    const bool numbered = base.size() == 4 && base.at(3) >= QLatin1Char('1')
                          && base.at(3) <= QLatin1Char('9')
                          && (base.startsWith(QLatin1String("COM"))
                              || base.startsWith(QLatin1String("LPT")));
    if (numbered || base == QLatin1String("CON") || base == QLatin1String("PRN")
        || base == QLatin1String("AUX") || base == QLatin1String("NUL")) {
        return no(tr("\"%1\" is a name Windows keeps for devices.").arg(name));
    }
    if (reason) {
        reason->clear();
    }
    return true;
}

bool isProtected(const QString &path, const QString &home)
{
    const QString target = QFileInfo(path).canonicalFilePath();
    if (target.isEmpty()) {
        return false;
    }
    // The home folder, and everything above it up to the root of the volume.
    QString folder = QFileInfo(home).canonicalFilePath();
    while (!folder.isEmpty()) {
        if (samePath(folder, target)) {
            return true;
        }
        const QString parent = QFileInfo(folder).path();
        if (samePath(parent, folder)) {
            break;
        }
        folder = parent;
    }
    return false;
}

}  // namespace FileOps
