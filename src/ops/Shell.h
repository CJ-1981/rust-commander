#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

// Talking to the system around the app: opening files with their applications,
// a terminal, and the volumes there are.
namespace Shell {

struct Volume {
    QString root;  // where the volume is mounted, e.g. "/" or "D:/"
    QString name;  // what to show
    qint64 bytesTotal = 0;
    qint64 bytesFree = 0;
};

// Opens a file with the application the system uses for it, or a folder in
// the system's own file manager.
bool open(const QString &path);

// Starts a terminal with `folder` as its working folder. Returns false when
// none could be started.
bool openTerminal(const QString &folder);

// The terminal openTerminal() would start; false when there is none.
bool terminalCommand(const QString &folder, QString *program, QStringList *arguments);

// The volumes a user may want to put files on.
QList<Volume> volumes();

// Free space on the volume `path` is on; -1 when it can't be told.
qint64 freeBytes(const QString &path);

}  // namespace Shell
