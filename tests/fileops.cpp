#include <QDateTime>
#include <QDirIterator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTest>

#include <optional>

#include "helpers.h"
#include "ops/FileOps.h"

#include <algorithm>
#include <filesystem>
#ifdef Q_OS_MACOS
#include <sys/xattr.h>
#endif

namespace {

// Answers overwrite questions from a script, and keeps count.
class Answers : public FileOps::OverwritePrompt {
public:
    FileOps::Existing ask(const QString &target) override
    {
        ++asked;
        targets.append(target);
        if (script.isEmpty()) {
            return FileOps::Existing::Cancel;
        }
        return script.takeFirst();
    }

    QList<FileOps::Existing> script;
    QStringList targets;
    int asked = 0;
};

// Records progress; can be told to stop after a number of reports.
class Recorder : public FileOps::Progress {
public:
    bool tick(qint64 done, qint64 total, const QString &current) override
    {
        ++ticks;
        neverWentBack = neverWentBack && done >= lastDone;
        lastDone = done;
        lastTotal = total;
        lastCurrent = current;
        return !stopAfter || ticks < *stopAfter;
    }

    int ticks = 0;
    qint64 lastDone = -1;
    qint64 lastTotal = 0;
    QString lastCurrent;
    bool neverWentBack = true;
    std::optional<int> stopAfter;
};

// A source folder with a.txt (3 bytes), sub/b.txt (4) and sub/deep/c.txt (5),
// next to an empty destination folder. 17 units of work in all: 12 bytes,
// 3 files and 2 folders.
struct Folders {
    explicit Folders(const QTemporaryDir &root)
        : source(root.path() + "/src")
        , target(root.path() + "/dst")
    {
        makeFolder(source + "/sub/deep");
        makeFolder(target);
        writeFile(source + "/a.txt", "aaa");
        writeFile(source + "/sub/b.txt", "bbbb");
        writeFile(source + "/sub/deep/c.txt", "ccccc");
    }

    QString source;
    QString target;
};

constexpr qint64 kWork = 17;

}  // namespace

class TestFileOps : public QObject {
    Q_OBJECT

private slots:
    void copiesFilesAndFolders();
    void mergesIntoAFolderOfTheSameName();
    void overwriteChoices();
    void aFolderIsNotCopiedIntoItself();
    void movingOnTheSameVolumeIsARename();
    void movingOntoAnotherVolumeCopies();
    void movingIntoAFolderOfTheSameName();
    void removeDeletesForGood();
    void someFoldersAreNeverDeleted();
    void trash();
    void quickOperations();
    void namesAreChecked();
    void largeFilesAreCopiedInPiecesAndCanBeStopped();
    void sizes();
    void linksAreCopiedAsLinks();

    // ---- added by the reviewer; each asserts the behaviour a user expects ----
    void reviewCrossVolumeMoveKeepsWhatWasSkipped();
    void reviewStopKeepsSomethingOfAFolderBeingRemoved();
    void reviewGuardSeesTheHomeFolderThroughAFirmlink();
    void reviewAFolderIsNotCopiedIntoItselfThroughAnAlias();
    void reviewAFolderIsNotCopiedIntoItselfThroughAnotherCase();
    void reviewMoveOntoASymlinkedFolderAsksLikeCopy();
    void reviewAMissingSourceDoesNotRemoveItsTarget();
    void reviewBothCopyWaysKeepWhatAFileHasBesidesItsBytes();
};

void TestFileOps::copiesFilesAndFolders()
{
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    Answers answers;
    QString error;
    FileOps::Totals totals;

    // A date to see whether it is kept.
    const QDateTime stamp(QDate(2021, 5, 6), QTime(7, 8, 9));
    setModified(f.source + "/sub/deep/c.txt", stamp);
    setModified(f.source + "/a.txt", stamp);

    QVERIFY(FileOps::copy({f.source + "/a.txt", f.source + "/sub"}, f.target, answers, progress,
                          &error, &totals));
    QCOMPARE(error, QString());
    QCOMPARE(answers.asked, 0);

    QCOMPARE(readFile(f.target + "/a.txt"), QString("aaa"));
    QCOMPARE(readFile(f.target + "/sub/b.txt"), QString("bbbb"));
    QCOMPARE(readFile(f.target + "/sub/deep/c.txt"), QString("ccccc"));
    // The sources stay.
    QCOMPARE(readFile(f.source + "/a.txt"), QString("aaa"));
    QVERIFY(QFileInfo::exists(f.source + "/sub/deep/c.txt"));

    QCOMPARE(totals.files, 3);
    QCOMPARE(totals.folders, 2);
    QCOMPARE(totals.bytes, 12);
    QCOMPARE(progress.lastTotal, kWork);
    QCOMPARE(progress.lastDone, kWork);
    QVERIFY(progress.neverWentBack);
    QVERIFY(progress.ticks >= 6);

    QCOMPARE(QFileInfo(f.target + "/sub/deep/c.txt").lastModified(), stamp);
    QCOMPARE(QFileInfo(f.target + "/a.txt").lastModified(), stamp);
}

void TestFileOps::mergesIntoAFolderOfTheSameName()
{
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    Answers answers;
    QString error;
    makeFolder(f.target + "/sub");
    writeFile(f.target + "/sub/b.txt", "different");
    writeFile(f.target + "/sub/d.txt", "keep");

    // b.txt exists already: skipped, so it stays as it is.
    answers.script = {FileOps::Existing::Skip};
    QVERIFY(FileOps::copy({f.source + "/sub"}, f.target, answers, progress, &error));
    QCOMPARE(answers.asked, 1);
    // The question names the file as the system writes paths there.
    QCOMPARE(answers.targets.size(), 1);
    QCOMPARE(QDir::fromNativeSeparators(answers.targets.value(0)),
             f.target + "/sub/b.txt");
    QCOMPARE(readFile(f.target + "/sub/b.txt"), QString("different"));
    QCOMPARE(readFile(f.target + "/sub/d.txt"), QString("keep"));
    QCOMPARE(readFile(f.target + "/sub/deep/c.txt"), QString("ccccc"));

    // Overwritten: replaced by the source. c.txt is asked about as well now.
    writeFile(f.source + "/sub/b.txt", "the new one");
    answers = Answers();
    answers.script = {FileOps::Existing::Overwrite, FileOps::Existing::Overwrite};
    QVERIFY(FileOps::copy({f.source + "/sub"}, f.target, answers, progress, &error));
    QCOMPARE(answers.asked, 2);
    QCOMPARE(error, QString());
    QCOMPARE(readFile(f.target + "/sub/b.txt"), QString("the new one"));
    QCOMPARE(readFile(f.target + "/sub/d.txt"), QString("keep"));
    // The names come back as the system writes paths there.
    const bool bWasAskedAbout = std::any_of(answers.targets.cbegin(), answers.targets.cend(),
                                            [&](const QString &asked) {
                                                return QDir::fromNativeSeparators(asked)
                                                       == f.target + "/sub/b.txt";
                                            });
    QVERIFY(bWasAskedAbout);
}

void TestFileOps::overwriteChoices()
{
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    Answers answers;
    QString error;
    for (const QString &name : {"f1", "f2", "f3"}) {
        writeFile(f.source + "/" + name, "new");
        writeFile(f.target + "/" + name, "old");
    }
    const QStringList all = {f.source + "/f1", f.source + "/f2", f.source + "/f3"};

    // Asked once, then "all" applies to the rest.
    answers.script = {FileOps::Existing::OverwriteAll};
    QVERIFY(FileOps::copy(all, f.target, answers, progress, &error));
    QCOMPARE(answers.asked, 1);
    for (const QString &name : {"f1", "f2", "f3"}) {
        QCOMPARE(readFile(f.target + "/" + name), QString("new"));
    }

    answers = Answers();
    answers.script = {FileOps::Existing::SkipAll};
    QVERIFY(FileOps::copy(all, f.target, answers, progress, &error));
    QCOMPARE(answers.asked, 1);

    // Cancel stops the work; what was done stays done, the rest is untouched.
    writeFile(f.source + "/f3", "latest");
    answers = Answers();
    answers.script = {FileOps::Existing::Cancel};
    QVERIFY(!FileOps::copy(all, f.target, answers, progress, &error));
    QCOMPARE(answers.asked, 1);
    QCOMPARE(error, QString());
    QCOMPARE(readFile(f.target + "/f3"), QString("new"));

    // A file in place of a folder is asked about too.
    answers = Answers();
    writeFile(f.target + "/sub", "in the way");
    answers.script = {FileOps::Existing::Overwrite};
    QVERIFY(FileOps::copy({f.source + "/sub"}, f.target, answers, progress, &error));
    QVERIFY(QFileInfo(f.target + "/sub").isDir());
}

void TestFileOps::aFolderIsNotCopiedIntoItself()
{
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    Answers answers;
    QString error;

    QVERIFY(!FileOps::copy({f.source + "/sub"}, f.source + "/sub", answers, progress, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(f.source + "/sub/sub"));

    // Onto itself: there is nothing to do, which is not an error.
    QVERIFY(FileOps::copy({f.source + "/a.txt"}, f.source, answers, progress, &error));
    QCOMPARE(error, QString());
    QCOMPARE(readFile(f.source + "/a.txt"), QString("aaa"));
    QVERIFY(!FileOps::move({f.source + "/sub"}, f.source + "/sub", answers, progress, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo::exists(f.source + "/sub/b.txt"));
}

void TestFileOps::movingOnTheSameVolumeIsARename()
{
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    Answers answers;
    QString error;
    FileOps::Totals totals;
    const QDateTime stamp(QDate(2019, 12, 31), QTime(23, 59, 58));
    setModified(f.source + "/a.txt", stamp);

    QVERIFY(FileOps::move({f.source + "/a.txt", f.source + "/sub"}, f.target, answers, progress,
                          &error, &totals));
    QCOMPARE(error, QString());
    QCOMPARE(answers.asked, 0);

    QVERIFY(!QFileInfo::exists(f.source + "/a.txt"));
    QVERIFY(!QFileInfo::exists(f.source + "/sub"));
    QCOMPARE(readFile(f.target + "/a.txt"), QString("aaa"));
    QCOMPARE(readFile(f.target + "/sub/deep/c.txt"), QString("ccccc"));
    // A rename keeps the file's date.
    QCOMPARE(QFileInfo(f.target + "/a.txt").lastModified(), stamp);
    // a.txt, b.txt and deep/c.txt; sub and deep.
    QCOMPARE(totals.files, 3);
    QCOMPARE(totals.folders, 2);
    QCOMPARE(totals.bytes, 12);
    QCOMPARE(progress.lastDone, kWork);
    QCOMPARE(progress.lastTotal, kWork);
}

void TestFileOps::movingOntoAnotherVolumeCopies()
{
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    Answers answers;
    QString error;
    const QDateTime stamp(QDate(2018, 2, 3), QTime(4, 5, 6));
    setModified(f.source + "/sub/deep/c.txt", stamp);

    // Without renaming, the way a move onto another volume has to be done.
    QVERIFY(FileOps::move({f.source + "/a.txt", f.source + "/sub"}, f.target, answers, progress,
                          &error, nullptr, FileOps::MoveOptions{.renameOnSameVolume = false}));
    QCOMPARE(error, QString());
    QVERIFY(!QFileInfo::exists(f.source + "/a.txt"));
    QVERIFY(!QFileInfo::exists(f.source + "/sub"));
    QCOMPARE(readFile(f.target + "/a.txt"), QString("aaa"));
    QCOMPARE(readFile(f.target + "/sub/b.txt"), QString("bbbb"));
    QCOMPARE(readFile(f.target + "/sub/deep/c.txt"), QString("ccccc"));
    QCOMPARE(QFileInfo(f.target + "/sub/deep/c.txt").lastModified(), stamp);
    QCOMPARE(progress.lastDone, kWork);
}

void TestFileOps::movingIntoAFolderOfTheSameName()
{
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    Answers answers;
    QString error;
    makeFolder(f.target + "/sub");
    writeFile(f.target + "/sub/b.txt", "stays");

    // b.txt is skipped: it and the folder it is in stay where they are.
    answers.script = {FileOps::Existing::Skip};
    QVERIFY(FileOps::move({f.source + "/sub"}, f.target, answers, progress, &error));
    QCOMPARE(answers.asked, 1);
    QVERIFY(QFileInfo::exists(f.source + "/sub/b.txt"));
    QVERIFY(QFileInfo::exists(f.source + "/sub"));
    QVERIFY(!QFileInfo::exists(f.source + "/sub/deep"));
    QCOMPARE(readFile(f.target + "/sub/b.txt"), QString("stays"));
    QCOMPARE(readFile(f.target + "/sub/deep/c.txt"), QString("ccccc"));

    // Overwritten: the folder is moved whole, and the source is gone.
    answers = Answers();
    answers.script = {FileOps::Existing::Overwrite};
    QVERIFY(FileOps::move({f.source + "/sub"}, f.target, answers, progress, &error));
    QVERIFY(!QFileInfo::exists(f.source + "/sub"));
    QCOMPARE(readFile(f.target + "/sub/b.txt"), QString("bbbb"));
}

void TestFileOps::removeDeletesForGood()
{
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    QString error;
    FileOps::Totals totals;

    QVERIFY(FileOps::remove({f.source + "/a.txt", f.source + "/sub"}, progress, &error, &totals));
    QCOMPARE(error, QString());
    QVERIFY(!QFileInfo::exists(f.source + "/a.txt"));
    QVERIFY(!QFileInfo::exists(f.source + "/sub"));
    QVERIFY(QFileInfo::exists(f.target));
    QCOMPARE(totals.files, 3);
    QCOMPARE(totals.folders, 2);
    QCOMPARE(progress.lastDone, kWork);

    // Stopped before anything happened.
    progress = Recorder();
    progress.stopAfter = 1;
    writeFile(f.target + "/keep", "k");
    QVERIFY(!FileOps::remove({f.target + "/keep"}, progress, &error));
    QCOMPARE(error, QString());
    QCOMPARE(readFile(f.target + "/keep"), QString("k"));
}

void TestFileOps::someFoldersAreNeverDeleted()
{
    // A folder stands in for the home folder, so that nothing real is ever at
    // risk if the guard should fail.
    QTemporaryDir root;
    const QString home = root.path() + "/home";
    const QString elsewhere = root.path() + "/elsewhere";
    QVERIFY(makeFolder(home + "/Documents"));
    QVERIFY(writeFile(home + "/Documents/letter.txt", "dear"));
    QVERIFY(makeFolder(elsewhere));

    // The home folder, the folders above it and the root of the volume.
    QVERIFY(FileOps::isProtected(home, home));
    QVERIFY(FileOps::isProtected(home + "/Documents/..", home));
    QVERIFY(FileOps::isProtected(root.path(), home));
    QVERIFY(FileOps::isProtected(QDir::rootPath(), home));
    // Inside the home folder is fine, and so is anywhere else.
    QVERIFY(!FileOps::isProtected(home + "/Documents", home));
    QVERIFY(!FileOps::isProtected(elsewhere, home));
    QVERIFY(!FileOps::isProtected(home + "/no-such-folder", home));
    // The real ones too; that is only a question, nothing is done to them.
    QVERIFY(FileOps::isProtected(QDir::homePath()));
    QVERIFY(FileOps::isProtected(QDir::rootPath()));

    Recorder progress;
    QString error;
    QVERIFY(!FileOps::remove({home}, progress, &error, nullptr, home));
    QVERIFY(!error.isEmpty());
    QVERIFY(!FileOps::remove({elsewhere, home + "/Documents"}, progress, &error, nullptr,
                             home + "/Documents"));
    QVERIFY(!error.isEmpty());
    QVERIFY(!FileOps::trash({home}, &error, nullptr, home));
    QVERIFY(!error.isEmpty());
    // Refused before any work was done.
    QCOMPARE(progress.ticks, 0);
    QCOMPARE(readFile(home + "/Documents/letter.txt"), QString("dear"));

    // The guard is all that keeps it: with another folder as the home, the
    // same call deletes it like anything else.
    QVERIFY(FileOps::remove({home}, progress, &error, nullptr, elsewhere));
    QVERIFY(!QFileInfo::exists(home));
}

void TestFileOps::trash()
{
    // This puts test files into the real Trash, so it only runs where that is
    // welcome; CI asks for it. The refusal to trash a protected folder is
    // tested in someFoldersAreNeverDeleted() without touching the Trash.
    if (qEnvironmentVariableIntValue("RC_TEST_TRASH") != 1) {
        QSKIP("set RC_TEST_TRASH=1 to put test files into the real Trash");
    }
    QTemporaryDir root;
    Folders f(root);
    FileOps::Totals totals;
    QString error;
    QVERIFY(FileOps::trash({f.source + "/a.txt", f.source + "/sub"}, &error, &totals));
    QCOMPARE(error, QString());
    QVERIFY(!QFileInfo::exists(f.source + "/a.txt"));
    QVERIFY(!QFileInfo::exists(f.source + "/sub"));
    QCOMPARE(totals.files, 1);
    QCOMPARE(totals.folders, 1);
    QCOMPARE(totals.bytes, 0);

    // What isn't there can't be trashed.
    QVERIFY(!FileOps::trash({f.source + "/a.txt"}, &error));
    QVERIFY(!error.isEmpty());
}

void TestFileOps::quickOperations()
{
    QTemporaryDir root;
    const QString dir = root.path();
    QString error;

    QVERIFY(FileOps::makeFolder(dir + "/new", &error));
    QVERIFY(QFileInfo(dir + "/new").isDir());
    QVERIFY(!FileOps::makeFolder(dir + "/new", &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(FileOps::makeFile(dir + "/new/file.txt", &error));
    QCOMPARE(readFile(dir + "/new/file.txt"), QString());
    QVERIFY(!FileOps::makeFile(dir + "/new/file.txt", &error));

    QVERIFY(FileOps::rename(dir + "/new/file.txt", "renamed.md", &error));
    QVERIFY(QFileInfo::exists(dir + "/new/renamed.md"));
    QVERIFY(!QFileInfo::exists(dir + "/new/file.txt"));

    // Not onto a name that is taken, and not to a name that can't be used.
    writeFile(dir + "/new/taken", "t");
    QVERIFY(!FileOps::rename(dir + "/new/renamed.md", "taken", &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!FileOps::rename(dir + "/new/renamed.md", "a/b", &error));
    QVERIFY(QFileInfo::exists(dir + "/new/renamed.md"));

    // Only the case of the name changes.
    writeFile(dir + "/new/Case.txt", "c");
    QVERIFY(FileOps::rename(dir + "/new/Case.txt", "CASE.TXT", &error));
    QVERIFY(QFileInfo::exists(dir + "/new/CASE.TXT"));
    QVERIFY(!FileOps::rename(dir + "/nope", "x", &error));
}

void TestFileOps::namesAreChecked()
{
    QString reason;
    QVERIFY(FileOps::isAllowedName("notes.txt", &reason));
    QCOMPARE(reason, QString());
    QVERIFY(FileOps::isAllowedName(QString::fromUtf8("한국어 파일.txt"), &reason));
    QVERIFY(FileOps::isAllowedName("auxiliary", &reason));  // not the AUX device
    QVERIFY(FileOps::isAllowedName("com10", &reason));
    QVERIFY(FileOps::isAllowedName("a.b.c", &reason));
    QVERIFY(FileOps::isAllowedName(".hidden", &reason));

    for (const QString &bad : {QString(), QString("."), QString(".."), QString("a/b"),
                               QString("a\\b"), QString("a:b"), QString("a?b"), QString("a*b"),
                               QString("a\"b"), QString("a<b"), QString("a|b"), QString("name."),
                               QString("name "), QString(" name"), QString("CON"),
                               QString("nul.txt"), QString("com1"), QString("LPT9.log")}) {
        QVERIFY2(!FileOps::isAllowedName(bad, &reason), qPrintable(bad));
        QVERIFY2(!reason.isEmpty(), qPrintable(bad));
    }
}

void TestFileOps::largeFilesAreCopiedInPiecesAndCanBeStopped()
{
    QTemporaryDir root;
    Folders f(root);
    QString error;
    Recorder progress;
    Answers answers;

    // Bigger than what is copied in one go.
    QByteArray data(9 * 1024 * 1024, Qt::Uninitialized);
    for (int i = 0; i < data.size(); ++i) {
        data[i] = char((i * 31 + (i >> 16)) & 0xff);
    }
    QVERIFY(writeFile(f.source + "/big.bin", data));

    // Stopped after a few reports: no half file is left behind.
    progress.stopAfter = 5;
    QVERIFY(!FileOps::copy({f.source + "/big.bin"}, f.target, answers, progress, &error));
    QCOMPARE(error, QString());
    QVERIFY(!QFileInfo::exists(f.target + "/big.bin"));
    QCOMPARE(QFileInfo(f.source + "/big.bin").size(), qint64(data.size()));

    // Through to the end: the same bytes, reported piece by piece.
    progress = Recorder();
    QVERIFY(FileOps::copy({f.source + "/big.bin"}, f.target, answers, progress, &error));
    QFile copy(f.target + "/big.bin");
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.readAll(), data);
    QVERIFY(progress.ticks > 5);
    QVERIFY(progress.neverWentBack);
    QCOMPARE(progress.lastDone, qint64(data.size()) + 1);
    QCOMPARE(progress.lastTotal, qint64(data.size()) + 1);
}

void TestFileOps::sizes()
{
    QTemporaryDir root;
    Folders f(root);
    QCOMPARE(FileOps::sizeOf(f.source + "/a.txt"), 3);
    QCOMPARE(FileOps::sizeOf(f.source + "/sub"), 9);
    QCOMPARE(FileOps::sizeOf(f.source), 12);
    QCOMPARE(FileOps::sizeOf(f.source + "/nope"), 0);
}

void TestFileOps::linksAreCopiedAsLinks()
{
#ifndef Q_OS_UNIX
    QSKIP("links are not created the same way here");
#else
    QTemporaryDir root;
    Folders f(root);
    QString error;
    Recorder progress;
    Answers answers;
    QVERIFY(QFile::link(f.source + "/a.txt", f.source + "/link"));

    QVERIFY(FileOps::copy({f.source + "/link"}, f.target, answers, progress, &error));
    const QFileInfo copied(f.target + "/link");
    QVERIFY(copied.isSymLink());
    QCOMPARE(copied.symLinkTarget(), QFileInfo(f.source + "/link").symLinkTarget());
    // Not followed: the link has no size of its own, and what it points to doesn't count.
    QCOMPARE(FileOps::sizeOf(f.source + "/link"), 0);
    QCOMPARE(FileOps::sizeOf(f.source), 12);
#endif
}

// ---------------------------------------------------------------------------
// Reviewer's tests. They assert what a user would expect; a failure is the bug.
// ---------------------------------------------------------------------------

void TestFileOps::reviewCrossVolumeMoveKeepsWhatWasSkipped()
{
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    Answers answers;
    QString error;
    makeFolder(f.target + "/sub");
    writeFile(f.target + "/sub/b.txt", "stays");

    // The move onto another volume: copy, then the source removed.
    answers.script = {FileOps::Existing::Skip};
    const bool ok = FileOps::move({f.source + "/sub"}, f.target, answers, progress, &error, nullptr,
                                  FileOps::MoveOptions{.renameOnSameVolume = false});
    qInfo() << "moved:" << ok << "error:" << error << "asked:" << answers.asked;
    // b.txt was skipped, as in the test with a rename: it must still be here.
    QVERIFY2(QFileInfo::exists(f.source + "/sub/b.txt"),
             "the file the user skipped is gone from the source");
    QCOMPARE(readFile(f.target + "/sub/b.txt"), QString("stays"));
}

void TestFileOps::reviewStopKeepsSomethingOfAFolderBeingRemoved()
{
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    QString error;
    // The first report (before the work) goes through, the next one says stop.
    progress.stopAfter = 2;
    const bool ok = FileOps::remove({f.source + "/sub"}, progress, &error);
    qInfo() << "removed:" << ok << "error:" << error << "ticks:" << progress.ticks
            << "c.txt still there:" << QFileInfo::exists(f.source + "/sub/deep/c.txt");
    // Reported as stopped by the user (false, no error), so something must be left.
    QVERIFY2(QFileInfo::exists(f.source + "/sub"), "Stop was honoured on paper, but all is gone");
}

void TestFileOps::reviewGuardSeesTheHomeFolderThroughAFirmlink()
{
#ifndef Q_OS_MACOS
    QSKIP("firmlinks exist on macOS only");
#else
    // The Data volume is also reachable under /System/Volumes/Data; that is
    // where the home folder really is.
    const QString alias = QStringLiteral("/System/Volumes/Data") + QDir::homePath();
    if (!QFileInfo::exists(alias)) {
        QSKIP("no firmlink to the Data volume here");
    }
    std::error_code ec;
    const bool same = std::filesystem::equivalent(alias.toStdString(), QDir::homePath().toStdString(), ec);
    qInfo().noquote() << "canonical:" << QFileInfo(alias).canonicalFilePath()
                      << "| same folder as home:" << same << "| protected:" << FileOps::isProtected(alias);

    // That files can be removed through the firmlink, with a scratch file in a
    // temp folder (the temp folder is on the same volume).
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QString real = QFileInfo(scratch.path()).canonicalFilePath();
    const QString through = QStringLiteral("/System/Volumes/Data") + real + "/victim.txt";
    QVERIFY(writeFile(scratch.path() + "/victim.txt", "v"));
    QVERIFY(QFileInfo::exists(through));
    Recorder progress;
    QString error;
    QVERIFY(FileOps::remove({through}, progress, &error));
    QVERIFY(!QFileInfo::exists(scratch.path() + "/victim.txt"));

    // The home folder through the firmlink is the home folder.
    QVERIFY2(FileOps::isProtected(alias), qPrintable(QFileInfo(alias).canonicalFilePath()));
#endif
}

void TestFileOps::reviewAFolderIsNotCopiedIntoItselfThroughAnAlias()
{
#ifndef Q_OS_UNIX
    QSKIP("needs symbolic links");
#else
    auto count = [](const QString &folder) {
        int files = 0;
        QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            ++files;
        }
        return files;
    };
    Recorder progress;
    Answers answers;
    QString error;

    // 1. The target folder is reached through a link to a folder inside the source.
    {
        QTemporaryDir root;
        const QString real = root.path() + "/real";
        QVERIFY(makeFolder(real + "/backup"));
        QVERIFY(makeFolder(real + "/projects/one"));
        QVERIFY(writeFile(real + "/projects/one/a.txt", "a"));
        QVERIFY(writeFile(real + "/b.txt", "b"));
        QVERIFY(QFile::link(real, root.path() + "/alias"));
        const bool ok = FileOps::copy({real}, root.path() + "/alias/backup", answers, progress, &error);
        qInfo() << "through a link: copied:" << ok << "error:" << error
                << "| files in the source tree before: 2, after:" << count(real);
        QVERIFY2(!ok && error.contains("into itself"), qPrintable(error));
        QCOMPARE(count(real), 2);
    }
#endif
}

static int reviewCountFiles(const QString &folder)
{
    int files = 0;
    QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++files;
    }
    return files;
}

void TestFileOps::reviewAFolderIsNotCopiedIntoItselfThroughAnotherCase()
{
    // The same, with the name in another case on a volume that ignores case
    // (the usual APFS volume, and every NTFS volume on Windows).
    Recorder progress;
    Answers answers;
    QString error;
    QTemporaryDir root;
    const QString real = root.path() + "/Docs";
    QVERIFY(makeFolder(real + "/backup"));
    QVERIFY(makeFolder(real + "/p1"));
    QVERIFY(makeFolder(real + "/p2"));
    QVERIFY(writeFile(real + "/backup/old.txt", "o"));
    QVERIFY(writeFile(real + "/p1/a.txt", "a"));
    QVERIFY(writeFile(real + "/p2/b.txt", "b"));
    QVERIFY(writeFile(real + "/c.txt", "c"));
    if (!QFileInfo::exists(root.path() + "/DOCS")) {
        QSKIP("the temp volume tells cases apart");
    }
    const int before = reviewCountFiles(real);
    const bool ok = FileOps::copy({real}, root.path() + "/DOCS/backup", answers, progress, &error);
    qInfo() << "through the case: copied:" << ok << "| error:" << error.left(120)
            << "| files in the source tree before:" << before << "after:" << reviewCountFiles(real);
    QVERIFY2(!ok && error.contains("into itself"), qPrintable(error.left(200)));
    QCOMPARE(reviewCountFiles(real), before);
}

void TestFileOps::reviewMoveOntoASymlinkedFolderAsksLikeCopy()
{
#ifndef Q_OS_UNIX
    QSKIP("needs symbolic links");
#else
    QTemporaryDir root;
    Folders f(root);
    const QString elsewhere = root.path() + "/elsewhere";
    QVERIFY(makeFolder(elsewhere));
    QVERIFY(QFile::link(elsewhere, f.target + "/sub"));  // dst/sub leads to elsewhere

    Recorder progress;
    Answers answers;
    QString error;
    // Copy: asks about dst/sub, as it is not a plain folder.
    const bool copied = FileOps::copy({f.source + "/sub"}, f.target, answers, progress, &error);
    qInfo() << "copy: asked" << answers.asked << "target" << answers.targets << "| done:" << copied
            << "| in elsewhere:" << QFileInfo::exists(elsewhere + "/deep");

    // Move: does it ask too?
    answers = Answers();
    progress = Recorder();
    const bool moved = FileOps::move({f.source + "/sub"}, f.target, answers, progress, &error);
    qInfo() << "move: asked" << answers.asked << "| done:" << moved << error
            << "| in elsewhere:" << QFileInfo::exists(elsewhere + "/deep")
            << "| source gone:" << !QFileInfo::exists(f.source + "/sub");
    // Nothing may end up behind a link without a word.
    QVERIFY2(!QFileInfo::exists(elsewhere + "/deep"), "the folder was moved through the link");
#endif
}

void TestFileOps::reviewAMissingSourceDoesNotRemoveItsTarget()
{
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    Answers answers;
    QString error;

    // A selection from a listing that is out of date: a.txt was moved away
    // already (say by the job before it), and the target has it.
    QVERIFY(FileOps::move({f.source + "/a.txt"}, f.target, answers, progress, &error));
    QVERIFY(!QFileInfo::exists(f.source + "/a.txt"));

    // Asked about the target, "Replace" is answered.
    answers = Answers();
    progress = Recorder();
    answers.script = {FileOps::Existing::Overwrite};
    const bool moved = FileOps::move({f.source + "/a.txt"}, f.target, answers, progress, &error);
    qInfo() << "move of what isn't there: asked" << answers.asked << "| done:" << moved << "| error:"
            << error << "| target still there:" << QFileInfo::exists(f.target + "/a.txt");
    QVERIFY2(QFileInfo::exists(f.target + "/a.txt"),
             "the only copy of the file was deleted for good");

    // The same with a folder, and a copy.
    QVERIFY(FileOps::copy({f.source + "/sub"}, f.target, answers, progress, &error));
    QVERIFY(FileOps::remove({f.source + "/sub"}, progress, &error));
    answers = Answers();
    answers.script = {FileOps::Existing::Overwrite};
    FileOps::copy({f.source + "/sub"}, f.target, answers, progress, &error);
    qInfo() << "copy of what isn't there: asked" << answers.asked << "| c.txt in the target still there:"
            << QFileInfo::exists(f.target + "/sub/deep/c.txt");
    QVERIFY2(QFileInfo::exists(f.target + "/sub/deep/c.txt"),
             "the only copy of the folder was deleted for good");
}

// A file on macOS has more than its bytes: tags, Finder information, the
// quarantine mark. Kept by the copy in one go, and by the copy in pieces?
void TestFileOps::reviewBothCopyWaysKeepWhatAFileHasBesidesItsBytes()
{
#ifdef Q_OS_MACOS
    QTemporaryDir root;
    Folders f(root);
    Recorder progress;
    Answers answers;
    QString error;
    QVERIFY(writeFile(f.source + "/small.bin", QByteArray(1000, 's')));
    QVERIFY(writeFile(f.source + "/big.bin", QByteArray(9 * 1024 * 1024, 'b')));
    const char *name = "com.example.review-tag";
    const QByteArray value("a tag");
    for (const char *file : {"/small.bin", "/big.bin"}) {
        const QByteArray path = (f.source + file).toLatin1();
        QVERIFY(setxattr(path.constData(), name, value.constData(), value.size(), 0, 0) == 0);
    }
    QVERIFY(FileOps::copy({f.source + "/small.bin", f.source + "/big.bin"}, f.target, answers, progress,
                          &error));
    char buffer[64];
    const auto tag = [&](const QString &path) -> int {
        return getxattr(path.toLatin1().constData(), name, buffer, sizeof(buffer), 0, 0);
    };
    QCOMPARE(tag(f.target + "/small.bin"), tag(f.source + "/small.bin"));
    QCOMPARE(tag(f.target + "/big.bin"), tag(f.source + "/big.bin"));
#endif
}

QTEST_MAIN(TestFileOps)
#include "fileops.moc"
