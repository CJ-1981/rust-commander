#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <cstring>
#include <memory>

#include "panels/FileModel.h"
#include "panels/FileProxyModel.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

bool writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

bool makeFolder(const QString &path)
{
    return QDir().mkpath(path);
}

void setModified(const QString &path, const QDateTime &when)
{
    QFile file(path);
    if (file.open(QIODevice::ReadWrite)) {
        file.setFileTime(when, QFileDevice::FileModificationTime);
    }
}

// Unix hides names that start with a dot; Windows needs the attribute.
void setHiddenAttribute(const QString &path)
{
#ifdef Q_OS_WIN
    const std::wstring native = path.toStdWString();
    SetFileAttributesW(native.c_str(), FILE_ATTRIBUTE_HIDDEN);
#else
    Q_UNUSED(path);
#endif
}

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

// Waits until the listing of `model` has arrived; false on timeout.
bool waitListed(FileModel &model)
{
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 15000;
    while (model.isLoading() && QDateTime::currentMSecsSinceEpoch() < deadline) {
        QTest::qWait(10);
    }
    return !model.isLoading();
}

// Names in row order. macOS may hand names back in a different Unicode form
// than they were created with, so they are normalised before comparing.
QStringList names(const QAbstractItemModel &model)
{
    QStringList result;
    for (int row = 0; row < model.rowCount(); ++row) {
        result.append(model.index(row, FileModel::NameColumn).data().toString()
                          .normalized(QString::NormalizationForm_C));
    }
    return result;
}

int rowOfName(const QAbstractItemModel &model, const QString &name)
{
    return names(model).indexOf(name);
}

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
};

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
    QCOMPARE(mime->formats(), QStringList({"text/uri-list"}));
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

QTEST_MAIN(TestFileModel)
#include "filemodel.moc"
