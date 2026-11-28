#include <QCryptographicHash>
#include <QFile>
#include <QTableWidget>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QTest>
#include <QTimer>

#include <memory>

#include "helpers.h"
#include "tools/ChecksumWindow.h"

using namespace Checksums;

namespace {

// Known answers, from the specifications and from the tools that write these
// files. The text "abc", hashed by each algorithm.
const QByteArray kAbc("abc");
const QHash<QString, QString> kAbcHashes = {
    {QStringLiteral("sfv"), QStringLiteral("352441c2")},
    {QStringLiteral("md5"), QStringLiteral("900150983cd24fb0d6963f7d28e17f72")},
    {QStringLiteral("sha1"), QStringLiteral("a9993e364706816aba3e25717850c26c9cd0d89d")},
    {QStringLiteral("sha256"), QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")},
    {QStringLiteral("sha512"),
     QStringLiteral("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f")},
};

// Writes a file with `content` and returns its path.
QString makeFile(const QTemporaryDir &root, const QString &name, const QByteArray &content)
{
    const QString path = root.path() + QLatin1Char('/') + name;
    if (!writeFile(path, content)) {
        qWarning().noquote() << QStringLiteral("could not write %1").arg(path);
        return QString();
    }
    return path;
}

// Waits until the window has hashed everything; false on timeout.
bool settle(ChecksumWindow *window)
{
    return QTest::qWaitFor([&] { return !window->isHashing(); }, 15000);
}

// A text with `entries` as sha256sum writes it: hash, two spaces, name.
QString sumText(const QList<QPair<QString, QString>> &entries)
{
    QString text;
    for (const auto &entry : entries) {
        text += entry.first + QStringLiteral("  ") + entry.second + QLatin1Char('\n');
    }
    return text;
}

}  // namespace

class TestChecksum : public QObject {
    Q_OBJECT

private slots:
    void hashesMatchTheSpecifications();
    void aBigFileIsHashedInPieces();
    void whatIsTypedOrPastedCountsAsAHash();
    void readsWhatTheToolsWrite();
    void itemsAreFoundWhereTheChecksumFileSays();
    void checkingSaysOkDifferentAndMissing();
    void aChecksumFileIsWrittenAndReadBack();
    void theWindowHashesAndCompares();
    void theWindowChecksAChecksumFile();
    void theWindowStaysUsableWhileItHashes();
};

void TestChecksum::hashesMatchTheSpecifications()
{
    QTemporaryDir root;
    const QString path = makeFile(root, "abc.txt", kAbc);

    QString error;
    for (int algorithm = int(Algorithm::Crc32); algorithm <= int(Algorithm::Sha512); ++algorithm) {
        const Algorithm kind = Algorithm(algorithm);
        QString hash;
        QVERIFY(hashFile(path, kind, &hash, &error));
        QCOMPARE(hash, kAbcHashes.value(fileSuffix(kind)));
    }

    // The same file twice, the same answer.
    QString again;
    QVERIFY(hashFile(path, Algorithm::Sha256, &again, &error));
    QCOMPARE(again, kAbcHashes.value("sha256"));

    // A file that isn't there is an error, and no half answer.
    QString hash;
    QVERIFY(!hashFile(root.path() + "/no-such-file", Algorithm::Sha256, &hash, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(hash.isEmpty());

    // What the algorithms are called, and what their hashes look like.
    QCOMPARE(algorithmName(Algorithm::Sha256), QStringLiteral("SHA-256"));
    QCOMPARE(fileSuffix(Algorithm::Md5), QStringLiteral("md5"));
    QCOMPARE(fileSuffix(Algorithm::Crc32), QStringLiteral("sfv"));
    QCOMPARE(hashLength(Algorithm::Md5), 32);
    QCOMPARE(hashLength(Algorithm::Sha512), 128);
    QCOMPARE(algorithmForLength(40).value_or(Algorithm::Crc32), Algorithm::Sha1);
    QVERIFY(!algorithmForLength(41).has_value());
    QCOMPARE(algorithmFromName("sha256sum").value_or(Algorithm::Crc32), Algorithm::Sha256);
    QCOMPARE(algorithmFromName("SHA2-512").value_or(Algorithm::Crc32), Algorithm::Sha512);
    QCOMPARE(algorithmFromName("sfv").value_or(Algorithm::Sha256), Algorithm::Crc32);
    QVERIFY(!algorithmFromName("nope").has_value());
}

void TestChecksum::aBigFileIsHashedInPieces()
{
    QTemporaryDir root;
    // More than one chunk of 1 MiB: the reading has to come in pieces.
    QByteArray content(3 * 1024 * 1024 + 12345, Qt::Uninitialized);
    for (int i = 0; i < content.size(); ++i) {
        content[i] = char((i * 31 + (i >> 13)) & 0xff);
    }
    const QString path = makeFile(root, "big.bin", content);

    // The same as hashing it in one go, which is what QCryptographicHash does.
    QString hash;
    QString error;
    QVERIFY(hashFile(path, Algorithm::Sha256, &hash, &error));
    QCryptographicHash whole(QCryptographicHash::Sha256);
    whole.addData(content);
    QCOMPARE(hash, QString::fromLatin1(whole.result().toHex()));

    // Reported in pieces as well, in order, ending with the whole size.
    class Recording : public Progress {
    public:
        bool bytes(qint64 done, qint64 total) override
        {
            reports << qMakePair(done, total);
            return true;
        }
        bool checked(const Check &, int, int) override { return true; }

        QList<QPair<qint64, qint64>> reports;
    } progress;
    QVERIFY(hashFile(path, Algorithm::Sha256, &hash, &error, &progress));
    QVERIFY(progress.reports.size() > 2);
    qint64 last = -1;
    for (const auto &report : progress.reports) {
        QVERIFY(report.first >= last);  // never goes back
        last = report.first;
        QCOMPARE(report.second, qint64(content.size()));
    }
    QCOMPARE(progress.reports.last().first, qint64(content.size()));

    // Stopped in the middle: no answer, and no error either.
    class Stopper : public Progress {
    public:
        bool bytes(qint64, qint64) override { return ++ticks < 2; }
        bool checked(const Check &, int, int) override { return true; }

        int ticks = 0;
    } stopper;
    QVERIFY(!hashFile(path, Algorithm::Sha256, &hash, &error, &stopper));
    QVERIFY(error.isEmpty());
    QVERIFY(hash.isEmpty());
}

void TestChecksum::whatIsTypedOrPastedCountsAsAHash()
{
    // In and around it, and lines as tools print them.
    QCOMPARE(hashIn(QStringLiteral("  BA7816BF 8F01CFEA 4141 40DE5DAE2223 ")),
             QStringLiteral("ba7816bf8f01cfea414140de5dae2223"));
    QCOMPARE(hashIn(QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc.txt")),
             QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    QCOMPARE(hashIn(QStringLiteral("SHA256 (abc.txt) = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")),
             QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    // A name with hex digits in it is not taken for the hash.
    QCOMPARE(hashIn(QStringLiteral("ba7816bf  deadbeef.txt")), QStringLiteral("ba7816bf"));
    QVERIFY(hashIn(QStringLiteral("not a hash at all")).isEmpty());
    QVERIFY(hashIn(QString()).isEmpty());

    // Comparing: the same, of the same kind but another, or no hash at all.
    const QString sha256 = kAbcHashes.value("sha256");
    QCOMPARE(compare(sha256, sha256.toUpper()), Comparison::Match);
    QCOMPARE(compare(sha256, QStringLiteral("  ") + sha256 + QStringLiteral("  ")), Comparison::Match);
    QCOMPARE(compare(sha256, sha256.left(63) + (sha256.endsWith('d') ? 'e' : 'd')), Comparison::Different);
    QCOMPARE(compare(sha256, QStringLiteral("hello")), Comparison::NotHash);
    QCOMPARE(compare(sha256, QString()), Comparison::NotHash);
    // A hash of another algorithm is not "different": it isn't one of this kind.
    QCOMPARE(compare(sha256, kAbcHashes.value("md5")), Comparison::NotHash);
}

void TestChecksum::readsWhatTheToolsWrite()
{
    // sha256sum: hash, two spaces, name. A "*" says binary; comments and
    // blank lines are left out.
    const QString sha256 = kAbcHashes.value("sha256");
    const QString md5 = kAbcHashes.value("md5");
    ChecksumFile file = parse(QStringLiteral(
            "; made by hand\n"
            "# as well\n"
            "\n"
            "%1  abc.txt\n"
            "%1 *abc2.txt\n"
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  empty.txt\n").arg(sha256));
    QCOMPARE(file.entries.size(), 3);
    QCOMPARE(file.unread, 0);
    QCOMPARE(file.algorithm.value_or(Algorithm::Crc32), Algorithm::Sha256);
    QCOMPARE(file.entries.at(0).name, QStringLiteral("abc.txt"));
    QCOMPARE(file.entries.at(0).hash, sha256);
    QCOMPARE(file.entries.at(1).name, QStringLiteral("abc2.txt"));
    QCOMPARE(file.entries.at(2).hash,
             QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

    // Windows line endings, a Windows path in the name, ./ in front.
    file = parse(QStringLiteral("%1  ./folder\\abc.txt\r\n").arg(md5));
    QCOMPARE(file.entries.size(), 1);
    QCOMPARE(file.algorithm.value_or(Algorithm::Crc32), Algorithm::Md5);
    QCOMPARE(file.entries.at(0).name, QStringLiteral("folder\\abc.txt"));

    // What the macOS tools print: MD5 (name) = hash.
    file = parse(QStringLiteral("MD5 (abc.txt) = %1\n").arg(md5));
    QCOMPARE(file.entries.size(), 1);
    QCOMPARE(file.entries.at(0).name, QStringLiteral("abc.txt"));
    QCOMPARE(file.entries.at(0).hash, md5);

    // An SFV file: name, spaces, CRC in capitals. All its hashes are 8 long,
    // so no one algorithm is told from them.
    file = parse(QStringLiteral("abc.txt 352441C2\nabc2.txt 352441C2\n"));
    QCOMPARE(file.entries.size(), 2);
    // CRC32 is the one algorithm whose hashes are 8 digits long.
    QCOMPARE(file.algorithm.value_or(Algorithm::Sha256), Algorithm::Crc32);
    QCOMPARE(file.entries.at(0).hash, QStringLiteral("352441c2"));

    // A line that is neither a comment nor a checksum is counted, left out.
    file = parse(QStringLiteral("%1  abc.txt\nsomething else entirely\n").arg(sha256));
    QCOMPARE(file.entries.size(), 1);
    QCOMPARE(file.unread, 1);

    // Mixed hashes: no one algorithm.
    file = parse(QStringLiteral("%1  a.txt\n%2  b.txt\n").arg(sha256, md5));
    QVERIFY(!file.algorithm.has_value());

    // Nothing that can be read as a list of checksums.
    file = parse(QStringLiteral("just some text\nand more of it\n"));
    QVERIFY(file.entries.isEmpty());
    QCOMPARE(file.unread, 2);

    // Read from a file, and what a checksum file is called.
    QTemporaryDir root;
    const QString path = makeFile(root, "check.sha256", sumText({{sha256, "abc.txt"}}).toUtf8());
    QVERIFY(isChecksumFile(path));
    QString error;
    ChecksumFile read_back;
    QVERIFY(read(path, &read_back, &error));
    QCOMPARE(read_back.entries.size(), 1);
    QCOMPARE(read_back.entries.at(0).name, QStringLiteral("abc.txt"));
    QVERIFY(read_back.algorithm.value_or(Algorithm::Crc32) == Algorithm::Sha256);
    QVERIFY(!read(root.path() + "/no-such-file.sha256", &read_back, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!isChecksumFile(root.path() + "/no-such-file.sha256"));
    // A file whose name says nothing is still one when it reads as a list.
    const QString unnamed = makeFile(root, "checklist", sumText({{sha256, "abc.txt"}}).toUtf8());
    QVERIFY(isChecksumFile(unnamed));
    QVERIFY(!isChecksumFile(makeFile(root, "photo.jpg", QByteArray("not a checksum"))));
}

void TestChecksum::itemsAreFoundWhereTheChecksumFileSays()
{
    QTemporaryDir root;
    makeFile(root, "abc.txt", kAbc);
    makeFolder(root.path() + "/folder/deep");
    makeFile(root, "folder/deep/abc2.txt", kAbc);
    ChecksumFile file = parse(QStringLiteral(
            "%1  abc.txt\n"
            "%1  ./folder/deep/abc2.txt\n"
            "%1  folder\\deep\\from-windows.txt\n"
            "%1  no-such.txt\n").arg(kAbcHashes.value("sha256")));

    const QList<Item> items = itemsIn(root.path(), file.entries);
    QCOMPARE(items.size(), 4);
    QCOMPARE(items.at(0).name, QStringLiteral("abc.txt"));
    QCOMPARE(items.at(0).path, root.path() + "/abc.txt");
    QCOMPARE(items.at(1).path, root.path() + "/folder/deep/abc2.txt");
    // The name is as it was written; the file is looked for with slashes.
    QCOMPARE(items.at(2).name, QStringLiteral("folder\\deep\\from-windows.txt"));
    QCOMPARE(items.at(2).path, root.path() + "/folder/deep/from-windows.txt");
    QCOMPARE(items.at(3).path, root.path() + "/no-such.txt");
}

void TestChecksum::checkingSaysOkDifferentAndMissing()
{
    QTemporaryDir root;
    const QString sha256 = kAbcHashes.value("sha256");
    const QString ok = makeFile(root, "ok.txt", kAbc);
    const QString different = makeFile(root, "different.txt", QByteArray("not abc"));
    makeFile(root, "empty.txt", QByteArray());

    const QList<Item> items = {
        {QStringLiteral("ok.txt"), ok, Algorithm::Sha256, sha256},
        {QStringLiteral("different.txt"), different, Algorithm::Sha256, sha256},
        {QStringLiteral("empty.txt"), root.path() + "/empty.txt", Algorithm::Sha256,
         QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
        {QStringLiteral("missing.txt"), root.path() + "/missing.txt", Algorithm::Sha256, sha256},
        {QStringLiteral("just-hashed.txt"), ok, Algorithm::Sha256, QString()},
    };
    QList<Check> checks;
    QVERIFY(check(items, &checks));
    QCOMPARE(checks.size(), items.size());

    QCOMPARE(checks.at(0).status, Status::Ok);
    QCOMPARE(checks.at(0).actual, sha256);
    QCOMPARE(checks.at(1).status, Status::Different);
    QVERIFY(!checks.at(1).actual.isEmpty());
    QCOMPARE(checks.at(2).status, Status::Ok);  // the hash of nothing
    QCOMPARE(checks.at(3).status, Status::Missing);
    QCOMPARE(checks.at(4).status, Status::Ok);  // hashed, nothing to compare with

    QCOMPARE(summary(checks), QStringLiteral("3 OK, 1 different, 1 missing"));

    // A check that is stopped leaves the rest alone.
    class StopAfter : public Progress {
    public:
        bool bytes(qint64, qint64) override { return true; }
        bool checked(const Check &, int done, int) override { return done < 2; }
    } progress;
    checks.clear();
    QVERIFY(!check(items, &checks, &progress));
    QCOMPARE(checks.size(), items.size());
    QCOMPARE(checks.at(0).status, Status::Ok);
    QCOMPARE(checks.at(1).status, Status::Different);
    QCOMPARE(checks.at(2).status, Status::NotChecked);
    QCOMPARE(summary(checks), QStringLiteral("1 OK, 1 different, 3 not checked"));
}

void TestChecksum::aChecksumFileIsWrittenAndReadBack()
{
    QTemporaryDir root;
    const QList<Entry> entries = {
        {QStringLiteral("abc.txt"), kAbcHashes.value("sha256"), Algorithm::Sha256},
        {QStringLiteral("empty.txt"),
         QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), Algorithm::Sha256},
    };
    const QString path = root.path() + "/check.sha256";
    QString error;
    QVERIFY(write(path, entries, &error));

    ChecksumFile file;
    QVERIFY(read(path, &file, &error));
    QCOMPARE(file.entries.size(), entries.size());
    QCOMPARE(file.entries.at(0).name, QStringLiteral("abc.txt"));
    QCOMPARE(file.entries.at(0).hash, entries.at(0).hash);
    QVERIFY(file.algorithm.value_or(Algorithm::Crc32) == Algorithm::Sha256);

    // The text is what sha256sum writes: one line per file, each ended.
    QFile written(path);
    QVERIFY(written.open(QIODevice::ReadOnly));
    const QByteArray bytes = written.readAll();
    QVERIFY(bytes.endsWith('\n'));
    QVERIFY(bytes.contains("  abc.txt\n"));

    // A name with a line break in it is refused rather than written as two.
    QVERIFY(!write(path, {{QStringLiteral("a\nb.txt"), entries.at(0).hash, Algorithm::Sha256}}, &error));
    QVERIFY(!error.isEmpty());

    // An SFV file, when the entries are all CRC32.
    const QList<Entry> crc = {{QStringLiteral("abc.txt"), QStringLiteral("352441c2"), Algorithm::Crc32}};
    const QString sfv = root.path() + "/check.sfv";
    QVERIFY(write(sfv, crc, &error));
    QFile sfvFile(sfv);
    QVERIFY(sfvFile.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(sfvFile.readAll()), QStringLiteral("abc.txt 352441C2\n"));
}

void TestChecksum::theWindowHashesAndCompares()
{
    QTemporaryDir root;
    ScriptedDialogs dialogs;
    const QString ok = makeFile(root, "ok.txt", kAbc);
    ChecksumWindow window(dialogs);

    window.setFiles({ok});
    QVERIFY(settle(&window));
    QCOMPARE(window.files(), QStringList({ok}));
    QCOMPARE(window.hash(), kAbcHashes.value("sha256"));
    QVERIFY(!window.isHashing());

    // Nothing to compare with yet.
    QCOMPARE(window.verdict(), ChecksumWindow::Verdict::Nothing);

    // The same hash, typed and ended with Enter: it matches.
    QTest::keyClicks(window.expectedField(), kAbcHashes.value("sha256").toUpper());
    QTest::keyClick(window.expectedField(), Qt::Key_Return);
    QCOMPARE(window.verdict(), ChecksumWindow::Verdict::Match);
    QVERIFY(window.verdictLabel()->text().contains(QStringLiteral("atch")));

    // Another hash of the same kind: it does not.
    // A hash of another kind: the window computes that kind, so it matches.
    window.expectedField()->setText(kAbcHashes.value("md5"));
    QVERIFY(settle(&window));
    QCOMPARE(window.algorithm(), Algorithm::Md5);
    QCOMPARE(window.verdict(), ChecksumWindow::Verdict::Match);

    // A hash of the same kind that isn't the file's: it does not match.
    window.setAlgorithm(Algorithm::Sha256);
    QVERIFY(settle(&window));
    window.expectedField()->setText(kAbcHashes.value("sha256").left(63) + QLatin1Char('e'));
    QTest::keyClick(window.expectedField(), Qt::Key_Return);
    QVERIFY(settle(&window));
    QCOMPARE(window.verdict(), ChecksumWindow::Verdict::Different);
    // What is no hash at all asks for one again, instead of saying "no".
    window.expectedField()->setText(QStringLiteral("hello"));
    QTest::keyClick(window.expectedField(), Qt::Key_Return);
    QVERIFY(settle(&window));
    QCOMPARE(window.verdict(), ChecksumWindow::Verdict::Nothing);

    // Another algorithm: the same file, another hash.
    window.setAlgorithm(Algorithm::Md5);
    QVERIFY(settle(&window));
    QCOMPARE(window.algorithm(), Algorithm::Md5);
    QCOMPARE(window.hash(), kAbcHashes.value("md5"));
    // Several files, one after the other: the selected row's hash is shown.
    const QString two = makeFile(root, "two.txt", QByteArray("2"));
    window.setFiles({ok, two});
    QVERIFY(settle(&window));
    QCOMPARE(window.files().size(), 2);
    QVERIFY(!window.hash().isEmpty());
    QCOMPARE(window.table()->rowCount(), 2);
}

void TestChecksum::theWindowChecksAChecksumFile()
{
    QTemporaryDir root;
    ScriptedDialogs dialogs;
    const QString sha256 = kAbcHashes.value("sha256");
    makeFile(root, "ok.txt", kAbc);
    makeFile(root, "different.txt", QByteArray("not abc"));
    const QString list = makeFile(root, "check.sha256",
                                  sumText({{sha256, "ok.txt"},
                                           {sha256, "different.txt"},
                                           {sha256, "missing.txt"}}).toUtf8());

    const std::unique_ptr<ChecksumWindow> window(ChecksumWindow::open(list, dialogs));
    QCOMPARE(window->checksumFile(), list);
    QCOMPARE(window->folder(), root.path());
    QVERIFY(settle(window.get()));

    const QList<Check> checks = window->checks();
    QCOMPARE(checks.size(), 3);
    QCOMPARE(checks.at(0).status, Status::Ok);
    QCOMPARE(checks.at(1).status, Status::Different);
    QCOMPARE(checks.at(2).status, Status::Missing);
    QCOMPARE(window->table()->rowCount(), 3);
    QVERIFY(window->statusLabel()->text().contains(QStringLiteral("1 different")));
    QVERIFY(window->statusLabel()->text().contains(QStringLiteral("1 missing")));

    // Another folder: the check starts over.
    window->setFolder(root.path());
    QVERIFY(settle(window.get()));
    QCOMPARE(window->checks().size(), 3);
}

void TestChecksum::theWindowStaysUsableWhileItHashes()
{
    QTemporaryDir root;
    ScriptedDialogs dialogs;
    // Big enough that the hashing takes more than a moment.
    QByteArray content(150 * 1024 * 1024, Qt::Uninitialized);
    content[0] = 'x';
    const QString big = makeFile(root, "big.bin", content);
    ChecksumWindow window(dialogs);
    window.setFiles({big});
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.isHashing());

    // A timer keeps firing, and the window takes clicks meanwhile.
    int beats = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, [&] { ++beats; });
    heartbeat.start(5);
    QTest::qWait(300);
    qInfo() << "beats in 300 ms:" << beats << "| still hashing:" << window.isHashing();
    // A blocked GUI thread would not have let the timer fire at all.
    QVERIFY(beats > 2);

    // Stopped before the end: no answer of its own, and the hash winds down
    // without holding the loop up. How fast a loaded runner serves timers is
    // its own business; the counting above is what says the window answers.
    window.stop();
    QVERIFY(settle(&window));
    heartbeat.stop();
}

QTEST_MAIN(TestChecksum)
#include "checksum.moc"
