#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTest>

#include "helpers.h"
#include "panels/PathBar.h"

namespace {

constexpr int kChevron = 16;  // the gap between two folder buttons

// The folders from the volume down to `path`, worked out independently of
// the bar: by splitting the path.
QStringList foldersDownTo(QString path)
{
    path = QDir::cleanPath(path);
#ifdef Q_OS_WIN
    QString current = path.left(3);
    const QStringList parts = path.mid(3).split(QLatin1Char('/'), Qt::SkipEmptyParts);
#else
    QString current = QStringLiteral("/");
    const QStringList parts = path.mid(1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
#endif
    QStringList result{QDir::cleanPath(current)};
    for (const QString &part : parts) {
        current = QDir::cleanPath(current + QLatin1Char('/') + part);
        result.append(current);
    }
    return result;
}

// A bar in a window of its own, showing <temp>/a/b/c.
struct Fixture {
    QTemporaryDir root;
    QString deep;
    QString elsewhere;
    PathBar bar;
    bool ready = false;

    Fixture()
        : deep(root.path() + "/a/b/c")
        , elsewhere(root.path() + "/elsewhere")
    {
        makeFolder(deep);
        makeFolder(elsewhere);
        bar.setPath(deep);
        bar.resize(2400, bar.sizeHint().height());
        bar.show();
        ready = QTest::qWaitForWindowExposed(&bar);
        QTest::qWait(20);
    }

    QLineEdit *edit() { return bar.findChild<QLineEdit *>(); }
    // Clicks at `point`, given in the bar's coordinates.
    void click(const QPoint &point)
    {
        QWidget *under = bar.childAt(point);
        QVERIFY(under);
        QTest::mouseClick(under, Qt::LeftButton, Qt::NoModifier, under->mapFrom(&bar, point));
    }
    // The gap between two folder buttons: empty space.
    int gapAfter(int index) const { return bar.folderRect(index).right() + kChevron / 2; }
};

}  // namespace

class TestPathBar : public QObject {
    Q_OBJECT

private slots:
    void showsTheFoldersFromTheVolumeDown();
    void clickingAFolderChoosesIt();
    void aClickOnEmptySpaceStartsTyping();
    void typingOnTheButtonsStartsTheField();
    void anEnteredPathIsChosen();
    void escapeDropsWhatWasTyped();
    void aNarrowBarLeavesOutTheOuterFolders();
};

void TestPathBar::showsTheFoldersFromTheVolumeDown()
{
    Fixture f;
    QVERIFY(f.ready);
    const QStringList expected = foldersDownTo(f.deep);
    QCOMPARE(f.bar.path(), QDir::cleanPath(f.deep));
    QCOMPARE(f.bar.folders(), expected);
    QVERIFY(!f.bar.isEditing());

    // Each has its place, from left to right, inside the bar.
    int right = -1;
    for (int i = 0; i < expected.size(); ++i) {
        const QRect rect = f.bar.folderRect(i);
        QVERIFY(rect.isValid());
        QVERIFY(rect.left() >= right);
        QVERIFY(rect.right() <= f.bar.width());
        right = rect.right();
    }
    QVERIFY(f.bar.folderRect(-1).isEmpty());
    QVERIFY(f.bar.folderRect(expected.size()).isEmpty());
}

void TestPathBar::clickingAFolderChoosesIt()
{
    Fixture f;
    QVERIFY(f.ready);
    QSignalSpy chosen(&f.bar, &PathBar::pathChosen);
    const QStringList folders = f.bar.folders();
    const int count = folders.size();
    QVERIFY(count >= 4);

    f.click(f.bar.folderRect(count - 3).center());
    QCOMPARE(chosen.count(), 1);
    QCOMPARE(chosen.at(0).at(0).toString(), folders.at(count - 3));

    // The current folder can be chosen too: nothing changes, but it is a choice.
    f.click(f.bar.folderRect(count - 1).center());
    QCOMPARE(chosen.count(), 2);
    QCOMPARE(chosen.at(1).at(0).toString(), folders.at(count - 1));
    QVERIFY(!f.bar.isEditing());

    // Between two folders is empty space: that starts typing instead.
    f.click(QPoint(f.gapAfter(0), f.bar.height() / 2));
    QVERIFY(f.bar.isEditing());
    QCOMPARE(chosen.count(), 2);
    f.bar.stopEditing();
}

void TestPathBar::aClickOnEmptySpaceStartsTyping()
{
    Fixture f;
    QVERIFY(f.ready);
    QSignalSpy started(&f.bar, &PathBar::editingStarted);
    QSignalSpy chosen(&f.bar, &PathBar::pathChosen);
    f.click(QPoint(f.bar.width() - 6, f.bar.height() / 2));
    QVERIFY(f.bar.isEditing());
    QCOMPARE(started.count(), 1);

    // The field has the focus, with the whole path selected, ready to be replaced.
    QCOMPARE(QApplication::focusWidget(), f.edit());
    QCOMPARE(f.edit()->text(), f.bar.path());
    QCOMPARE(f.edit()->selectedText(), f.bar.path());

    // Leaving the field drops what was typed.
    f.edit()->setText(QStringLiteral("/something/else"));
    f.edit()->clearFocus();
    QVERIFY(!f.bar.isEditing());
    QCOMPARE(chosen.count(), 0);
    QCOMPARE(f.bar.path(), QDir::cleanPath(f.deep));
}

void TestPathBar::typingOnTheButtonsStartsTheField()
{
    Fixture f;
    QVERIFY(f.ready);
    // The buttons fill the bar wherever the field isn't: to the right of the last one.
    QWidget *buttons = f.bar.childAt(QPoint(f.bar.width() - 4, f.bar.height() / 2));
    QVERIFY(buttons);
    QVERIFY(buttons != f.edit());
    buttons->setFocus(Qt::OtherFocusReason);

    // A printable key starts the path with that character...
    QTest::keyClick(buttons, Qt::Key_D);
    QVERIFY(f.bar.isEditing());
    QCOMPARE(f.edit()->text(), QString("d"));
    f.bar.stopEditing();
    QVERIFY(!f.bar.isEditing());

    // ...and so does Enter, with the whole path.
    buttons->setFocus(Qt::OtherFocusReason);
    QTest::keyClick(buttons, Qt::Key_Return);
    QVERIFY(f.bar.isEditing());
    QCOMPARE(f.edit()->text(), f.bar.path());
    f.bar.stopEditing();
}

void TestPathBar::anEnteredPathIsChosen()
{
    Fixture f;
    QVERIFY(f.ready);
    QSignalSpy chosen(&f.bar, &PathBar::pathChosen);
    QSignalSpy ended(&f.bar, &PathBar::editingEnded);

    f.bar.edit();
    f.edit()->setText(f.elsewhere);
    QTest::keyClick(f.edit(), Qt::Key_Return);
    QCOMPARE(chosen.count(), 1);
    QCOMPARE(chosen.at(0).at(0).toString(), f.elsewhere);
    QVERIFY(!f.bar.isEditing());
    QCOMPARE(ended.count(), 1);
    QCOMPARE(ended.at(0).at(0).toBool(), true);

    // A relative path is taken from the folder shown.
    f.bar.edit();
    f.edit()->setText(QStringLiteral("../../../elsewhere/./"));
    QTest::keyClick(f.edit(), Qt::Key_Enter);
    QCOMPARE(chosen.count(), 2);
    QCOMPARE(chosen.at(1).at(0).toString(), f.elsewhere);

    // So is the home folder.
    f.bar.edit();
    f.edit()->setText(QStringLiteral("~"));
    QTest::keyClick(f.edit(), Qt::Key_Return);
    QCOMPARE(chosen.count(), 3);
    QCOMPARE(chosen.at(2).at(0).toString(), QDir::cleanPath(QDir::homePath()));

    // The field completes folder names as they are typed.
    QVERIFY(f.edit()->completer());
    QCOMPARE(ended.count(), 3);
    QVERIFY(!f.bar.isEditing());
}

void TestPathBar::escapeDropsWhatWasTyped()
{
    Fixture f;
    QVERIFY(f.ready);
    QSignalSpy chosen(&f.bar, &PathBar::pathChosen);
    QSignalSpy ended(&f.bar, &PathBar::editingEnded);
    f.bar.edit();
    f.edit()->setText(f.elsewhere);
    QTest::keyClick(f.edit(), Qt::Key_Escape);
    QVERIFY(!f.bar.isEditing());
    QCOMPARE(chosen.count(), 0);
    QCOMPARE(ended.count(), 1);
    QCOMPARE(ended.at(0).at(0).toBool(), false);
    QCOMPARE(f.bar.path(), QDir::cleanPath(f.deep));

    // Showing another folder while typing ends the typing as well.
    f.bar.edit();
    f.bar.setPath(f.elsewhere);
    QVERIFY(!f.bar.isEditing());
    QCOMPARE(f.bar.folders().last(), f.elsewhere);
    QCOMPARE(ended.count(), 2);
}

void TestPathBar::aNarrowBarLeavesOutTheOuterFolders()
{
    Fixture f;
    QVERIFY(f.ready);
    const int count = f.bar.folders().size();
    QVERIFY(count >= 4);
    const QString shown = f.bar.path();
    // Wide enough for all of them.
    for (int i = 0; i < count; ++i) {
        QVERIFY2(f.bar.folderRect(i).isValid(), qPrintable(QString::number(i)));
    }

    // Narrower than all the names.
    f.bar.resize(150, f.bar.height());
    QTest::qWait(20);

    // They are all still known, and the current one is still shown.
    QCOMPARE(f.bar.folders().size(), count);
    QCOMPARE(f.bar.folders().last(), shown);
    const QRect last = f.bar.folderRect(count - 1);
    QVERIFY(last.isValid());
    QVERIFY(last.right() < f.bar.width());
    // The outer folders gave way, but not all of them: the row is used.
    int visible = 0;
    for (int i = 0; i < count; ++i) {
        visible += f.bar.folderRect(i).isValid() ? 1 : 0;
    }
    QVERIFY(visible < count);
    QVERIFY(visible >= 1);

    // Wide again: everything is back.
    f.bar.resize(2400, f.bar.height());
    QTest::qWait(20);
    for (int i = 0; i < count; ++i) {
        QVERIFY2(f.bar.folderRect(i).isValid(), qPrintable(QString::number(i)));
    }

    // Typing still starts from the empty space. The bar is taken down with
    // the field still open: that must not crash.
    f.click(QPoint(f.bar.width() - 3, f.bar.height() / 2));
    QVERIFY(f.bar.isEditing());
    QCOMPARE(f.edit()->text(), shown);
}

QTEST_MAIN(TestPathBar)
#include "pathbar.moc"
