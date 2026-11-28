#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTableView>
#include <QComboBox>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <utility>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "helpers.h"
#include "tools/DuplicateFinder.h"
#include "tools/DuplicatesWindow.h"

namespace {

namespace fs = std::filesystem;
namespace df = DuplicateFinder;

constexpr qint64 kKB = 1024;

// A file's worth of content that `seed` decides, so that files of one size
// can still differ: the seed is written at the start, where files are
// compared first.
QByteArray content(int seed, qint64 bytes)
{
    QByteArray data(int(bytes), Qt::Uninitialized);
    for (qsizetype i = 0; i < data.size(); ++i) {
        data[i] = char((i * 7) & 0xff);
    }
    for (int k = 0; k < 4 && k < data.size(); ++k) {
        data[k] = char((seed >> (8 * k)) & 0xff);
    }
    return data;
}

QDateTime date(int year)
{
    return QDateTime(QDate(year, 6, 15), QTime(12, 0));
}

// Files in a temporary folder. A file that couldn't be written isn't said so
// where it is written; `failed` says it once for all.
struct Tree {
    QTemporaryDir root;
    bool failed = false;

    QString at(const QString &relative) const { return root.path() + QLatin1Char('/') + relative; }

    void file(const QString &relative, const QByteArray &data)
    {
        failed = !(makeFolder(QFileInfo(at(relative)).path()) && writeFile(at(relative), data)) || failed;
    }
};

QStringList paths(const df::Group &group)
{
    QStringList result;
    for (const df::File &file : group.files) {
        result.append(file.path);
    }
    return result;
}

// Tells groups apart by where their first file is.
QSet<QString> starts(const df::Result &scan)
{
    QSet<QString> result;
    for (const df::Group &group : scan.groups) {
        result.insert(group.files.first().path);
    }
    return result;
}

// A path from the root of the disk, as this system writes it; "" is the root.
QString rooted(const char *relative)
{
    return QDir::cleanPath(QDir::rootPath() + QLatin1Char('/') + QString::fromLatin1(relative));
}

// A group as the finder lists one, but without any file on disk, to try the
// rules on: each file is a path from the root of the disk, and the year it
// was modified in.
df::Group makeGroup(std::initializer_list<std::pair<const char *, int>> files, qint64 size = 100)
{
    df::Group group;
    group.size = size;
    for (const auto &[path, year] : files) {
        group.files.append(df::File{rooted(path), date(year)});
    }
    return group;
}

df::Rule rule(df::Rule::Kind kind, const char *folder = nullptr)
{
    df::Rule result;
    result.kind = kind;
    if (folder) {
        result.folder = rooted(folder);
    }
    return result;
}

// A scan that is never stopped, with the options a user starts with unless
// given others.
df::Result scan(const QStringList &folders, const df::Report &report = {}, qint64 minSize = 1 * kKB,
                bool includeEmpty = false, const QString &pattern = {}, bool subfolders = true)
{
    df::Options options;
    options.folders = folders;
    options.minSize = minSize;
    options.includeEmpty = includeEmpty;
    options.pattern = pattern;
    options.subfolders = subfolders;
    std::atomic<bool> stop = false;
    return df::find(options, stop, report);
}

// A window on a tree with two groups of alike files: three photos, of which
// the one in a is the oldest and the one in b the newest, and two songs.
struct Window {
    QTemporaryDir root;
    QString a;
    QString b;
    QString c;
    ScriptedDialogs dialogs;
    std::unique_ptr<DuplicatesWindow> window;
    bool ready = false;

    QString at(const char *relative) const { return root.path() + QLatin1Char('/') + QString::fromLatin1(relative); }

    Window()
        : a(at("a"))
        , b(at("b"))
        , c(at("c"))
    {
        const QByteArray photo = content(1, 4000);
        const QByteArray song = content(2, 2500);
        makeFolder(a);
        makeFolder(b);
        makeFolder(c);
        writeFile(at("a/photo.bin"), photo);
        writeFile(at("b/photo.bin"), photo);
        writeFile(at("c/photo copy.bin"), photo);
        writeFile(at("a/song.bin"), song);
        writeFile(at("b/song.bin"), song);
        writeFile(at("a/unique.bin"), content(3, 3000));
        setModified(at("a/photo.bin"), date(2020));
        setModified(at("b/photo.bin"), date(2024));
        setModified(at("c/photo copy.bin"), date(2022));
        setModified(at("a/song.bin"), date(2020));
        setModified(at("b/song.bin"), date(2024));

        window = std::make_unique<DuplicatesWindow>(dialogs);
        window->addFolder(root.path());
        window->show();
        ready = QTest::qWaitForWindowExposed(window.get());
    }

    // Scans to the end; false when that took too long.
    bool scan()
    {
        window->startScan();
        return settle(&DuplicatesWindow::isScanning);
    }

    // Waits until `running` says the window is no longer busy; false on timeout.
    bool settle(bool (DuplicatesWindow::*running)() const)
    {
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 15000;
        while ((window.get()->*running)() && QDateTime::currentMSecsSinceEpoch() < deadline) {
            QTest::qWait(5);
        }
        return !(window.get()->*running)();
    }

    // The row of a file in the list, or of the group's own row for -1, in the column of the boxes.
    QModelIndex row(int group, int file) const
    {
        const QAbstractItemModel *model = window->list()->model();
        int result = 0;
        for (int g = 0; g < group; ++g) {
            result += 1 + int(window->groups().at(g).files.size());
        }
        return model->index(file < 0 ? result : result + 1 + file, 0);
    }

    void click(const QModelIndex &index)
    {
        QTest::mouseClick(window->list()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          window->list()->visualRect(index).center());
    }

    QAbstractButton *button(const QString &text) const
    {
        for (QAbstractButton *found : window->findChildren<QAbstractButton *>()) {
            if (found->text() == text) {
                return found;
            }
        }
        return nullptr;
    }
};

// A tree of `copies` pairs of files, each pair alike and no pair like another, and long enough that a scan takes a moment.
QString bigTree(QTemporaryDir &root, int copies)
{
    for (int i = 0; i < copies; ++i) {
        const QByteArray data = content(1000 + i, 96 * kKB);
        makeFolder(root.path() + QStringLiteral("/g%1").arg(i));
        writeFile(root.path() + QStringLiteral("/g%1/a.bin").arg(i), data);
        writeFile(root.path() + QStringLiteral("/g%1/b.bin").arg(i), data);
    }
    return root.path();
}

}  // namespace

class TestDuplicates : public QObject {
    Q_OBJECT

private slots:
    void identicalFilesInDifferentFoldersAreOneGroup();
    void theSameSizeWithOtherContentIsNotAGroup();
    void filesThatDifferLateAreToldApart();
    void emptyFilesAreAnOption();
    void minSizePatternAndSubfoldersDecideWhatIsCompared();
    void foldersNestedTooDeeplyAreLeftOut();
    void linksAreLeftAlone();
    void aFolderThatCantBeReadIsLeftOutAndCounted();
    void aFolderGivenTwiceScansItsFilesOnce();
    void twoNamesOfOneFileAreOneCopy();
    void groupsAreOrderedByWastedSpace();
    void progressIsReported();
    void stopEndsAScanQuickly();
    void unchangedTellsAFileThatChanged();

    void ruleFirstKeepsTheFirst();
    void ruleNewestAndOldestKeepThatOne();
    void ruleInFolderKeepsTheCopiesThere();
    void ruleNotInFolderRemovesTheCopiesThere();
    void whateverTheRuleOneCopyStays();

    void windowListsTheGroupsOfAScan();
    void windowMarksByRule();
    void windowKeepsOneCopyOfEachGroup();
    void windowCollapsesAGroup();
    void windowRemovesOnlyWhatWasAskedFor();
    void windowAsksBeforeMovingToTheTrash();
    void windowKeepsAFileThatChangedSinceTheScan();
    void windowLeavesAGroupWhoseOtherCopiesAreGone();
    void windowRemembersItsOptions();
    void windowStaysResponsiveDuringAScan();
    void windowStopsAScan();
};

void TestDuplicates::identicalFilesInDifferentFoldersAreOneGroup()
{
    Tree t;
    const QByteArray photo = content(1, 5000);
    t.file("a/photo.bin", photo);
    t.file("b/photo copy.bin", photo);
    t.file("c/other.bin", content(2, 5000));  // as big, but something else
    t.file("a/readme.txt", content(3, 3000));
    QVERIFY(!t.failed);

    const df::Result result = scan({t.root.path()});

    QCOMPARE(result.groups.size(), 1);
    QCOMPARE(result.files, 4);
    QCOMPARE(result.unreadable, 0);
    QVERIFY(!result.stopped);
    const df::Group &group = result.groups.first();
    QCOMPARE(group.size, 5000);
    QCOMPARE(group.wasted(), 5000);
    // In path order, which is what "the first copy" goes by.
    QCOMPARE(paths(group), QStringList({t.at("a/photo.bin"), t.at("b/photo copy.bin")}));
}

void TestDuplicates::theSameSizeWithOtherContentIsNotAGroup()
{
    Tree t;
    t.file("a/one.bin", content(1, 200 * kKB));
    t.file("b/two.bin", content(2, 200 * kKB));  // differs in the first byte
    QVERIFY(!t.failed);

    QList<df::Progress> reports;
    const df::Result result = scan({t.root.path()}, [&](const df::Progress &p) { reports.append(p); });

    QVERIFY(result.groups.isEmpty());
    // Only the start of each was read: that is what tells them apart.
    QCOMPARE(reports.last().bytesRead, 2 * df::kHeadSize);
}

void TestDuplicates::filesThatDifferLateAreToldApart()
{
    // Longer than the part that is compared first.
    {
        Tree t;
        const QByteArray original = content(5, 200 * kKB);
        QByteArray late = original;
        late[late.size() - 1] = char(uchar(late[late.size() - 1]) ^ 0xff);
        t.file("a/x.bin", original);
        t.file("b/y.bin", late);  // the same up to the last byte
        t.file("c/z.bin", original);
        QVERIFY(!t.failed);

        QList<df::Progress> reports;
        const df::Result result = scan({t.root.path()}, [&](const df::Progress &p) { reports.append(p); });

        QCOMPARE(result.groups.size(), 1);
        QCOMPARE(paths(result.groups.first()), QStringList({t.at("a/x.bin"), t.at("c/z.bin")}));
        // All three got as far as being read whole.
        QCOMPARE(reports.last().bytesRead, 3 * df::kHeadSize + 3 * 200 * kKB);
    }
    // No longer than that: then what was compared first is all there is.
    {
        Tree t;
        const QByteArray original = content(5, 4000);
        QByteArray late = original;
        late[late.size() - 1] = char(uchar(late[late.size() - 1]) ^ 0xff);
        t.file("a/x.bin", original);
        t.file("b/y.bin", late);
        t.file("c/z.bin", original);
        QVERIFY(!t.failed);

        QList<df::Progress> reports;
        const df::Result result = scan({t.root.path()}, [&](const df::Progress &p) { reports.append(p); });

        QCOMPARE(result.groups.size(), 1);
        QCOMPARE(paths(result.groups.first()), QStringList({t.at("a/x.bin"), t.at("c/z.bin")}));
        QCOMPARE(reports.last().bytesRead, 3 * 4000);
    }
}

void TestDuplicates::emptyFilesAreAnOption()
{
    Tree t;
    t.file("a/e1.tmp", "");
    t.file("b/e2.tmp", "");
    t.file("c/e3.tmp", "");
    QVERIFY(!t.failed);

    // Left out by default.
    df::Result result = scan({t.root.path()});
    QVERIFY(result.groups.isEmpty());
    QCOMPARE(result.files, 0);

    result = scan({t.root.path()}, {}, 1 * kKB, true);
    QCOMPARE(result.groups.size(), 1);
    QCOMPARE(result.groups.first().size, 0);
    QCOMPARE(result.groups.first().wasted(), 0);
    QCOMPARE(result.groups.first().files.size(), 3);
    QCOMPARE(result.files, 3);

    // The minimum size is for files that have a size.
    result = scan({t.root.path()}, {}, 1000 * kKB, true);
    QCOMPARE(result.groups.size(), 1);
}

void TestDuplicates::minSizePatternAndSubfoldersDecideWhatIsCompared()
{
    Tree t;
    const QByteArray jpg = content(2, 4000);
    const QByteArray txt = content(3, 4000);
    const QByteArray inner = content(4, 6000);
    t.file("a/small.bin", content(1, 900));
    t.file("b/small.bin", content(1, 900));
    t.file("a/one.jpg", jpg);
    t.file("b/one.jpg", jpg);
    t.file("a/two.TXT", txt);
    t.file("b/two.txt", txt);
    t.file("a/sub/inner.jpg", inner);
    t.file("b/sub/inner.jpg", inner);
    QVERIFY(!t.failed);

    // Below the minimum of 1 KB is the small pair; the subfolders are looked in.
    df::Result result = scan({t.root.path()});
    QCOMPARE(starts(result),
             QSet<QString>({t.at("a/one.jpg"), t.at("a/two.TXT"), t.at("a/sub/inner.jpg")}));
    QCOMPARE(result.files, 6);

    result = scan({t.root.path()}, {}, 0);
    QCOMPARE(result.groups.size(), 4);
    QCOMPARE(result.files, 8);

    // Several patterns, which match the name and don't mind its case.
    const QStringList both = {t.at("a"), t.at("b")};
    result = scan(both, {}, 1 * kKB, false, "*.jpg", false);
    QCOMPARE(starts(result), QSet<QString>({t.at("a/one.jpg")}));
    QCOMPARE(result.files, 2);

    result = scan(both, {}, 1 * kKB, false, "*.jpg *.txt", false);
    QCOMPARE(starts(result), QSet<QString>({t.at("a/one.jpg"), t.at("a/two.TXT")}));

    result = scan(both, {}, 1 * kKB, false, "*.JPG;*.TXT", false);
    QCOMPARE(starts(result), QSet<QString>({t.at("a/one.jpg"), t.at("a/two.TXT")}));

    // Nothing given is everything, and only what lies directly in the folders
    // given: not what is in the folders below them.
    result = scan(both, {}, 1 * kKB, false, "   ", false);
    QCOMPARE(starts(result), QSet<QString>({t.at("a/one.jpg"), t.at("a/two.TXT")}));
    QCOMPARE(result.files, 4);

    result = scan(both, {}, 1 * kKB, false, "*", false);
    QCOMPARE(starts(result), QSet<QString>({t.at("a/one.jpg"), t.at("a/two.TXT")}));
    QCOMPARE(result.files, 4);
}

void TestDuplicates::foldersNestedTooDeeplyAreLeftOut()
{
    Tree t;
    QString shallow;
    QString deep;
    for (int i = 0; i < 70; ++i) {
        deep += "d/";
        if (i < 10) {
            shallow += "d/";
        }
    }
    const QByteArray data = content(6, 2000);
    t.file(shallow + "one.bin", data);
    t.file(shallow + "two.bin", data);
    t.file(deep + "one.bin", data);
    t.file(deep + "two.bin", data);
    QVERIFY(!t.failed);

    const df::Result result = scan({t.root.path()});

    QCOMPARE(result.groups.size(), 1);
    QCOMPARE(result.groups.first().files.first().path, t.at(shallow + "one.bin"));
}

void TestDuplicates::linksAreLeftAlone()
{
#ifndef Q_OS_UNIX
    QSKIP("QFile::link makes something else than a symbolic link on Windows");
#else
    Tree t;
    const QByteArray data = content(8, 3000);
    t.file("a/one.bin", data);
    t.file("a/two.bin", data);
    t.file("b/only.bin", content(9, 3000));
    QVERIFY(!t.failed);

    // A link to a file is no copy: there are two in the group, not three.
    QVERIFY(QFile::link(t.at("b/only.bin"), t.at("a/link.bin")));

    // Links between folders lead nowhere: not back to where the scan began,
    // nor round in circles.
    QVERIFY(makeFolder(t.at("loop")));
    QVERIFY(QFile::link(t.root.path(), t.at("loop/back")));
    QVERIFY(QFile::link(t.at("loop"), t.at("a/round")));

    const df::Result result = scan({t.root.path()});

    QCOMPARE(result.groups.size(), 1);
    QCOMPARE(paths(result.groups.first()), QStringList({t.at("a/one.bin"), t.at("a/two.bin")}));
    QCOMPARE(result.files, 3);
#endif
}

void TestDuplicates::aFolderThatCantBeReadIsLeftOutAndCounted()
{
#ifndef Q_OS_UNIX
    QSKIP("folder permissions are changed with chmod");
#else
    if (::geteuid() == 0) {
        QSKIP("root reads every folder");
    }
    Tree t;
    const QByteArray data = content(10, 3000);
    t.file("open/one.bin", data);
    t.file("open/two.bin", data);
    t.file("locked/one.bin", data);
    t.file("locked/two.bin", data);
    QVERIFY(!t.failed);

    // Put back when the test is over, however it ends: else the temporary
    // folder can't be removed.
    struct Restore {
        QString path;
        ~Restore() { ::chmod(QFile::encodeName(path).constData(), 0700); }
    } restore{t.at("locked")};
    QCOMPARE(::chmod(QFile::encodeName(t.at("locked")).constData(), 0), 0);

    const df::Result result = scan({t.root.path()});

    QCOMPARE(result.unreadable, 1);
    QCOMPARE(result.groups.size(), 1);
    QCOMPARE(result.groups.first().files.first().path, t.at("open/one.bin"));
    QCOMPARE(result.files, 2);

    // What is no folder at all, or is no more, counts the same way.
    QCOMPARE(scan({t.at("open/one.bin")}).unreadable, 1);
    QCOMPARE(scan({t.at("nowhere")}).unreadable, 1);
    QCOMPARE(scan({}).unreadable, 0);
#endif
}

void TestDuplicates::aFolderGivenTwiceScansItsFilesOnce()
{
    Tree t;
    const QByteArray data = content(11, 4000);
    t.file("a/one.bin", data);
    t.file("a/two.bin", data);
    QVERIFY(!t.failed);

    // The second lies in the first: each file once, so a group of two and not of four.
    df::Result result = scan({t.root.path(), t.at("a")});
    QCOMPARE(result.groups.size(), 1);
    QCOMPARE(result.groups.first().files.size(), 2);
    QCOMPARE(result.groups.first().wasted(), 4000);
    QCOMPARE(result.files, 2);

    result = scan({t.at("a"), t.root.path()});
    QCOMPARE(result.groups.size(), 1);
    QCOMPARE(result.groups.first().files.size(), 2);
    QCOMPARE(result.files, 2);

#ifndef Q_OS_UNIX
    QSKIP("the rest needs a link to a folder");
#else
    // The same folder by another name is the same folder.
    QVERIFY(QFile::link(t.at("a"), t.at("alias")));
    result = scan({t.at("a"), t.at("alias")});
    QCOMPARE(result.groups.size(), 1);
    QCOMPARE(result.groups.first().files.size(), 2);
    QCOMPARE(result.files, 2);

    // Given as the link alone, it is scanned through the link.
    result = scan({t.at("alias")});
    QCOMPARE(result.groups.size(), 1);
    QCOMPARE(paths(result.groups.first()), QStringList({t.at("alias/one.bin"), t.at("alias/two.bin")}));
#endif
}

void TestDuplicates::twoNamesOfOneFileAreOneCopy()
{
    Tree t;
    const QByteArray data = content(12, 3000);
    t.file("a/one.bin", data);
    QVERIFY(makeFolder(t.at("b")));
    QVERIFY(!t.failed);

    std::error_code ec;
    fs::create_hard_link(fs::path(t.at("a/one.bin").toStdString()), fs::path(t.at("b/two.bin").toStdString()),
                         ec);
    if (ec) {
        QSKIP("this disk doesn't make hard links");
    }

    // One file with two names has no copy.
    df::Result result = scan({t.root.path()});
    QVERIFY(result.groups.isEmpty());
    QCOMPARE(result.files, 2);

    // With a copy, the group is the file and the copy: one copy's worth to
    // gain, not two.
    t.file("c/three.bin", data);
    result = scan({t.root.path()});
    QCOMPARE(result.groups.size(), 1);
    QCOMPARE(result.groups.first().files.size(), 2);
    QCOMPARE(result.groups.first().wasted(), 3000);
}

void TestDuplicates::groupsAreOrderedByWastedSpace()
{
    Tree t;
    const char *folders[4] = {"a", "b", "c", "d"};
    const auto spread = [&](const QString &name, int seed, qint64 size, int copies) {
        const QByteArray data = content(seed, size);
        for (int i = 0; i < copies; ++i) {
            t.file(QString(folders[i]) + name, data);
        }
    };
    spread("/p.bin", 1, 100 * kKB, 3);  // 200 KB in the two copies beyond the first
    spread("/q.bin", 2, 150 * kKB, 2);  // 150 KB, though each file is the largest of all
    spread("/r.bin", 3, 40 * kKB, 4);   // 120 KB
    spread("/s.bin", 4, 100 * kKB, 2);  // 100 KB
    QVERIFY(!t.failed);

    const df::Result result = scan({t.root.path()});

    QStringList order;
    for (const df::Group &group : result.groups) {
        order.append(QFileInfo(group.files.first().path).fileName());
    }
    // Not by the size of a file: by what the spare copies take up.
    QCOMPARE(order, QStringList({"p.bin", "q.bin", "r.bin", "s.bin"}));
    QCOMPARE(result.groups.at(0).files.size(), 3);
    QCOMPARE(result.groups.at(2).files.size(), 4);
    QCOMPARE(result.wasted(), (200 + 150 + 120 + 100) * kKB);
}

void TestDuplicates::progressIsReported()
{
    Tree t;
    const QByteArray photo = content(1, 5000);
    t.file("a/photo.bin", photo);
    t.file("b/photo.bin", photo);
    t.file("c/other.bin", content(2, 5000));
    t.file("readme.txt", content(3, 3000));
    QVERIFY(!t.failed);

    QList<df::Progress> reports;
    scan({t.root.path()}, [&](const df::Progress &p) { reports.append(p); });

    // At least when the comparing begins, and once at the end with the totals.
    QVERIFY(reports.size() >= 2);
    QCOMPARE(reports.last().stage, df::Progress::Stage::Comparing);
    QCOMPARE(reports.last().files, 4);
    // The three of one size were read; none of them is long enough to be read twice.
    QCOMPARE(reports.last().bytesRead, 3 * 5000);
    QCOMPARE(reports.last().bytesTotal, 3 * 5000);

    // While the folders are read nothing is to be read yet; and the numbers
    // only go up.
    for (const df::Progress &p : reports) {
        if (p.stage == df::Progress::Stage::Walking) {
            QCOMPARE(p.bytesRead, 0);
            QCOMPARE(p.bytesTotal, 0);
        }
    }
    for (int i = 1; i < reports.size(); ++i) {
        QVERIFY(reports.at(i).stage >= reports.at(i - 1).stage);
        QVERIFY(reports.at(i).files >= reports.at(i - 1).files);
        QVERIFY(reports.at(i).bytesRead >= reports.at(i - 1).bytesRead);
    }
}

void TestDuplicates::stopEndsAScanQuickly()
{
    Tree t;
    for (int i = 0; i < 400; ++i) {
        const QByteArray data = content(i, 32 * kKB);
        t.file(QString("g%1/a.bin").arg(i), data);
        t.file(QString("g%1/b.bin").arg(i), data);
    }
    QVERIFY(!t.failed);

    df::Options options;
    options.folders = {t.root.path()};

    // Stopped before it began: nothing is read.
    std::atomic<bool> stop = true;
    QElapsedTimer timer;
    timer.start();
    df::Result result = df::find(options, stop);
    QVERIFY(timer.elapsed() < 1000);
    QVERIFY(result.stopped);
    QCOMPARE(result.files, 0);
    QVERIFY(result.groups.isEmpty());

    // Stopped while the folders are read.
    stop = false;
    result = df::find(options, stop, [&](const df::Progress &) { stop = true; });
    QVERIFY(result.stopped);
    QVERIFY(result.files < 800);
    QVERIFY(result.groups.isEmpty());

    // Stopped as the contents are compared: after one file at the very most.
    stop = false;
    QList<df::Progress> reports;
    result = df::find(options, stop, [&](const df::Progress &p) {
        reports.append(p);
        stop = p.stage == df::Progress::Stage::Comparing;
    });
    QVERIFY(result.stopped);
    QCOMPARE(result.files, 800);
    QVERIFY(result.groups.isEmpty());
    QVERIFY(reports.last().bytesRead <= 32 * kKB);

    // For comparison, the same scan run to its end. The walk is long enough
    // here to be reported while it goes on.
    stop = false;
    reports.clear();
    result = df::find(options, stop, [&](const df::Progress &p) { reports.append(p); });
    QVERIFY(!result.stopped);
    QCOMPARE(result.groups.size(), 400);
    QCOMPARE(result.wasted(), 400 * 32 * kKB);
    QCOMPARE(reports.first().stage, df::Progress::Stage::Walking);
    QCOMPARE(reports.first().bytesRead, 0);
    QCOMPARE(reports.first().bytesTotal, 0);
    QCOMPARE(reports.last().stage, df::Progress::Stage::Comparing);
    QCOMPARE(reports.last().files, 800);
    // No file is longer than what is compared first: nothing is read twice.
    QCOMPARE(reports.last().bytesRead, 800 * 32 * kKB);
    QCOMPARE(reports.last().bytesTotal, 800 * 32 * kKB);
}

void TestDuplicates::unchangedTellsAFileThatChanged()
{
    Tree t;
    const QByteArray data = content(13, 4000);
    t.file("one.bin", data);
    t.file("two.bin", data);
    QVERIFY(!t.failed);

    const df::Result result = scan({t.root.path()});
    const df::File file = result.groups.first().files.first();
    QCOMPARE(file.path, t.at("one.bin"));
    QVERIFY(df::unchanged(file, 4000));

    // Another date.
    setModified(file.path, date(2001));
    QVERIFY(!df::unchanged(file, 4000));
    setModified(file.path, file.modified);
    QVERIFY(df::unchanged(file, 4000));

    // Another size, or gone.
    writeFile(file.path, content(14, 3000));
    QVERIFY(!df::unchanged(file, 4000));
    QVERIFY(!df::unchanged(df::File{t.at("nowhere.bin"), file.modified}, 4000));

    // A folder of that name now.
    QVERIFY(QFile::remove(file.path));
    QVERIFY(makeFolder(file.path));
    QVERIFY(!df::unchanged(file, 4000));

#ifndef Q_OS_UNIX
    QSKIP("the rest needs a symbolic link");
#else
    // A link of that name now: it is not followed to see what it points at.
    QVERIFY(QDir().rmdir(file.path));
    QVERIFY(QFile::link(t.at("two.bin"), file.path));
    QVERIFY(!df::unchanged(file, 4000));
#endif
}

void TestDuplicates::ruleFirstKeepsTheFirst()
{
    const df::Group group = makeGroup({{"/a/one", 2020}, {"/b/two", 2020}, {"/c/three", 2020}});
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::First)), QList<int>({1, 2}));
    QCOMPARE(df::toRemove(group, df::Rule()), QList<int>({1, 2}));
}

void TestDuplicates::ruleNewestAndOldestKeepThatOne()
{
    df::Group group = makeGroup({{"/a/one", 2020}, {"/b/two", 2024}, {"/c/three", 2022}});
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::Newest)), QList<int>({0, 2}));
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::Oldest)), QList<int>({1, 2}));

    // Of two that are as new, or as old, the first stays.
    group = makeGroup({{"/a/one", 2024}, {"/b/two", 2024}, {"/c/three", 2022}});
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::Newest)), QList<int>({1, 2}));
    group = makeGroup({{"/a/one", 2024}, {"/b/two", 2022}, {"/c/three", 2022}});
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::Oldest)), QList<int>({0, 2}));

    // Without any date to go by, the first stays.
    group = makeGroup({{"/a/one", 2024}, {"/b/two", 2022}, {"/c/three", 2022}});
    for (df::File &file : group.files) {
        file.modified = QDateTime();
    }
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::Newest)), QList<int>({1, 2}));
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::Oldest)), QList<int>({1, 2}));
}

void TestDuplicates::ruleInFolderKeepsTheCopiesThere()
{
    df::Group group = makeGroup({{"/a/one", 2020}, {"/b/two", 2020}, {"/c/three", 2020}});
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::InFolder, "/b")), QList<int>({0, 2}));

    // Every copy that is there stays.
    group = makeGroup({{"/a/one", 2020}, {"/b/two", 2020}, {"/b/three", 2020}, {"/c/four", 2020}});
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::InFolder, "/b")), QList<int>({0, 3}));

    // What lies below the folder lies in it.
    group = makeGroup({{"/a/one", 2020}, {"/b/deep/down/two", 2020}, {"/c/three", 2020}});
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::InFolder, "/b")), QList<int>({0, 2}));
    QVERIFY(df::isInFolder(rooted("/b/deep/down/two"), rooted("/b")));
    QVERIFY(!df::isInFolder(rooted("/bc/two"), rooted("/b")));  // the same letters, another folder
    QVERIFY(!df::isInFolder(rooted("/b/two"), QString()));

    // No copy is in that folder: then nothing is removed, as there is nothing
    // that could be kept instead.
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::InFolder, "/nowhere")), QList<int>());
    QCOMPARE(df::toRemove(group, df::Rule{df::Rule::Kind::InFolder, QString()}), QList<int>());
}

void TestDuplicates::ruleNotInFolderRemovesTheCopiesThere()
{
    df::Group group = makeGroup({{"/a/one", 2020}, {"/b/two", 2020}, {"/c/three", 2020}});
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::NotInFolder, "/b")), QList<int>({1}));

    // The ones elsewhere, wherever they are.
    group = makeGroup({{"/a/one", 2020}, {"/b/two", 2020}, {"/a/deep/three", 2020}, {"/c/four", 2020}});
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::NotInFolder, "/a")), QList<int>({0, 2}));

    // Every copy is in that folder: one stays.
    group = makeGroup({{"/a/one", 2020}, {"/a/two", 2020}, {"/a/three", 2020}});
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::NotInFolder, "/a")), QList<int>({1, 2}));
    QCOMPARE(df::toRemove(group, rule(df::Rule::Kind::NotInFolder, "")), QList<int>({1, 2}));
}

void TestDuplicates::whateverTheRuleOneCopyStays()
{
    const QList<df::Rule::Kind> kinds = {df::Rule::Kind::First,      df::Rule::Kind::Newest,
                                         df::Rule::Kind::Oldest,     df::Rule::Kind::InFolder,
                                         df::Rule::Kind::NotInFolder};
    const char *folders[] = {"/f0", "/f1", "/f2", "", "/none"};
    for (int count = 0; count <= 6; ++count) {
        df::Group group;
        group.size = 10;
        for (int i = 0; i < count; ++i) {
            group.files.append(df::File{rooted(qPrintable(QStringLiteral("/f%1/file%2").arg(i % 3).arg(i))),
                                         date(2000 + (i * 7) % 5)});
        }
        for (const df::Rule::Kind kind : kinds) {
            for (const char *folder : folders) {
                const QString what = QStringLiteral("%1 files, rule %2, folder %3")
                                             .arg(count)
                                             .arg(int(kind))
                                             .arg(QString::fromLatin1(folder));
                const QList<int> remove = df::toRemove(group, rule(kind, folder));
                // Never all of them, each once, and only what is there.
                QVERIFY2(count < 2 ? remove.isEmpty() : remove.size() < count, qPrintable(what));
                QCOMPARE(remove.size(), QSet<int>(remove.begin(), remove.end()).size());
                for (int i : remove) {
                    QVERIFY2(0 <= i && i < count, qPrintable(what));
                    if (kind == df::Rule::Kind::InFolder) {
                        QVERIFY2(!df::isInFolder(group.files.at(i).path, rule(kind, folder).folder),
                                 qPrintable(what));
                    }
                    if (kind == df::Rule::Kind::NotInFolder) {
                        QVERIFY2(df::isInFolder(group.files.at(i).path, rule(kind, folder).folder),
                                 qPrintable(what));
                    }
                }
                // The rules that don't go by a folder always leave exactly the one they chose.
                const bool byFolder = kind == df::Rule::Kind::InFolder || kind == df::Rule::Kind::NotInFolder;
                if (!byFolder) {
                    QVERIFY2(remove.size() == qMax(0, count - 1), qPrintable(what));
                }
            }
        }
    }
}

void TestDuplicates::windowListsTheGroupsOfAScan()
{
    Window w;
    QVERIFY(w.ready);
    QCOMPARE(w.window->options().folders, QStringList({w.root.path()}));
    QVERIFY(w.window->groups().isEmpty());
    QVERIFY(!w.window->isScanning());
    QCOMPARE(w.window->checkedPaths(), QStringList());
    QVERIFY(w.window->statusText().contains(QStringLiteral("Click Scan")));
    QVERIFY(!w.button(QStringLiteral("Stop"))->isEnabled());
    QVERIFY(!w.button(QStringLiteral("Delete for good"))->isEnabled());

    QVERIFY(w.scan());
    QVERIFY(!w.window->isScanning());
    QCOMPARE(w.window->groups().size(), 2);
    QCOMPARE(paths(w.window->groups().at(0)),
             QStringList({w.at("a/photo.bin"), w.at("b/photo.bin"), w.at("c/photo copy.bin")}));
    QCOMPARE(w.window->groups().at(0).wasted(), 8000);
    QCOMPARE(paths(w.window->groups().at(1)), QStringList({w.at("a/song.bin"), w.at("b/song.bin")}));

    // "6 files checked, 2 groups, <10.5 kB> can be freed"
    QCOMPARE(w.window->statusText(),
             QStringLiteral("%1 files checked, 2 groups, %2 can be freed")
                     .arg(QLocale::system().toString(6))
                     .arg(humanSize(10500)));
    QCOMPARE(w.button(QStringLiteral("Stop"))->toolTip(), QString());  // nothing is said of a button that can't be used
    QVERIFY(!w.button(QStringLiteral("Stop"))->isEnabled());
    QVERIFY(w.button(QStringLiteral("Scan"))->isEnabled());

    // A scan that finds nothing says so.
    QFile::remove(w.at("b/song.bin"));
    QFile::remove(w.at("c/photo copy.bin"));
    QFile::remove(w.at("b/photo.bin"));
    QVERIFY(w.scan());
    QVERIFY(w.window->groups().isEmpty());
    QVERIFY(w.window->statusText().contains(QStringLiteral("no duplicates found")));

    // Without a folder there is nothing to scan.
    w.window->removeFolder(w.root.path());
    QVERIFY(w.window->options().folders.isEmpty());
    QVERIFY(!w.button(QStringLiteral("Scan"))->isEnabled());
    QVERIFY(w.window->statusText().contains(QStringLiteral("Add a folder")));
}

void TestDuplicates::windowMarksByRule()
{
    Window w;
    QVERIFY(w.ready && w.scan());
    const int photo = 0;
    const int song = 1;
    using Kind = df::Rule::Kind;

    // In one group.
    w.window->mark(df::Rule{Kind::Newest}, photo);
    QCOMPARE(w.window->checkedPaths(), QStringList({w.at("a/photo.bin"), w.at("c/photo copy.bin")}));
    QCOMPARE(w.window->totalText(), QStringLiteral("2 files marked, %1").arg(humanSize(8000)));
    QCOMPARE(w.row(photo, 0).data(Qt::CheckStateRole).value<Qt::CheckState>(), Qt::Checked);
    QCOMPARE(w.row(photo, 1).data(Qt::CheckStateRole).value<Qt::CheckState>(), Qt::Unchecked);
    QCOMPARE(w.row(photo, -1).data(Qt::CheckStateRole), QVariant());

    // In all of them, with the rule that was chosen before replaced.
    w.window->mark(df::Rule{Kind::Oldest}, -1);
    QCOMPARE(w.window->checkedPaths(),
             QStringList({w.at("b/photo.bin"), w.at("c/photo copy.bin"), w.at("b/song.bin")}));

    w.window->mark(df::Rule{Kind::InFolder, w.b}, -1);
    QCOMPARE(w.window->checkedPaths(),
             QStringList({w.at("a/photo.bin"), w.at("c/photo copy.bin"), w.at("a/song.bin")}));
    // Only in the group named: from the photo group, the copy that is in b.
    w.window->mark(df::Rule{Kind::NotInFolder, w.b}, photo);
    QCOMPARE(w.window->checkedPaths(), QStringList({w.at("b/photo.bin")}));

    // The rule the window has chosen is the one the buttons go by. The cursor
    // is on the first group after a scan.
    QCOMPARE(w.window->currentGroup(), 0);
    w.window->ruleBox()->setCurrentIndex(0);
    QCOMPARE(w.window->rule().kind, Kind::First);
    w.button(QStringLiteral("In this group"))->click();
    QCOMPARE(w.window->checkedPaths(),
             QStringList({w.at("b/photo.bin"), w.at("c/photo copy.bin")}));
    w.button(QStringLiteral("In all groups"))->click();
    QCOMPARE(w.window->checkedPaths(),
             QStringList({w.at("b/photo.bin"), w.at("c/photo copy.bin"), w.at("b/song.bin")}));

    // A folder rule takes its folder from the window.
    w.window->ruleBox()->setCurrentIndex(4);
    QCOMPARE(w.window->rule().kind, Kind::NotInFolder);
    QVERIFY(!w.window->rule().folder.isEmpty());
}

void TestDuplicates::windowKeepsOneCopyOfEachGroup()
{
    Window w;
    QVERIFY(w.ready && w.scan());

    // By hand: the last copy of a group can't be marked.
    QVERIFY(w.window->setChecked(0, 0, true));
    QVERIFY(w.window->setChecked(0, 1, true));
    QVERIFY(!w.window->setChecked(0, 2, true));
    QCOMPARE(w.window->checkedPaths(), QStringList({w.at("a/photo.bin"), w.at("b/photo.bin")}));
    QVERIFY(w.window->setChecked(1, 0, true));
    QVERIFY(!w.window->setChecked(1, 1, true));

    // Marking everything again changes nothing...
    w.window->mark(df::Rule{df::Rule::Kind::NotInFolder, w.root.path()}, -1);
    QCOMPARE(w.window->checkedPaths(), QStringList({w.at("a/photo.bin"), w.at("b/photo.bin"), w.at("a/song.bin")}));

    // ... and neither does the box in the list, which is what a click goes through:
    // the model itself refuses.
    auto *model = const_cast<QAbstractItemModel *>(w.row(0, 2).model());
    QVERIFY(model != nullptr);
    QVERIFY(!model->setData(w.row(0, 2), Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(w.row(0, 2).data(Qt::CheckStateRole).value<Qt::CheckState>(), Qt::Unchecked);
    w.click(w.row(0, 2));
    QCOMPARE(w.row(0, 2).data(Qt::CheckStateRole).value<Qt::CheckState>(), Qt::Unchecked);
    // The box of a marked file unmarks it, and then the one before can be marked.
    w.click(w.row(0, 1));
    QCOMPARE(w.window->checkedPaths(), QStringList({w.at("a/photo.bin"), w.at("a/song.bin")}));
    w.click(w.row(0, 2));
    QCOMPARE(w.window->checkedPaths(),
             QStringList({w.at("a/photo.bin"), w.at("c/photo copy.bin"), w.at("a/song.bin")}));
    // Clicking where the name is selects the row, and doesn't mark the file.
    w.click(w.row(0, 1).siblingAtColumn(1));
    QCOMPARE(w.window->checkedPaths(),
             QStringList({w.at("a/photo.bin"), w.at("c/photo copy.bin"), w.at("a/song.bin")}));
    QVERIFY(w.window->list()->selectionModel()->isSelected(w.row(0, 1)));
}

void TestDuplicates::windowCollapsesAGroup()
{
    Window w;
    QVERIFY(w.ready && w.scan());
    QCOMPARE(w.window->list()->model()->rowCount(), 7);

    // The row of a group hides its files and shows them again.
    w.click(w.row(0, -1));
    QCOMPARE(w.window->list()->model()->rowCount(), 4);
    QCOMPARE(w.window->groups().size(), 2);  // only hidden, not gone
    // Nothing is marked in this test, so nothing can be deleted.
    QVERIFY(!w.button(QStringLiteral("Delete for good"))->isEnabled());
    w.click(w.row(0, -1));
    QCOMPARE(w.window->list()->model()->rowCount(), 7);

    // A double-click on it shows what the click before hid, and ends there.
    const QModelIndex group = w.row(1, -1);
    QTest::mouseDClick(w.window->list()->viewport(), Qt::LeftButton, Qt::NoModifier,
                       w.window->list()->visualRect(group).center());
    QTest::qWait(50);
    // The press of the double-click hid the files, the double-click itself
    // showed them again: the same rows as before.
    QCOMPARE(w.window->list()->model()->rowCount(), 7);

    // What is marked stays marked while the files are hidden.
    w.window->mark(df::Rule{df::Rule::Kind::First}, 0);
    const int marked = w.window->checkedPaths().size();
    w.click(w.row(0, -1));
    QCOMPARE(w.window->checkedPaths().size(), marked);
    w.click(w.row(0, -1));
    QCOMPARE(w.window->checkedPaths().size(), marked);
}

void TestDuplicates::windowRemovesOnlyWhatWasAskedFor()
{
    Window w;
    QVERIFY(w.ready && w.scan());
    w.window->mark(df::Rule{df::Rule::Kind::First}, -1);
    const QStringList marked = {w.at("b/photo.bin"), w.at("c/photo copy.bin"), w.at("b/song.bin")};
    QCOMPARE(w.window->checkedPaths(), marked);

    // No: nothing happens, and nothing is asked twice.
    w.dialogs.answer = Dialogs::Answer::No;
    w.window->removeChecked(false);
    QCOMPARE(w.dialogs.questions.size(), 1);
    QVERIFY(w.dialogs.questions.last().contains(QStringLiteral("3 files")));
    QVERIFY(w.dialogs.questions.last().contains(QStringLiteral("can't be undone")));
    QCOMPARE(w.window->checkedPaths(), marked);
    for (const QString &path : marked) {
        QVERIFY(QFileInfo::exists(path));
    }

    // Cancel: the same.
    w.dialogs.answer = Dialogs::Answer::Cancel;
    w.window->removeChecked(false);
    QCOMPARE(w.dialogs.questions.size(), 2);
    QCOMPARE(w.window->checkedPaths(), marked);

    // Yes: the marked files, and only those. What stays is the first of each group.
    w.dialogs.answer = Dialogs::Answer::Yes;
    w.button(QStringLiteral("Delete for good"))->click();
    QVERIFY(!w.button(QStringLiteral("Delete for good"))->isEnabled());
    QVERIFY(w.settle(&DuplicatesWindow::isRemoving));
    QCOMPARE(w.dialogs.questions.size(), 3);
    for (const QString &path : marked) {
        QVERIFY2(!QFileInfo::exists(path), qPrintable(path));
    }
    for (const char *stays : {"a/photo.bin", "a/song.bin", "a/unique.bin"}) {
        QVERIFY2(QFileInfo::exists(w.at(stays)), stays);
    }

    // A group with one file left is no group, so the list is empty and says how it went.
    QVERIFY(w.window->groups().isEmpty());
    QCOMPARE(w.window->checkedPaths(), QStringList());
    QCOMPARE(w.window->totalText(), QStringLiteral("No files are marked."));
    // Two photos of 4000 bytes and a song of 2500.
    QVERIFY(w.window->statusText().contains(
            QStringLiteral("Deleted 3 files (%1). 6 files checked: no duplicates found.").arg(humanSize(10500))));
    QVERIFY(w.dialogs.told.isEmpty());  // nothing went wrong
    QVERIFY(w.button(QStringLiteral("Scan"))->isEnabled());
}

void TestDuplicates::windowAsksBeforeMovingToTheTrash()
{
    Window w;
    QVERIFY(w.ready && w.scan());
    w.window->mark(df::Rule{df::Rule::Kind::First}, 0);
    const QStringList marked = {w.at("b/photo.bin"), w.at("c/photo copy.bin")};
    QCOMPARE(w.window->checkedPaths(), marked);

    // Only ever answered with No here: to answer Yes puts the files in the real Trash.
    w.dialogs.answer = Dialogs::Answer::No;
    w.button(QStringLiteral("Move to Trash"))->click();
    QCOMPARE(w.dialogs.questions.size(), 1);
    QVERIFY(w.dialogs.questions.last().contains(QStringLiteral("Trash")));
    for (const QString &path : marked) {
        QVERIFY(QFileInfo::exists(path));
    }
    QCOMPARE(w.window->checkedPaths(), marked);
}

void TestDuplicates::windowKeepsAFileThatChangedSinceTheScan()
{
    Window w;
    QVERIFY(w.ready && w.scan());
    w.window->mark(df::Rule{df::Rule::Kind::First}, 0);
    const QStringList marked = {w.at("b/photo.bin"), w.at("c/photo copy.bin")};

    // One of the marked files is written over after the scan: it is no longer
    // known to be a copy of the others, so it stays.
    writeFile(marked.last(), content(9, 3000));
    w.dialogs.answer = Dialogs::Answer::Yes;
    w.window->removeChecked(false);
    QVERIFY(w.settle(&DuplicatesWindow::isRemoving));

    QVERIFY(!QFileInfo::exists(marked.first()));
    QVERIFY(QFileInfo::exists(marked.last()));
    QVERIFY(QFileInfo::exists(w.at("a/photo.bin")));
    // The changed file is still in the list, but not marked any more. What
    // is left: the photo group without the copy that was deleted, and the songs.
    QCOMPARE(w.window->groups().size(), 2);
    QCOMPARE(paths(w.window->groups().first()),
             QStringList({w.at("a/photo.bin"), w.at("c/photo copy.bin")}));
    QVERIFY(w.window->checkedPaths().isEmpty());
    QVERIFY(w.window->statusText().contains(QStringLiteral("Deleted 1 file")));
    QVERIFY(w.window->statusText().contains(QStringLiteral("1 file stayed: it changed since the scan.")));
    QVERIFY(w.dialogs.told.isEmpty());
}

void TestDuplicates::windowLeavesAGroupWhoseOtherCopiesAreGone()
{
    Window w;
    QVERIFY(w.ready && w.scan());
    w.window->mark(df::Rule{df::Rule::Kind::First}, 0);
    const QStringList marked = {w.at("b/photo.bin"), w.at("c/photo copy.bin")};

    // The copy that was to stay is deleted by something else in the meantime:
    // the marked ones are all there is of it now, so they stay too.
    QVERIFY(QFile::remove(w.at("a/photo.bin")));
    w.dialogs.answer = Dialogs::Answer::Yes;
    w.window->removeChecked(false);
    QVERIFY(w.settle(&DuplicatesWindow::isRemoving));

    for (const QString &path : marked) {
        QVERIFY(QFileInfo::exists(path));
    }
    // Still marked, as the user meant to remove them; a new scan sorts it out.
    QCOMPARE(w.window->checkedPaths(), marked);
    QVERIFY(w.window->statusText().contains(
            QStringLiteral("1 group stayed as it was: none of its other copies is still there.")));
}

void TestDuplicates::windowRemembersItsOptions()
{
    Window w;
    QVERIFY(w.ready);
    df::Options options;
    options.folders = {w.a, w.b};
    options.pattern = "*.bin";
    options.subfolders = false;
    options.minSize = 3000;
    options.includeEmpty = true;
    w.window->setOptions(options);
    QCOMPARE(w.window->options().folders, options.folders);
    QCOMPARE(w.window->options().pattern, options.pattern);
    QCOMPARE(w.window->options().subfolders, false);
    // The field is in whole KB, and a floor of 3000 bytes needs 3 KB of it.
    QCOMPARE(w.window->options().minSize, qint64(3) * 1024);
    QCOMPARE(w.window->options().includeEmpty, true);

    // The minimum size is in kilobytes, and can't be asked beyond what the field holds.
    options.minSize = qint64(5000) * 1024 * 1024;
    w.window->setOptions(options);
    QCOMPARE(w.window->options().minSize, qint64(1024) * 1024 * 1024);

    // No pattern is every file, and is shown as such.
    options.pattern = "  ";
    w.window->setOptions(options);
    QCOMPARE(w.window->options().pattern, QStringLiteral("*"));

    // A folder is on the list once, however often and wherever it is given.
    w.window->addFolder(w.a);
    w.window->addFolder(w.a + "/");
    w.window->addFolder(w.a + "/./");
    w.window->addFolder(w.at("a/photo.bin"));  // a file stands for its folder
    w.window->addFolder(w.root.path() + "/nowhere");
    QCOMPARE(w.window->options().folders, QStringList({w.a, w.b}));
    w.window->removeFolder(w.a + "/");
    QCOMPARE(w.window->options().folders, QStringList({w.b}));
    QVERIFY(w.scan());  // what the list says is what is scanned: only b
    QCOMPARE(w.window->groups().size(), 0);
}

void TestDuplicates::windowStaysResponsiveDuringAScan()
{
    QTemporaryDir big;
    const QString folder = bigTree(big, 300);
    ScriptedDialogs dialogs;
    DuplicatesWindow window(dialogs);
    window.addFolder(folder);

    int ticks = 0;
    QTimer timer;
    timer.setInterval(1);
    connect(&timer, &QTimer::timeout, [&] {
        if (window.isScanning()) {
            ++ticks;
        }
    });
    timer.start();

    window.startScan();
    QVERIFY(window.isScanning());
    QVERIFY(window.statusText().contains(QStringLiteral("Looking for files")));
    QVERIFY(QTest::qWaitFor([&] { return !window.isScanning(); }, 15000));

    QCOMPARE(window.groups().size(), 300);
    // The scan ran to its end, and all the while the GUI thread kept processing timers.
    QVERIFY2(ticks > 0, qPrintable(QStringLiteral("%1 timer ticks while scanning").arg(ticks)));
}

void TestDuplicates::windowStopsAScan()
{
    QTemporaryDir big;
    const QString folder = bigTree(big, 300);
    ScriptedDialogs dialogs;
    DuplicatesWindow window(dialogs);
    window.addFolder(folder);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    window.startScan();
    QVERIFY(window.isScanning());
    window.stopScan();
    QVERIFY(QTest::qWaitFor([&] { return !window.isScanning(); }, 15000));
    QVERIFY(window.statusText().contains(QStringLiteral("Stopped early")));
    QVERIFY(window.groups().size() < 300);
    QVERIFY(!window.isScanning());
}

QTEST_MAIN(TestDuplicates)
#include "duplicates.moc"
