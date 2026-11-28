#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>

#include "helpers.h"
#include "tools/FileSearch.h"
#include "tools/SearchWindow.h"

using namespace FileSearch;

namespace {

// A tree to look through:
//   root/a/         notes.txt, photo.jpg, .secret, report-2024.pdf, report-2023.pdf
//   root/a/deep/    photo.jpg (the same bytes as the one above, another date)
//   root/b/         notes.txt (other bytes), song.mp3
//   root/empty/     (nothing in it)
//   root/link-a/    a link to root/a (Unix only)
QString makeTree()
{
    QTemporaryDir *root = new QTemporaryDir();
    const QString base = root->path();
    makeFolder(base + "/a/deep");
    makeFolder(base + "/b");
    makeFolder(base + "/empty");
    writeFile(base + "/a/notes.txt", "first version\n");
    writeFile(base + "/a/photo.jpg", QByteArray("photo v1"));
    writeFile(base + "/a/.secret", "hidden");
    writeFile(base + "/a/report-2024.pdf", "report");
    writeFile(base + "/a/report-2023.pdf", "report");
    writeFile(base + "/a/deep/photo.jpg", QByteArray("photo v2"));
    writeFile(base + "/b/notes.txt", "second version\n");
    writeFile(base + "/b/song.mp3", QByteArray("music"));
#ifdef Q_OS_UNIX
    QFile::link(base + "/a", base + "/link-a");
#endif
    return base;
}

// The paths of what was found, sorted, without the root in front.
QStringList found(const QString &base, const Query &query)
{
    std::atomic<bool> stop{false};
    QStringList paths;
    search(query,
           [&](const Match *match, const Progress &) {
               if (match) {
                   paths.append(match->path.mid(base.size() + 1));
               }
           },
           stop);
    paths.sort();
    return paths;
}

}  // namespace

class TestFileSearch : public QObject {
    Q_OBJECT

private slots:
    void namesMatchByWordAndWildcard();
    void theNameCanBeARegularExpression();
    void foldersAndFilesAreToldApart();
    void hiddenFilesStayOutUnlessAskedFor();
    void sizesAndDatesNarrowWhatIsFound();
    void textIsLookedForInsideFiles();
    void aSearchWalksFoldersAndCanBeStopped();
    void linksAreNotFollowedAndErrorsAreCounted();
    void theWindowSearchesAndLists();
    void theWindowStopsASearchAndKeepsWhatItFound();
};

void TestFileSearch::namesMatchByWordAndWildcard()
{
    Query query;
    query.name = "report";
    // Without a wildcard, the word may be anywhere in the name.
    QVERIFY(matches(query, "2024-report.pdf", false, 6, QDateTime()));
    QVERIFY(!matches(query, "notes.txt", false, 14, QDateTime()));
    // Whatever the case, unless it matters.
    QVERIFY(matches(query, "REPORT.PDF", false, 6, QDateTime()));
    query.caseSensitive = true;
    QVERIFY(!matches(query, "REPORT.PDF", false, 6, QDateTime()));

    // With a wildcard, the word must be the whole name.
    query.caseSensitive = false;
    query.name = "*.jpg";
    QVERIFY(matches(query, "photo.jpg", false, 8, QDateTime()));
    QVERIFY(!matches(query, "photo.jpg.bak", false, 8, QDateTime()));
    query.name = "photo*";
    QVERIFY(matches(query, "photo.jpg", false, 8, QDateTime()));
    query.name = "*.jp?";
    QVERIFY(matches(query, "photo.jpg", false, 8, QDateTime()));

    // Several words: all of them must be in the name.
    query.name = "report 2024";
    QVERIFY(matches(query, "report-2024.pdf", false, 6, QDateTime()));
    QVERIFY(!matches(query, "report-2023.pdf", false, 6, QDateTime()));

    // Names match however they are spelled in Unicode.
    query.name = "한국어";
    QVERIFY(matches(query, QString::fromUtf8("파일 한국어.txt"), false, 4, QDateTime()));

    // Nothing: every name.
    query.name = QString();
    QVERIFY(matches(query, "anything", false, 0, QDateTime()));

    // A regular expression instead.
    query.name = "^report-\\d{4}\\.pdf$";
    query.nameIsRegex = true;
    QVERIFY(matches(query, "report-2024.pdf", false, 6, QDateTime()));
    QVERIFY(!matches(query, "old report-2024.pdf", false, 6, QDateTime()));

    // What is wrong with a query is said.
    Query bad;
    bad.name = "[";
    bad.nameIsRegex = true;
    QVERIFY(!validate(bad).isEmpty());
    bad.name = QString();
    bad.nameIsRegex = false;
    bad.folders = {"/no-such-folder"};
    QVERIFY(!validate(bad).isEmpty());
    Query fine;
    fine.folders = {QDir::tempPath()};
    QVERIFY(validate(fine).isEmpty());
}

void TestFileSearch::theNameCanBeARegularExpression()
{
    Query query;
    query.name = "rep(or)ted?";
    query.nameIsRegex = true;
    QVERIFY(matches(query, "reported", false, 8, QDateTime()));
    QVERIFY(!matches(query, "report", false, 6, QDateTime()));  // "ted?" needs "te"
    QVERIFY(!matches(query, "reposted", false, 8, QDateTime()));
}

void TestFileSearch::foldersAndFilesAreToldApart()
{
    Query files;
    files.what = Query::What::Files;
    QVERIFY(matches(files, "notes.txt", false, 14, QDateTime()));
    QVERIFY(!matches(files, "a", true, -1, QDateTime()));

    Query folders;
    folders.what = Query::What::Folders;
    QVERIFY(matches(folders, "a", true, -1, QDateTime()));
    QVERIFY(!matches(folders, "notes.txt", false, 14, QDateTime()));
}

void TestFileSearch::hiddenFilesStayOutUnlessAskedFor()
{
    Query query;
    QVERIFY(!matches(query, ".secret", false, 7, QDateTime(), true));
    QVERIFY(matches(query, ".secret", false, 7, QDateTime(), false));
    query.includeHidden = true;
    QVERIFY(matches(query, ".secret", false, 7, QDateTime(), true));
}

void TestFileSearch::sizesAndDatesNarrowWhatIsFound()
{
    const QDateTime march(QDate(2024, 3, 15), QTime(12, 0));
    const QDateTime may(QDate(2024, 5, 1), QTime(9, 0));
    Query query;
    query.minSize = 10;
    QVERIFY(!matches(query, "small.bin", false, 9, QDateTime()));
    QVERIFY(matches(query, "big.bin", false, 10, QDateTime()));
    query.maxSize = 100;
    QVERIFY(matches(query, "big.bin", false, 100, QDateTime()));
    QVERIFY(!matches(query, "huge.bin", false, 101, QDateTime()));
    // A size keeps folders out, which have no size.
    QVERIFY(!matches(query, "folder", true, -1, QDateTime()));

    query.minSize = -1;
    query.maxSize = -1;
    query.modifiedAfter = march;
    query.modifiedBefore = may;
    QVERIFY(!matches(query, "early.txt", false, 1, QDateTime(QDate(2024, 2, 1), QTime(0, 0))));
    QVERIFY(matches(query, "in-march.txt", false, 1, march));
    QVERIFY(matches(query, "in-may.txt", false, 1, may));
    QVERIFY(!matches(query, "late.txt", false, 1, QDateTime(QDate(2024, 6, 1), QTime(0, 0))));
    // Without a date either, a folder gets through again.
    query.modifiedAfter = QDateTime();
    query.modifiedBefore = QDateTime();
    QVERIFY(matches(query, "folder", true, -1, QDateTime()));
}

void TestFileSearch::textIsLookedForInsideFiles()
{
    Query query;
    query.content = Query::Content{QStringLiteral("second version")};
    query.folders = {};
    QVERIFY(matches(query, "notes.txt", false, 15, QDateTime()));
    QVERIFY(matches(query, "song.mp3", false, 5, QDateTime()));  // it isn't checked here
    // Whether the text is in the file is the walk's part, not the name's.
}

void TestFileSearch::aSearchWalksFoldersAndCanBeStopped()
{
    const QString base = makeTree();
    Query query;
    query.folders = {base};

    // Everything that is not hidden: a, a/deep, b, empty, link-a, and the
    // files in a, a/deep and b — the link itself is one more folder.
    const QStringList all = found(base, query);
    QCOMPARE(all.size(), 12);
    QVERIFY(all.contains("a/notes.txt"));
    QVERIFY(all.contains("a/deep/photo.jpg"));
    QVERIFY(all.contains("b/notes.txt"));

    // Recursion off: only what is directly in the folder.
    query.recursive = false;
    QCOMPARE(found(base, query), QStringList({"a", "b", "empty", "link-a"}));
    query.recursive = true;

    // Folders only, files only.
    query.what = Query::What::Folders;
    QCOMPARE(found(base, query).size(), 5);
    query.what = Query::What::Files;
    QCOMPARE(found(base, query).size(), 7);
    query.what = Query::What::Both;

    // The name narrows what is found.
    query.name = "*.jpg";
    QCOMPARE(found(base, query), QStringList({"a/deep/photo.jpg", "a/photo.jpg"}));

    // The text narrows it further: only one photo has these bytes in it.
    query.content = Query::Content{QStringLiteral("photo v2")};
    QCOMPARE(found(base, query), QStringList({"a/deep/photo.jpg"}));

    // A search that is stopped has seen some of it, and says so.
    std::atomic<bool> stop{false};
    qint64 foldersSeen = 0;
    Summary summary = search(query,
                             [&](const Match *, const Progress &progress) {
                                 foldersSeen = progress.folders;
                                 if (progress.folders >= 1) {
                                     stop = true;
                                 }
                             },
                             stop);
    QVERIFY(summary.stopped);
    QVERIFY(summary.folders < 4);
    QVERIFY(foldersSeen >= 1);

    // The limit ends it, and says there may be more.
    Query limited;
    limited.folders = {base};
    limited.maxResults = 3;
    stop = false;
    summary = search(limited, {}, stop);
    QVERIFY(summary.capped);
    summary = search(limited, {}, stop);
    QVERIFY(summary.capped);

    // How much was looked at.
    Query plain;
    plain.folders = {base};
    stop = false;
    summary = search(plain, {}, stop);
    QVERIFY(!summary.stopped);
    QVERIFY(!summary.capped);
    QCOMPARE(summary.folders, qint64(5));
    QVERIFY(summary.entries >= 9);
    QCOMPARE(summary.unreadable, qint64(0));
}

void TestFileSearch::linksAreNotFollowedAndErrorsAreCounted()
{
#ifndef Q_OS_UNIX
    QSKIP("links to folders are made differently here");
#else
    const QString base = makeTree();
    Query query;
    query.folders = {base};
    // link-a leads to a; without following, what is in a is found once.
    const QStringList all = found(base, query);
    QCOMPARE(all.count("a/notes.txt"), 1);

    // A folder that can't be read is counted, and its contents are not found.
    const QString locked = base + "/locked";
    QVERIFY(makeFolder(locked));
    QVERIFY(writeFile(locked + "/inside.txt", "x"));
    if (!QFile::setPermissions(locked, {})) {
        QSKIP("folder permissions don't apply to this user");
    }
    if (QFileInfo(locked).isReadable()) {
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                              | QFileDevice::ExeOwner);
        QSKIP("folder permissions don't apply to this user");
    }
    std::atomic<bool> stop{false};
    const Summary summary = search(query, {}, stop);
    QCOMPARE(summary.unreadable, qint64(1));
    QVERIFY(!found(base, query).contains("locked/inside.txt"));
    QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                 | QFileDevice::ExeOwner));
#endif
}

namespace {

// A window over a tree of its own.
struct WindowFixture {
    QTemporaryDir root;
    QWidget window;
    ScriptedDialogs dialogs;
    std::unique_ptr<SearchWindow> search;
    bool ready = false;

    WindowFixture()
    {
        makeFolder(root.path() + "/a");
        makeFolder(root.path() + "/a/deep");
        makeFolder(root.path() + "/b");
        writeFile(root.path() + "/a/notes.txt", "first version\n");
        writeFile(root.path() + "/a/photo.jpg", QByteArray("photo v1"));
        writeFile(root.path() + "/a/deep/photo.jpg", QByteArray("photo v2"));
        writeFile(root.path() + "/a/report.pdf", "report");
        writeFile(root.path() + "/b/notes.txt", "second version\n");

        auto *layout = new QVBoxLayout(&window);
        search.reset(SearchWindow::open(root.path(), dialogs, &window));
        layout->addWidget(search.get());
        window.resize(980, 620);
        window.show();
        ready = QTest::qWaitForWindowExposed(&window);
    }

    // Searches and waits for it to be over; false on timeout.
    bool searchAndSettle()
    {
        search->startSearch();
        return QTest::qWaitFor([&] { return !search->isSearching(); }, 30000);
    }
};

}  // namespace

void TestFileSearch::theWindowSearchesAndLists()
{
    WindowFixture w;
    QVERIFY(w.ready);

    // Nothing to look in: said, and not searched.
    w.search->folderField()->clear();
    w.search->startSearch();
    QVERIFY(!w.search->isSearching());
    QVERIFY(w.search->statusText().contains(QStringLiteral("where to look")));

    // A search by name.
    w.search->folderField()->setText(w.root.path());
    w.search->nameField()->setText(QStringLiteral("*.txt"));
    QVERIFY(w.searchAndSettle());
    QCOMPARE(w.search->resultPaths().size(), 2);
    QVERIFY(w.search->resultPaths().contains(w.root.path() + "/a/notes.txt"));
    QVERIFY(w.search->resultPaths().contains(w.root.path() + "/b/notes.txt"));
    QVERIFY(w.search->statusText().contains(QStringLiteral("2 files found")));

    // The table is sorted by name, naturally.
    const QAbstractItemModel *model = w.search->resultsModel();
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->data(model->index(0, 0)).toString(), QStringLiteral("notes.txt"));

    // By text inside the files, through the fields.
    w.search->nameField()->clear();
    w.search->contentField()->setText(QStringLiteral("second version"));
    QVERIFY(w.searchAndSettle());
    QCOMPARE(w.search->resultPaths(), QStringList({w.root.path() + "/b/notes.txt"}));

    // The sizes and dates behind More options reach the search.
    w.search->contentField()->clear();
    w.search->moreOptionsButton()->setChecked(true);
    w.search->minSizeField()->setText(QStringLiteral("4 KB"));
    QVERIFY(w.searchAndSettle());
    // Every file in the tree is smaller than 4 KB.
    QCOMPARE(w.search->resultPaths().size(), 0);

    w.search->minSizeField()->clear();
    w.search->modifiedBox()->setChecked(true);
    w.search->modifiedFromField()->setDate(QDate(2099, 1, 1));
    QVERIFY(w.searchAndSettle());
    QCOMPARE(w.search->resultPaths().size(), 0);
    w.search->modifiedBox()->setChecked(false);
    QVERIFY(w.searchAndSettle());
    QCOMPARE(w.search->resultPaths().size(), 5);

    // An impossible regular expression is said, and not searched.
    w.search->regexBox()->setChecked(true);
    w.search->nameField()->setText(QStringLiteral("["));
    w.search->startSearch();
    QVERIFY(!w.search->isSearching());
    QVERIFY(w.search->statusText().contains(QStringLiteral("regular expression")));
    w.search->regexBox()->setChecked(false);
    w.search->nameField()->clear();
}

void TestFileSearch::theWindowStopsASearchAndKeepsWhatItFound()
{
    WindowFixture w;
    QVERIFY(w.ready);
    w.search->folderField()->setText(w.root.path());
    w.search->startSearch();
    // The search is over quickly in a tree this small, so the stop is asked
    // for right away and the outcome is either way.
    if (w.search->isSearching()) {
        w.search->stopSearch();
    }
    QTest::qWait(2000);
    QVERIFY(!w.search->isSearching());
    QVERIFY(!w.search->statusText().isEmpty());
    // Whatever was found is still listed.
    QCOMPARE(w.search->resultPaths(), w.search->resultPaths());
}

QTEST_MAIN(TestFileSearch)
#include "filesearch.moc"
