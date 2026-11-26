#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTest>

#include "helpers.h"
#include "ops/Shell.h"

class TestShell : public QObject {
    Q_OBJECT

private slots:
    void listsVolumes();
    void knowsFreeSpace();
    void findsATerminal();
};

void TestShell::listsVolumes()
{
    const QList<Shell::Volume> volumes = Shell::volumes();
    QVERIFY(!volumes.isEmpty());
    for (const Shell::Volume &volume : volumes) {
        QVERIFY(!volume.root.isEmpty());
        QVERIFY(!volume.name.isEmpty());
        QVERIFY(volume.bytesTotal > 0);
        QVERIFY(volume.bytesFree >= 0);
    }

    // The volume the temporary folder is on is among them.
    QTemporaryDir temp;
    const QString root = QDir::cleanPath(QStorageInfo(temp.path()).rootPath());
    bool found = false;
    for (const Shell::Volume &volume : volumes) {
        if (QDir::cleanPath(QDir::fromNativeSeparators(volume.root)) == root) {
            found = true;
        }
    }
    QVERIFY2(found, qPrintable(root));
}

void TestShell::knowsFreeSpace()
{
    QTemporaryDir temp;
    QVERIFY(Shell::freeBytes(temp.path()) > 0);
    QCOMPARE(Shell::freeBytes(QString()), qint64(-1));
}

void TestShell::findsATerminal()
{
    // Only the command is asked for: starting one would open a window.
    QTemporaryDir temp;
    QString program;
    QStringList arguments;
    QVERIFY(Shell::terminalCommand(temp.path(), &program, &arguments));
    QVERIFY(!program.isEmpty());
    QVERIFY(!Shell::terminalCommand(temp.path() + "/no-such-folder", &program, &arguments));
    // A file is not a folder to start in.
    QVERIFY(writeFile(temp.path() + "/file.txt", "x"));
    QVERIFY(!Shell::terminalCommand(temp.path() + "/file.txt", &program, &arguments));
}

QTEST_MAIN(TestShell)
#include "shell.moc"
