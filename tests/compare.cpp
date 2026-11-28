#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>

#include "helpers.h"
#include "tools/CompareWindow.h"
#include "tools/LineDiff.h"

using namespace LineDiff;

namespace {

QByteArray text(const char *content)
{
    return QByteArray(content);
}

// Writes a file with `content` and returns its path.
QString makeFile(const QString &folder, const QString &name, const char *content)
{
    const QString path = folder + QLatin1Char('/') + name;
    if (!writeFile(path, text(content))) {
        QWARN(qPrintable(QStringLiteral("could not write %1").arg(path)));
    }
    return path;
}

}  // namespace

class TestCompare : public QObject {
    Q_OBJECT

private slots:
    // The algorithm, on its own.
    void identicalFilesAreAllEqual();
    void linesAddedRemovedAndChanged();
    void endsOfFileDifferences();
    void oneFileEmpty();
    void whitespaceAndLineEndings();
    void koreanAndUnicode();
    void bigFilesAreComparedQuickly();

    // The window.
    void theWindowComparesTwoFiles();
    void nextAndPreviousWalkTheDifferences();
    void aBinaryFileIsRefused();
    void theOptionsChangeTheComparison();
    void swappingSidesSwapsTheRows();
};

void TestCompare::identicalFilesAreAllEqual()
{
    const Options options;
    const Text left = split(text("one\ntwo\nthree\n"), options);
    const Text right = split(text("one\ntwo\nthree\n"), options);
    const Result result = compare(left, right, options);
    QCOMPARE(result.rows.size(), 3);
    QCOMPARE(result.same, 3);
    QCOMPARE(result.removed, 0);
    QCOMPARE(result.added, 0);
    for (const Row &row : result.rows) {
        QCOMPARE(row.kind, Kind::Equal);
        QVERIFY(!row.left.isEmpty());
        QCOMPARE(row.left, row.right);
        QCOMPARE(row.leftNumber, row.rightNumber);
    }
    // The numbers count from 1.
    QCOMPARE(result.rows.first().leftNumber, 1);
    QCOMPARE(result.rows.last().leftNumber, 3);
}

void TestCompare::linesAddedRemovedAndChanged()
{
    const Options options;
    const Text left = split(text("kept\nold one\nalso old\ngone\ntail\n"), options);
    const Text right = split(text("kept\nnew one\ntail\nextra\n"), options);
    const Result result = compare(left, right, options);

    // The same lines are in both, in order.
    QStringList leftLines;
    QStringList rightLines;
    for (const Row &row : result.rows) {
        if (!row.left.isEmpty() || row.leftNumber > 0) {
            if (row.kind != Kind::Removed) {
                leftLines.append(row.left);
            }
        }
        if (row.kind != Kind::Added) {
            rightLines.append(row.right);
        }
    }
    // The rows walk both texts from the start to the end without going back.
    int leftNumber = 0;
    int rightNumber = 0;
    for (const Row &row : result.rows) {
        QVERIFY(row.leftNumber > leftNumber || row.leftNumber == 0);
        QVERIFY(row.rightNumber > rightNumber || row.rightNumber == 0);
        leftNumber = qMax(leftNumber, row.leftNumber);
        rightNumber = qMax(rightNumber, row.rightNumber);
    }
    QCOMPARE(leftNumber, 5);
    QCOMPARE(rightNumber, 4);
    QVERIFY(result.removed > 0);
    QVERIFY(result.added > 0);

    // The first row is the kept line.
    QCOMPARE(result.rows.first().kind, Kind::Equal);
    QCOMPARE(result.rows.first().left, QStringLiteral("kept"));

    // The changed pair has the parts that differ marked.
    const auto changed = std::find_if(result.rows.cbegin(), result.rows.cend(),
                                      [](const Row &row) { return row.kind == Kind::Changed; });
    QVERIFY(changed != result.rows.cend());
    QCOMPARE(changed->left, QStringLiteral("old one"));
    QCOMPARE(changed->right, QStringLiteral("new one"));
    QVERIFY(!changed->leftSpans.isEmpty());
    QVERIFY(!changed->rightSpans.isEmpty());
    // The span starts where "old" and "new" differ.
    QCOMPARE(changed->leftSpans.first().start, 0);
}

void TestCompare::endsOfFileDifferences()
{
    const Options options;
    // Everything of one side is gone at the end.
    const Text left = split(text("one\ntwo\nthree\n"), options);
    const Text right = split(text("one\n"), options);
    const Result result = compare(left, right, options);
    QCOMPARE(result.rows.size(), 3);
    QCOMPARE(result.rows.first().kind, Kind::Equal);
    QCOMPARE(result.rows.at(1).kind, Kind::Removed);
    QCOMPARE(result.rows.at(2).kind, Kind::Removed);
    QCOMPARE(result.rows.at(1).left, QStringLiteral("two"));
    QCOMPARE(result.rows.at(1).right, QString());
    QCOMPARE(result.rows.at(1).rightNumber, 0);
    QCOMPARE(result.added, 0);
    QCOMPARE(result.removed, 2);

    // The same at the start instead.
    const Result mirror = compare(right, left, options);
    QCOMPARE(mirror.rows.at(1).kind, Kind::Added);
    QCOMPARE(mirror.rows.at(1).leftNumber, 0);
    QCOMPARE(mirror.added, 2);
}

void TestCompare::oneFileEmpty()
{
    const Options options;
    const Text left = split(QString(), options);
    const Text right = split(text("one\ntwo\n"), options);
    const Result result = compare(left, right, options);
    QCOMPARE(result.rows.size(), 2);
    for (const Row &row : result.rows) {
        QCOMPARE(row.kind, Kind::Added);
        QCOMPARE(row.leftNumber, 0);
    }
    QCOMPARE(result.added, 2);
}

void TestCompare::whitespaceAndLineEndings()
{
    Options options;
    // Without the options, spaces and endings matter.
    options.ignoreTrailingWhitespace = false;
    options.ignoreLineEndings = false;
    const Text left = split(text("one  \ntwo\n"), options);
    const Text right = split(text("one\ntwo\n"), options);
    Result result = compare(left, right, options);
    QVERIFY(result.removed + result.added > 0);

    // With the options, they do not.
    options.ignoreTrailingWhitespace = true;
    options.ignoreLineEndings = true;
    result = compare(left, right, options);
    QCOMPARE(result.removed + result.added, 0);
    QCOMPARE(result.same, 2);

    // CRLF and LF end a line alike.
    const Text crlf = split(text("one\r\ntwo\r\n"), options);
    result = compare(crlf, right, options);
    QCOMPARE(result.removed + result.added, 0);

    // Blank lines are shown, but are no difference, when asked.
    const Text blank = split(text("one\n\ntwo\n"), options);
    result = compare(blank, right, options);
    QCOMPARE(result.rows.size(), 3);
    QVERIFY(result.removed + result.added > 0);
    options.ignoreBlankLines = true;
    result = compare(blank, right, options);
    QCOMPARE(result.rows.size(), 3);
    // The blank line is on the list, without counting as a difference.
    int ignored = 0;
    for (const Row &row : result.rows) {
        if (row.kind == Kind::Ignored) {
            ++ignored;
            QVERIFY(row.left.isEmpty());
            QCOMPARE(row.rightNumber, 0);
        }
    }
    QCOMPARE(ignored, 1);
    QCOMPARE(result.removed + result.added, 0);
}

void TestCompare::koreanAndUnicode()
{
    Options options;
    const QString hangul = QString::fromUtf8("한국어 파일.txt");
    const QString hangul2 = QString::fromUtf8("한국어 파일 2.txt");
    const Text left = split(hangul + QStringLiteral("\n") + hangul2 + QStringLiteral("\n"), options);
    const Text right = split(hangul2 + QStringLiteral("\n") + hangul + QStringLiteral("\n"), options);
    const Result result = compare(left, right, options);
    // The two lines swapped: two changed pairs, or a removal and an addition.
    QVERIFY(result.rows.size() >= 2);
    // The names come out as they were written, whole.
    const auto hasHangul = [&hangul](const Row &row) { return row.left == hangul; };
    QVERIFY(std::any_of(result.rows.cbegin(), result.rows.cend(), hasHangul));
}

void TestCompare::bigFilesAreComparedQuickly()
{
    Options options;
    // 20 000 lines, with a few differences at the end.
    QString big;
    for (int i = 0; i < 20000; ++i) {
        big += QStringLiteral("line %1 of the file\n").arg(i);
    }
    QString changed = big;
    changed += QStringLiteral("one more line\n");
    QElapsedTimer timer;
    timer.start();

    const Text left = split(big, options);
    const Text right = split(changed, options);
    const Result result = compare(left, right, options);
    QCOMPARE(result.added, 1);
    // Well under a second on any machine this decade.
    QVERIFY2(timer.elapsed() < 2000, qPrintable(QStringLiteral("%1 ms").arg(timer.elapsed())));

    // Two identical files of 50 000 lines: not quadratic, and all equal.
    QString bigger;
    for (int i = 0; i < 50000; ++i) {
        bigger += QStringLiteral("line %1\n").arg(i);
    }
    const Text left2 = split(bigger, options);
    const Text right2 = split(bigger, options);
    QElapsedTimer timer2;
    timer2.start();
    const Result same = compare(left2, right2, options);
    QCOMPARE(same.same, 50000);
    QVERIFY2(timer2.elapsed() < 2000, qPrintable(QStringLiteral("%1 ms").arg(timer2.elapsed())));
}

void TestCompare::theWindowComparesTwoFiles()
{
    QTemporaryDir root;
    ScriptedDialogs dialogs;
    const QString left = makeFile(root.path(), "left.txt", text("kept\nold one\ntail\n"));
    const QString right = makeFile(root.path(), "right.txt", text("kept\nnew one\ntail\nextra\n"));

    std::unique_ptr<CompareWindow> window(CompareWindow::open(left, right, dialogs));
    QVERIFY(window->isVisible());
    QCOMPARE(window->leftField()->text(), left);
    QCOMPARE(window->rightField()->text(), right);
    QVERIFY(window->differences() > 0);
    QVERIFY(window->rows().size() >= 4);
    QVERIFY(window->statusLabel()->text().contains(QStringLiteral("differences")));

    // The same file on both sides: nothing differs, and the buttons say so.
    window->compare(left, left);
    QCOMPARE(window->differences(), 0);
    QVERIFY(!window->nextButton()->isEnabled());
    QVERIFY(!window->previousButton()->isEnabled());
    QVERIFY(window->statusLabel()->text().contains(QStringLiteral("the same")));

    // Compared again from the fields.
    window->leftField()->setText(left);
    window->rightField()->setText(right);
    window->compareButton()->click();
    QVERIFY(window->differences() > 0);
}

void TestCompare::nextAndPreviousWalkTheDifferences()
{
    QTemporaryDir root;
    ScriptedDialogs dialogs;
    const QString left = makeFile(root.path(), "left.txt",
                                  text("kept\ndiff 1\nkept\ndiff 2\nkept\ndiff 3\nkept\n"));
    const QString right = makeFile(root.path(), "right.txt",
                                   text("kept\nfine 1\nkept\nfine 2\nkept\nfine 3\nkept\n"));
    std::unique_ptr<CompareWindow> window(CompareWindow::open(left, right, dialogs));
    QCOMPARE(window->differences(), 3);

    // From nothing, Next goes to the first difference, then through them all.
    window->nextButton()->click();
    const int first = window->table()->selectionModel()->currentIndex().row();
    QVERIFY(first >= 0);
    window->nextButton()->click();
    const int second = window->table()->selectionModel()->currentIndex().row();
    QVERIFY(second > first);
    window->nextButton()->click();
    QVERIFY(window->table()->selectionModel()->currentIndex().row() > second);

    // Previous goes back one.
    window->previousButton()->click();
    QCOMPARE(window->table()->selectionModel()->currentIndex().row(), second);

    // At the last one, Next stays there.
    window->nextButton()->click();
    window->nextButton()->click();
    const int last = window->table()->selectionModel()->currentIndex().row();
    window->nextButton()->click();
    QCOMPARE(window->table()->selectionModel()->currentIndex().row(), last);
}

void TestCompare::aBinaryFileIsRefused()
{
    QTemporaryDir root;
    ScriptedDialogs dialogs;
    const QString binary = root.path() + "/photo.bin";
    QFile file(binary);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray("ok so far\x00 and not after", 22));
    file.close();

    std::unique_ptr<CompareWindow> window(CompareWindow::open(binary, binary, dialogs));
    QVERIFY(!window->statusLabel()->text().isEmpty());
    QVERIFY(window->statusLabel()->text().contains(QStringLiteral("not a text file")));
    QCOMPARE(window->differences(), 0);
}

void TestCompare::theOptionsChangeTheComparison()
{
    QTemporaryDir root;
    ScriptedDialogs dialogs;
    const QString left = makeFile(root.path(), "left.txt", text("one  \n\n"));
    const QString right = makeFile(root.path(), "right.txt", text("one\n"));

    std::unique_ptr<CompareWindow> window(CompareWindow::open(left, right, dialogs));
    // The spaces at the end and the blank line are passed over.
    QVERIFY(window->whitespaceBox()->isChecked());
    QVERIFY(window->lineEndingsBox()->isChecked());
    window->blankBox()->setChecked(true);
    QVERIFY(window->statusLabel()->text().contains(QStringLiteral("the same")));

    // Without them, the lines are not the same any more.
    window->whitespaceBox()->setChecked(false);
    window->blankBox()->setChecked(true);
    QVERIFY(window->differences() > 0);
    QVERIFY(window->statusLabel()->text().contains(QStringLiteral("differences")));
}

void TestCompare::swappingSidesSwapsTheRows()
{
    QTemporaryDir root;
    ScriptedDialogs dialogs;
    const QString left = makeFile(root.path(), "left.txt", text("only on the left\ncommon\n"));
    const QString right = makeFile(root.path(), "right.txt", text("common\nonly on the right\n"));

    std::unique_ptr<CompareWindow> window(CompareWindow::open(left, right, dialogs));
    const auto isRemoved = [](const Row &row) { return row.kind == Kind::Removed; };
    const auto isAdded = [](const Row &row) { return row.kind == Kind::Added; };
    // The rows are copied out first: two calls to rows() would be two
    // different lists, and their iterators must not be mixed.
    const QList<Row> rows = window->rows();
    const int removed = std::count_if(rows.cbegin(), rows.cend(), isRemoved);
    const int added = std::count_if(rows.cbegin(), rows.cend(), isAdded);
    QVERIFY(removed > 0);
    QVERIFY(added > 0);

    QSignalSpy swapped(window.get(), &CompareWindow::swapped);
    window->swapButton()->click();
    QCOMPARE(swapped.count(), 1);
    // What was only on the left is only on the right now.
    const QList<Row> swappedRows = window->rows();
    QCOMPARE(int(std::count_if(swappedRows.cbegin(), swappedRows.cend(), isRemoved)), added);
    QCOMPARE(int(std::count_if(swappedRows.cbegin(), swappedRows.cend(), isAdded)), removed);
}

QTEST_MAIN(TestCompare)
#include "compare.moc"
