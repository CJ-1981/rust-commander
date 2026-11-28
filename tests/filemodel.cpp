#include <QLocale>
#include <QMimeData>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <memory>

#include "helpers.h"
#include "panels/FileProxyModel.h"

namespace {

// A folder with a few files and subfolders, as the models' source.
struct Sample {
    explicit Sample(const QString &root)
        : dir(root + "/sample")
    {
        makeFolder(dir + "/alpha");
        makeFolder(dir + "/zebra");
        writeFile(dir + "/a.md", "abc");
        writeFile(dir + "/file2.txt", "ab");
        writeFile(dir + "/file10.txt", "0123456789");
        writeFile(dir + "/한국어.txt", "한국어");
    }

    QString dir;
};

}  // namespace

class TestFileModel : public QObject {
    Q_OBJECT

private slots:
    void listsFoldersFirstInNaturalOrder();
    void reportsMissingFolders();
    void reportsUnreadableFolder();
    void newestListingWins();
    void mimeDataListsEachFileOnce();
    void hidesHiddenEntries();
    void filtersByWords();
    void sortsBySizeAndDate();
    void computesFolderSizes();
    void sizesStayThroughANavigationAndGoOnChange();

    // ---- added by the reviewer ----
    void reviewListingIsNotHeldUpByFolderSizes();
};

// Folder sizes are being worked out for folders with many files; the user
// goes somewhere else meanwhile. The listing of the new folder must not wait
// for the measurements.
void TestFileModel::reviewListingIsNotHeldUpByFolderSizes()
{
    QTemporaryDir root;
    const QString dir = root.path();
    for (const QString &name : {"big1", "big2", "big3", "big4"}) {
        for (int i = 0; i < 12; ++i) {
            const QString sub = dir + "/" + name + "/" + QString::number(i);
            QVERIFY(makeFolder(sub));
            for (int j = 0; j < 250; ++j) {
                writeFile(sub + "/" + QString::number(j) + ".txt", "x");
            }
        }
    }
    QVERIFY(makeFolder(dir + "/small"));
    QVERIFY(writeFile(dir + "/small/one.txt", "1"));
    const auto sizeOfRow = [&](const FileModel &m, const QString &name) -> qint64 {
        const int row = m.rowOf(dir + "/" + name);
        return m.index(row, FileModel::SizeColumn).data(FileModel::FolderSizeRole).toLongLong();
    };

    // How long one measurement takes, for comparison.
    qint64 one = 0;
    {
        FileModel probe;
        probe.setPath(dir);
        QVERIFY(waitListed(probe));
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        probe.requestFolderSizes();
        QTRY_VERIFY_WITH_TIMEOUT(sizeOfRow(probe, "big1") > 0, 60000);
        one = QDateTime::currentMSecsSinceEpoch() - started;
    }

    FileModel model;
    // Without any measuring going on: the listing takes next to no time.
    qint64 alone = 0;
    for (int round = 0; round < 3; ++round) {
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        model.setPath(dir + "/small");
        QVERIFY(waitListed(model));
        alone = qMax(alone, QDateTime::currentMSecsSinceEpoch() - t0);
    }

    model.setPath(dir);
    QVERIFY(waitListed(model));
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    model.requestFolderSizes();
    model.setPath(dir + "/small");
    QVERIFY(waitListed(model));
    const qint64 listedAfter = QDateTime::currentMSecsSinceEpoch() - started;
    qInfo() << "one measurement takes about" << one << "ms; the same listing alone takes" << alone
            << "ms, with sizes being worked out:" << listedAfter << "ms";
    QCOMPARE(names(model), QStringList({"one.txt"}));
    QVERIFY2(listedAfter < alone + qMax<qint64>(40, one / 2),
             "the listing waited behind the folder sizes");
}

void TestFileModel::listsFoldersFirstInNaturalOrder()
{
    QTemporaryDir root;
    Sample sample(root.path());
    FileModel model;
    QSignalSpy started(&model, &FileModel::listingStarted);
    QSignalSpy finished(&model, &FileModel::listingFinished);
    QSignalSpy failed(&model, &FileModel::listingFailed);

    model.setPath(sample.dir);
    QCOMPARE(model.path(), sample.dir);
    QVERIFY(waitListed(model));

    QVERIFY(model.error().isEmpty());
    QCOMPARE(model.rowCount(), 6);
    QCOMPARE(model.columnCount(), FileModel::ColumnCount);
    QCOMPARE(names(model),
             QStringList({"alpha", "zebra", "a.md", "file2.txt", "file10.txt", "한국어.txt"}));
    QCOMPARE(started.count(), 1);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(failed.count(), 0);

    const int file10 = model.rowOf(sample.dir + "/file10.txt");
    QVERIFY(file10 >= 0);
    const FileEntry *entry = model.entry(file10);
    QCOMPARE(entry->size, 10);
    QCOMPARE(entry->suffix, QString("txt"));
    QVERIFY(!entry->isDir);
    QVERIFY(entry->modified.isValid());
    QCOMPARE(model.index(file10, FileModel::ModifiedColumn).data().toString(),
             QLocale::system().toString(entry->modified, QStringLiteral("yyyy-MM-dd HH:mm")));
    QVERIFY(model.index(file10, FileModel::ModifiedColumn).data().toString().startsWith("20"));
    QCOMPARE(model.index(file10, FileModel::SizeColumn).data().toString(), humanSize(10));
    QVERIFY(!model.index(file10, FileModel::TypeColumn).data().toString().isEmpty());
    QCOMPARE(model.index(file10, FileModel::NameColumn).data(FileModel::PathRole).toString(),
             sample.dir + "/file10.txt");
    QCOMPARE(model.index(file10, FileModel::SizeColumn).data(FileModel::SortRole).toLongLong(), 10);

    const int alpha = model.rowOf(sample.dir + "/alpha");
    QVERIFY(alpha >= 0);
    QVERIFY(model.entry(alpha)->isDir);
    QCOMPARE(model.index(alpha, FileModel::NameColumn).data(FileModel::IsDirRole).toBool(), true);
    // Not measured yet, so the size column stays empty rather than guessing.
    QVERIFY(model.index(alpha, FileModel::SizeColumn).data().toString().isEmpty());
    QVERIFY(model.flags(model.index(alpha, FileModel::NameColumn)).testFlag(Qt::ItemIsDropEnabled));
    QVERIFY(!model.flags(model.index(file10, FileModel::NameColumn)).testFlag(Qt::ItemIsDropEnabled));

    // Out of range is answered safely.
    QVERIFY(model.entry(-1) == nullptr);
    QVERIFY(model.entry(6) == nullptr);
    QCOMPARE(model.rowOf(sample.dir + "/nope.txt"), -1);
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);
    QVERIFY(model.flags(QModelIndex()) == Qt::NoItemFlags);
    QVERIFY(!model.data(QModelIndex()).isValid());
}

void TestFileModel::reportsMissingFolders()
{
    QTemporaryDir root;
    Sample sample(root.path());
    FileModel model;
    QSignalSpy finished(&model, &FileModel::listingFinished);
    QSignalSpy failed(&model, &FileModel::listingFailed);

    model.setPath(sample.dir + "/missing");
    QVERIFY(waitListed(model));
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(!model.error().isEmpty());
    QCOMPARE(failed.count(), 1);
    QCOMPARE(finished.count(), 0);
    QCOMPARE(failed.at(0).at(0).toString(), sample.dir + "/missing");

    // A file is not a folder either.
    model.setPath(sample.dir + "/a.md");
    QVERIFY(waitListed(model));
    QVERIFY(!model.error().isEmpty());
    QCOMPARE(failed.count(), 2);

    // The error doesn't stick: a good folder afterwards lists normally.
    model.setPath(sample.dir);
    QVERIFY(waitListed(model));
    QVERIFY(model.error().isEmpty());
    QCOMPARE(model.rowCount(), 6);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(failed.count(), 2);
}

void TestFileModel::reportsUnreadableFolder()
{
#ifndef Q_OS_UNIX
    QSKIP("folder permissions work differently on this platform");
#else
    // A folder that can't be read is an error, not an empty listing.
    QTemporaryDir root;
    const QString locked = root.path() + "/locked";
    QVERIFY(makeFolder(locked));
    QVERIFY(writeFile(locked + "/inside.txt", "x"));
    QVERIFY(QFile::setPermissions(locked, {}));
    if (QFileInfo(locked).isReadable()) {
        // Root, as in some CI containers, reads it anyway.
        QSKIP("folder permissions don't apply to this user");
    }

    FileModel model;
    QSignalSpy finished(&model, &FileModel::listingFinished);
    QSignalSpy failed(&model, &FileModel::listingFailed);
    model.setPath(locked);
    QVERIFY(waitListed(model));
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(!model.error().isEmpty());
    QCOMPARE(failed.count(), 1);
    QCOMPARE(finished.count(), 0);

    // Restore, so that the temporary folder can be cleaned up.
    QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                 | QFileDevice::ExeOwner));
#endif
}

void TestFileModel::newestListingWins()
{
    QTemporaryDir root;
    QVERIFY(makeFolder(root.path() + "/a"));
    QVERIFY(makeFolder(root.path() + "/b"));
    QVERIFY(writeFile(root.path() + "/a/only-in-a.txt", "a"));
    QVERIFY(writeFile(root.path() + "/b/only-in-b.txt", "b"));
    const QString a = root.path() + "/a";
    const QString b = root.path() + "/b";

    FileModel model;
    model.setPath(a);
    model.setPath(b);
    QVERIFY(waitListed(model));
    // Give the superseded listing time to arrive as well.
    QTest::qWait(300);

    QCOMPARE(model.path(), b);
    QCOMPARE(names(model), QStringList({"only-in-b.txt"}));
    QCOMPARE(model.rowOf(a + "/only-in-a.txt"), -1);

    // Reload picks up changes.
    QVERIFY(writeFile(b + "/added.txt", "x"));
    model.reload();
    QVERIFY(waitListed(model));
    QCOMPARE(model.rowCount(), 2);
}

void TestFileModel::mimeDataListsEachFileOnce()
{
    QTemporaryDir root;
    Sample sample(root.path());
    FileModel model;
    model.setPath(sample.dir);
    QVERIFY(waitListed(model));

    // A selection has an index per column.
    const int file2 = model.rowOf(sample.dir + "/file2.txt");
    const int file10 = model.rowOf(sample.dir + "/file10.txt");
    QVERIFY(file2 >= 0 && file10 >= 0);
    QModelIndexList indexes;
    for (int column = 0; column < FileModel::ColumnCount; ++column) {
        indexes.append(model.index(file2, column));
        indexes.append(model.index(file10, column));
    }
    const std::unique_ptr<QMimeData> mime(model.mimeData(indexes));
    QVERIFY(mime);
    QCOMPARE(mime->formats(), QStringList({"text/uri-list", dragSourceFormat}));
    // The panes' own drags say where they come from.
    QCOMPARE(mime->data(dragSourceFormat), sample.dir.toUtf8());
    const QList<QUrl> urls = mime->urls();
    QCOMPARE(urls.size(), 2);
    QCOMPARE(urls.at(0).toLocalFile(), sample.dir + "/file2.txt");
    QCOMPARE(urls.at(1).toLocalFile(), sample.dir + "/file10.txt");
}

void TestFileModel::hidesHiddenEntries()
{
    QTemporaryDir root;
    QVERIFY(makeFolder(root.path() + "/.cache"));
    setHiddenAttribute(root.path() + "/.cache");
    QVERIFY(writeFile(root.path() + "/.secret.txt", "s"));
    setHiddenAttribute(root.path() + "/.secret.txt");
    QVERIFY(writeFile(root.path() + "/visible.txt", "v"));

    FileModel model;
    model.setPath(root.path());
    QVERIFY(waitListed(model));
    QCOMPARE(model.rowCount(), 3);
    const int secret = model.rowOf(root.path() + "/.secret.txt");
    QVERIFY(secret >= 0);
    QVERIFY(model.entry(secret)->isHidden);

    FileProxyModel proxy;
    proxy.setSourceModel(&model);
    QVERIFY(!proxy.showHidden());
    QCOMPARE(names(proxy), QStringList({"visible.txt"}));

    proxy.setShowHidden(true);
    QCOMPARE(names(proxy), QStringList({".cache", ".secret.txt", "visible.txt"}));

    proxy.setShowHidden(false);
    QCOMPARE(names(proxy), QStringList({"visible.txt"}));
}

void TestFileModel::filtersByWords()
{
    QTemporaryDir root;
    QVERIFY(writeFile(root.path() + "/notes.txt", "n"));
    QVERIFY(writeFile(root.path() + "/NOTES.md", "n"));
    QVERIFY(writeFile(root.path() + "/photo.jpg", "p"));
    QVERIFY(writeFile(root.path() + "/readme.txt", "r"));

    FileModel model;
    model.setPath(root.path());
    QVERIFY(waitListed(model));
    FileProxyModel proxy;
    proxy.setSourceModel(&model);
    QCOMPARE(proxy.rowCount(), 4);
    QVERIFY(proxy.filterText().isEmpty());

    proxy.setFilterText("txt");
    QCOMPARE(names(proxy), QStringList({"notes.txt", "readme.txt"}));  // case-insensitive

    proxy.setFilterText("*.md");
    QCOMPARE(names(proxy), QStringList({"NOTES.md"}));

    proxy.setFilterText("  o   s ");  // every word must match; extra spaces don't matter
    QCOMPARE(names(proxy), QStringList({"NOTES.md", "notes.txt"}));

    proxy.setFilterText("zzz");
    QCOMPARE(proxy.rowCount(), 0);
    QCOMPARE(model.rowCount(), 4);  // the source keeps everything

    proxy.setFilterText(QString());
    QCOMPARE(names(proxy), QStringList({"NOTES.md", "notes.txt", "photo.jpg", "readme.txt"}));

    // An entry that arrives later passes through the same filter.
    proxy.setFilterText("ph");
    QVERIFY(writeFile(root.path() + "/phase.txt", "p"));
    model.reload();
    QVERIFY(waitListed(model));
    QCOMPARE(names(proxy), QStringList({"phase.txt", "photo.jpg"}));
    QCOMPARE(proxy.entry(proxy.index(0, 0))->name, QString("phase.txt"));
    QVERIFY(proxy.entry(QModelIndex()) == nullptr);
}

void TestFileModel::sortsBySizeAndDate()
{
    QTemporaryDir root;
    QVERIFY(makeFolder(root.path() + "/big"));
    QVERIFY(makeFolder(root.path() + "/small"));
    QVERIFY(writeFile(root.path() + "/a.bin", QByteArray(300, 'a')));
    QVERIFY(writeFile(root.path() + "/b.bin", QByteArray(100, 'b')));
    QVERIFY(writeFile(root.path() + "/c.bin", QByteArray(200, 'c')));

    FileModel model;
    model.setPath(root.path());
    QVERIFY(waitListed(model));
    FileProxyModel proxy;
    proxy.setSourceModel(&model);
    QCOMPARE(proxy.sortColumn(), FileModel::NameColumn);
    QCOMPARE(proxy.sortOrder(), Qt::AscendingOrder);

    proxy.sort(FileModel::SizeColumn, Qt::AscendingOrder);
    QCOMPARE(names(proxy), QStringList({"big", "small", "b.bin", "c.bin", "a.bin"}));

    // Descending keeps the folders on top, like Total Commander, and sorts
    // them descending too.
    proxy.sort(FileModel::SizeColumn, Qt::DescendingOrder);
    QCOMPARE(names(proxy), QStringList({"small", "big", "a.bin", "c.bin", "b.bin"}));

    proxy.sort(FileModel::NameColumn, Qt::DescendingOrder);
    QCOMPARE(names(proxy), QStringList({"small", "big", "c.bin", "b.bin", "a.bin"}));

    const QString dir = root.path() + "/dates";
    QVERIFY(makeFolder(dir));
    QVERIFY(writeFile(dir + "/old.txt", "o"));
    QVERIFY(writeFile(dir + "/mid.txt", "m"));
    QVERIFY(writeFile(dir + "/new.txt", "n"));
    setModified(dir + "/old.txt", QDateTime(QDate(2020, 1, 1), QTime(12, 0)));
    setModified(dir + "/mid.txt", QDateTime(QDate(2022, 3, 3), QTime(12, 0)));
    setModified(dir + "/new.txt", QDateTime(QDate(2024, 6, 15), QTime(12, 0)));
    model.setPath(dir);
    QVERIFY(waitListed(model));

    proxy.sort(FileModel::ModifiedColumn, Qt::AscendingOrder);
    QCOMPARE(names(proxy), QStringList({"old.txt", "mid.txt", "new.txt"}));
    proxy.sort(FileModel::ModifiedColumn, Qt::DescendingOrder);
    QCOMPARE(names(proxy), QStringList({"new.txt", "mid.txt", "old.txt"}));
    QCOMPARE(model.rowCount(), 3);
}

void TestFileModel::computesFolderSizes()
{
    QTemporaryDir root;
    const QString dir = root.path();
    QVERIFY(makeFolder(dir + "/sub/nested"));
    QVERIFY(makeFolder(dir + "/sub/empty"));
    QVERIFY(writeFile(dir + "/sub/one.bin", QByteArray(100, '1')));
    QVERIFY(writeFile(dir + "/sub/two.bin", QByteArray(50, '2')));
    QVERIFY(writeFile(dir + "/sub/nested/three.bin", QByteArray(50, '3')));
    QVERIFY(writeFile(dir + "/loose.txt", "x"));

    FileModel model;
    model.setPath(dir);
    QVERIFY(waitListed(model));
    FileProxyModel proxy;
    proxy.setSourceModel(&model);

    const int sub = rowOfName(proxy, "sub");
    QVERIFY(sub >= 0);
    const QModelIndex sizeIndex = proxy.index(sub, FileModel::SizeColumn);
    QVERIFY(sizeIndex.data().toString().isEmpty());

    // The second call must not start a duplicate measurement.
    model.requestFolderSizes();
    model.requestFolderSizes();
    QTRY_COMPARE_WITH_TIMEOUT(sizeIndex.data().toString(), humanSize(200), 15000);
    // 100 + 50 + 50 bytes in 3 files; the empty folder adds nothing.
    QCOMPARE(sizeIndex.data(FileModel::FolderSizeRole).toLongLong(), 200);
    QCOMPARE(proxy.index(sub, FileModel::NameColumn).data(Qt::ToolTipRole).toString().right(7),
             QString("3 items"));
    QCOMPARE(proxy.index(rowOfName(proxy, "loose.txt"), FileModel::SizeColumn).data().toString(),
             humanSize(1));
}

void TestFileModel::sizesStayThroughANavigationAndGoOnChange()
{
    // A size that was computed once stays computed: going away and coming
    // back shows it at once, without measuring again. It is the folder that
    // changed, as when the watcher says so, that makes the sizes forgotten.
    QTemporaryDir root;
    const QString dir = root.path();
    QVERIFY(makeFolder(dir + "/sub/inner"));
    QVERIFY(writeFile(dir + "/sub/one.bin", QByteArray(100, '1')));

    FileModel model;
    model.setPath(dir);
    QVERIFY(waitListed(model));
    FileProxyModel proxy;
    proxy.setSourceModel(&model);
    const int sub = rowOfName(proxy, "sub");
    const QModelIndex sizeIndex = proxy.index(sub, FileModel::SizeColumn);

    model.requestFolderSizes();
    QTRY_COMPARE_WITH_TIMEOUT(sizeIndex.data(FileModel::FolderSizeRole).toLongLong(), 100, 15000);

    // Away and back: the size is there without waiting for a measurement.
    model.setPath(root.path() + "/sub/inner");
    QVERIFY(waitListed(model));
    model.setPath(dir);
    QVERIFY(waitListed(model));
    const int subAgain = rowOfName(proxy, "sub");
    QCOMPARE(proxy.index(subAgain, FileModel::SizeColumn).data(FileModel::FolderSizeRole)
                 .toLongLong(),
             100);

    // The folder changed (what reload() is for): the size is not trusted.
    model.reload();
    QVERIFY(waitListed(model));
    const int subFresh = rowOfName(proxy, "sub");
    QVERIFY(subFresh >= 0);
    QCOMPARE(proxy.index(subFresh, FileModel::SizeColumn).data(FileModel::FolderSizeRole)
                 .toLongLong(),
             -1);
}

QTEST_MAIN(TestFileModel)
#include "filemodel.moc"
