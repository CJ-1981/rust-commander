#include "Shell.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>

namespace {

// A terminal that is started with the folder as its working folder.
struct Terminal {
    QString program;
    QStringList arguments;
};

QString nameOf(const QStorageInfo &storage)
{
    const QString label = storage.name();
    if (!label.isEmpty()) {
        return label;
    }
    const QString root = QDir::toNativeSeparators(storage.rootPath());
    const QString last = QDir(storage.rootPath()).dirName();
    return last.isEmpty() ? root : last;
}

// Whether a volume is a place users see in their file manager. System
// volumes, memory disks and the like are left out.
bool isUserVolume(const QStorageInfo &storage)
{
    if (!storage.isValid() || !storage.isReady()) {
        return false;
    }
    const QString root = QDir::cleanPath(storage.rootPath());
#ifdef Q_OS_MACOS
    // The startup volume, and what is mounted under /Volumes; macOS also
    // mounts pieces of the system volume under /System/Volumes.
    return root == QLatin1String("/") || root.startsWith(QLatin1String("/Volumes/"));
#elif defined(Q_OS_LINUX)
    return root == QLatin1String("/") || root.startsWith(QLatin1String("/media/"))
           || root.startsWith(QLatin1String("/mnt/")) || root.startsWith(QLatin1String("/run/media/"));
#else
    Q_UNUSED(root);
    return true;
#endif
}

}  // namespace

namespace Shell {

bool open(const QString &path)
{
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool terminalCommand(const QString &folder, QString *program, QStringList *arguments)
{
    if (!QFileInfo(folder).isDir()) {
        return false;
    }
#ifdef Q_OS_WIN
    // Windows Terminal when it is there; it starts in the folder with -d.
    const QString terminal = QStandardPaths::findExecutable(QStringLiteral("wt.exe"));
    if (!terminal.isEmpty()) {
        *program = terminal;
        *arguments = {QStringLiteral("-d"), folder};
        return true;
    }
    *program = QStringLiteral("cmd.exe");
    *arguments = {QStringLiteral("/k")};
    return true;
#elif defined(Q_OS_MACOS)
    *program = QStringLiteral("open");
    *arguments = {QStringLiteral("-a"), QStringLiteral("Terminal"), folder};
    return true;
#else
    struct Candidate {
        const char *program;
        const char *flag;  // names the folder; without one, it is the working folder
    };
    static const Candidate candidates[] = {
        {"x-terminal-emulator", nullptr},
        {"gnome-terminal", "--working-directory=%1"},
        {"konsole", "--workdir %1"},
        {"xfce4-terminal", "--working-directory=%1"},
        {"mate-terminal", "--working-directory=%1"},
        {"qterminal", "--working-directory %1"},
        {"xterm", nullptr},
    };
    for (const Candidate &candidate : candidates) {
        const QString found = QStandardPaths::findExecutable(QString::fromLatin1(candidate.program));
        if (found.isEmpty()) {
            continue;
        }
        *program = found;
        arguments->clear();
        if (candidate.flag) {
            const QStringList flag = QString::fromLatin1(candidate.flag).split(QLatin1Char(' '));
            arguments->append(flag.first() + folder);
        }
        return true;
    }
    return false;
#endif
}

bool openTerminal(const QString &folder)
{
    QString program;
    QStringList arguments;
    if (!terminalCommand(folder, &program, &arguments)) {
        return false;
    }
    // The folder is the working folder too, for terminals that take no option.
    return QProcess::startDetached(program, arguments, folder);
}

QList<Volume> volumes()
{
    QList<Volume> result;
    const auto storages = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &storage : storages) {
        if (!isUserVolume(storage)) {
            continue;
        }
        Volume volume;
        volume.root = QDir::toNativeSeparators(storage.rootPath());
        volume.name = nameOf(storage);
        volume.bytesTotal = qint64(storage.bytesTotal());
        volume.bytesFree = qint64(storage.bytesAvailable());
        result.append(volume);
    }
    return result;
}

QString volumeName(const QString &path)
{
    const QStorageInfo storage(path);
    if (!storage.isValid()) {
        return path;
    }
    return nameOf(storage);
}

qint64 freeBytes(const QString &path)
{
    // An empty path would mean the working folder.
    if (path.isEmpty()) {
        return -1;
    }
    const QStorageInfo storage(path);
    if (!storage.isValid() || !storage.isReady()) {
        return -1;
    }
    return qint64(storage.bytesAvailable());
}

}  // namespace Shell
