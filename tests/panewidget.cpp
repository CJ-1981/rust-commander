#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <optional>

#include "app/Dialogs.h"
#include "helpers.h"
#include "ops/OperationRunner.h"
#include "panels/FileModel.h"
#include "panels/FileProxyModel.h"
#include "panels/FileView.h"
#include "panels/PaneWidget.h"

namespace {

// Answers from a script, and remembers what it was asked.
class ScriptedDialogs : public Dialogs {
public:
    std::optional<QString> askText(const QString &, const QString &, const QString &value) override
    {
        ++textsAsked;
        offered = value;
        return echoOffered ? std::optional<QString>(value) : textAnswer;
    }
    Answer ask(const QString &, const QString &question) override
    {
        questions.append(question);
        return answer;
    }
    void tell(const QString &title, const QString &text) override { told.append(title + ": " + text); }
    FileOps::Existing askOverwrite(const QString &target) override
    {
        targets.append(target);
        return FileOps::Existing::Cancel;
    }

    std::optional<QString> textAnswer;
    bool echoOffered = false;  // answer with the name offered
    QString offered;
    Answer answer = Answer::Yes;
    QStringList questions;
    QStringList told;
    QStringList targets;
    int textsAsked = 0;
};

// A pane in a window of its own, showing <temp>/home/a, which holds the folder
// b and the files one.txt and two.txt.
struct Fixture {
    QTemporaryDir root;
    QString home;
    QWidget window;
    ScriptedDialogs dialogs;
    OperationRunner runner{dialogs, &window};
    PaneWidget pane{dialogs, runner};
    QSignalSpy operations{&runner, &OperationRunner::finished};
    bool ready = false;

    Fixture()
        : home(root.path() + "/home")
    {
        makeFolder(home + "/a/b/c");
        makeFolder(home + "/other");
        writeFile(home + "/a/one.txt", "1");
        writeFile(home + "/a/two.txt", "22");
        writeFile(home + "/a/b/three.md", "333");

        auto *layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(&pane);
        pane.addTab(home + "/a");
        window.resize(660, 520);
        window.show();
        ready = QTest::qWaitForWindowExposed(&window) && settle();
    }

    // Waits until the listing of the folder shown has arrived.
    bool settle()
    {
        return QTest::qWaitFor([&] { return !pane.files()->fileModel()->isLoading(); });
    }

    int rows() const { return pane.files()->model()->rowCount(); }
    QStringList names() const { return ::names(*pane.files()->model()); }
    QString path(const QString &relative) const { return home + "/" + relative; }
    QModelIndex index(const QString &name) const
    {
        const int row = rowOfName(*pane.files()->model(), name);
        return row < 0 ? QModelIndex() : pane.files()->model()->index(row, 0);
    }
    // Selects one item, with the cursor on it.
    void select(const QString &name)
    {
        pane.files()->selectionModel()->setCurrentIndex(
                index(name), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    // Delivers a drop the way Qt does: the view is asked first whether it takes the drag.
    bool drop(const QMimeData &mime, const QString &onto)
    {
        QWidget *viewport = pane.files()->viewport();
        const QPoint position = pane.files()->visualRect(index(onto)).center();
        const Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction;
        QDragEnterEvent enter(position, actions, &mime, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &enter);
        QDropEvent event(QPointF(position), actions, &mime, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &event);
        return event.isAccepted();
    }
};

// A drop of `paths` that comes from another application.
bool dropFromOutside(Fixture &f, const QStringList &paths, const QString &onto)
{
    QMimeData mime;
    QList<QUrl> urls;
    for (const QString &path : paths) {
        urls.append(QUrl::fromLocalFile(path));
    }
    mime.setUrls(urls);
    return f.drop(mime, onto);
}

}  // namespace

class TestPaneWidget : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void showsTheFolder();
    void spaceMarksAFolderAndTellsHowBigItIs();
    void keepsAHistory();
    void goingUpSelectsTheFolderItCameFrom();
    void aFilePathShowsItsFolderWithTheFileSelected();
    void tabsCanBeAddedSwitchedAndClosed();
    void theSameFolderIsNotTwiceInTheHistory();
    void aMissingFolderIsRefused();
    void aFilterNarrowsTheList();
    void hiddenFilesCanBeShown();
    void aNewFolderIsAskedFor();
    void renamingIsAskedFor();
    void deletingAsksFirst();
    void copyingAndMovingIntoAFolder();
    void copyingToTheOtherPaneIsAskedFor();
    void openingACursorOnAFolderGoesIn();
    void aRefreshKeepsTheSelectionAndFindsNewFiles();
    void changesShowUpByThemselves();
    void aDropFromOutsideCopies();
    void aDropFromTheOtherPaneAsksFirst();
    void aClickMakesThePaneTheActiveOne();

    // ---- added by the reviewer ----
    void reviewNothingIsDoneToTheFolderLeftBehind();
    void reviewTheFolderShownIsRemovedByAnotherProgram();
};

void TestPaneWidget::initTestCase()
{
    qRegisterMetaType<PaneWidget *>("PaneWidget*");
}

void TestPaneWidget::spaceMarksAFolderAndTellsHowBigItIs()
{
    // Space marks the row, as everywhere; on a folder it also has the size
    // computed, so that the cell fills without the folder-sizes switch.
    Fixture f;
    QVERIFY(f.ready);
    const QModelIndex b = f.index("b");
    QVERIFY(b.isValid());
    f.pane.files()->selectionModel()->setCurrentIndex(
            b, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QWidget *view = f.pane.files();
    view->setFocus();
    QTest::keyClick(view, Qt::Key_Space);
    QVERIFY(f.pane.files()->selectionModel()->isSelected(f.index("b")));

    const QModelIndex sizeCell = f.pane.files()->model()->index(b.row(), FileModel::SizeColumn);
    QTRY_COMPARE_WITH_TIMEOUT(sizeCell.data().toString().isEmpty(), false, 15000);
    QVERIFY(sizeCell.data(FileModel::FolderSizeRole).toLongLong() >= 0);
}

void TestPaneWidget::showsTheFolder()
{
    Fixture f;
    QVERIFY(f.ready);
    QCOMPARE(f.pane.directory(), f.path("a"));
    QCOMPARE(f.names(), QStringList({"b", "one.txt", "two.txt"}));
    // The first item has the cursor.
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/b")}));
    QCOMPARE(f.pane.cursorPath(), f.path("a/b"));
    QVERIFY(f.pane.statusLabel()->text().contains("2"));
    QCOMPARE(f.pane.tabPaths(), QStringList({f.path("a")}));
    QVERIFY(!f.pane.canGoBack());
    QVERIFY(!f.pane.canGoForward());
    // Marked or not, as the window says.
    f.pane.setActive(false);
    QVERIFY(!f.pane.isActive());
    f.pane.setActive(true);
    QVERIFY(f.pane.isActive());
}

void TestPaneWidget::keepsAHistory()
{
    Fixture f;
    QVERIFY(f.ready);
    f.pane.openCursor();  // b
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), f.path("a/b"));
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/b/c")}));
    QVERIFY(f.pane.canGoBack());
    QVERIFY(!f.pane.canGoForward());

    f.pane.navigateTo(f.path("a/b/c"));
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), f.path("a/b/c"));

    // Back to b: what was selected there is again.
    f.pane.goBack();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), f.path("a/b"));
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/b/c")}));
    QVERIFY(f.pane.canGoForward());

    // Back once more: a, with b selected, the folder that was gone into.
    f.pane.goBack();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), f.path("a"));
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/b")}));
    QVERIFY(!f.pane.canGoBack());

    f.pane.goForward();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), f.path("a/b"));
    f.pane.goForward();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), f.path("a/b/c"));
    QVERIFY(!f.pane.canGoForward());
}

void TestPaneWidget::goingUpSelectsTheFolderItCameFrom()
{
    Fixture f;
    QVERIFY(f.ready);
    f.pane.navigateTo(f.path("a/b/c"));
    QVERIFY(f.settle());
    f.pane.goUp();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), f.path("a/b"));
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/b/c")}));
    f.pane.goUp();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/b")}));
    f.pane.goUp();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), f.home);
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a")}));
    f.pane.goHome();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), QDir::cleanPath(QDir::homePath()));
}

void TestPaneWidget::aFilePathShowsItsFolderWithTheFileSelected()
{
    Fixture f;
    QVERIFY(f.ready);
    f.pane.navigateTo(f.path("a/one.txt"));
    QVERIFY(f.settle());
    // The folder is shown already; the file gets selected.
    QCOMPARE(f.pane.directory(), f.path("a"));
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/one.txt")}));

    // In a new tab.
    f.pane.addTab(f.path("a/b/three.md"));
    QVERIFY(f.settle());
    QCOMPARE(f.pane.tabPaths(), QStringList({f.path("a"), f.path("a/b")}));
    QCOMPARE(f.pane.directory(), f.path("a/b"));
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/b/three.md")}));
}

void TestPaneWidget::tabsCanBeAddedSwitchedAndClosed()
{
    Fixture f;
    QVERIFY(f.ready);
    f.select("one.txt");
    f.pane.addTab(f.path("other"));
    QVERIFY(f.settle());
    QCOMPARE(f.pane.tabPaths(), QStringList({f.path("a"), f.path("other")}));
    QCOMPARE(f.pane.directory(), f.path("other"));

    // The last tab stays.
    f.pane.closeTab(0);
    QCOMPARE(f.pane.tabPaths(), QStringList({f.path("other")}));
    f.pane.closeTab(0);
    QCOMPARE(f.pane.tabPaths(), QStringList({f.path("other")}));

    // A tab without a folder shows the folder shown already.
    f.pane.addTab();
    QCOMPARE(f.pane.tabPaths().last(), f.path("other"));
}

void TestPaneWidget::theSameFolderIsNotTwiceInTheHistory()
{
    Fixture f;
    QVERIFY(f.ready);
    f.pane.navigateTo(f.path("a/b"));
    QVERIFY(f.settle());
    f.pane.navigateTo(f.path("a/b/"));
    QVERIFY(f.settle());
    QVERIFY(!f.pane.canGoForward());
    f.pane.goBack();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), f.path("a"));
    QVERIFY(!f.pane.canGoBack());  // b is in the history once
}

void TestPaneWidget::aMissingFolderIsRefused()
{
    Fixture f;
    QVERIFY(f.ready);
    f.pane.navigateTo(f.path("no-such-folder"));
    QCOMPARE(f.dialogs.told.size(), 1);
    QCOMPARE(f.pane.directory(), f.path("a"));
}

void TestPaneWidget::aFilterNarrowsTheList()
{
    Fixture f;
    QVERIFY(f.ready);
    f.pane.showFilter();
    QCOMPARE(QApplication::focusWidget(), f.pane.filterField());
    f.pane.filterField()->setText(QStringLiteral("T"));  // whatever the case
    QTRY_COMPARE(f.names(), QStringList({"one.txt", "two.txt"}));
    QVERIFY(f.pane.statusLabel()->text().contains("2 of 3"));
    // The filter is by name only.
    f.pane.filterField()->setText(QStringLiteral("three"));
    QCOMPARE(f.rows(), 0);
    f.pane.filterField()->clear();
    QCOMPARE(f.rows(), 3);
}

void TestPaneWidget::hiddenFilesCanBeShown()
{
    Fixture f;
    QVERIFY(f.ready);
    writeFile(f.path("a/.secret"), "s");
    setHiddenAttribute(f.path("a/.secret"));
    f.pane.refresh();
    QVERIFY(f.settle());
    QCOMPARE(f.rows(), 3);
    f.pane.toggleHidden();
    QCOMPARE(f.names(), QStringList({"b", ".secret", "one.txt", "two.txt"}));
    f.pane.toggleHidden();
    QCOMPARE(f.rows(), 3);
}

void TestPaneWidget::aNewFolderIsAskedFor()
{
    Fixture f;
    QVERIFY(f.ready);
    // Cancelled: nothing.
    f.dialogs.textAnswer = std::nullopt;
    f.pane.newFolder();
    QVERIFY(!QFileInfo::exists(f.path("a/New folder")));
    // The name offered is one that isn't taken.
    f.dialogs.echoOffered = true;
    f.pane.newFolder();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/New folder")}));
    f.pane.newFolder();
    QVERIFY(f.settle());
    QCOMPARE(f.dialogs.offered, QStringLiteral("New folder 2"));
    QVERIFY(QFileInfo::exists(f.path("a/New folder 2")));
    f.dialogs.echoOffered = false;
    // A name that is taken: said so, nothing more.
    f.dialogs.textAnswer = QStringLiteral("one.txt");
    f.pane.newFolder();
    QVERIFY(f.settle());
    QCOMPARE(f.dialogs.told.size(), 1);
}

void TestPaneWidget::renamingIsAskedFor()
{
    Fixture f;
    QVERIFY(f.ready);
    f.select("two.txt");
    f.dialogs.textAnswer = QStringLiteral("renamed.txt");
    f.pane.renameCursor();
    QVERIFY(f.settle());
    QVERIFY(!QFileInfo::exists(f.path("a/two.txt")));
    QVERIFY(QFileInfo::exists(f.path("a/renamed.txt")));
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/renamed.txt")}));
    QCOMPARE(f.dialogs.offered, QStringLiteral("two.txt"));

    // Cancelled, or the same name: nothing.
    f.dialogs.textAnswer = std::nullopt;
    f.pane.renameCursor();
    f.dialogs.textAnswer = QStringLiteral("renamed.txt");
    f.pane.renameCursor();
    QVERIFY(QFileInfo::exists(f.path("a/renamed.txt")));
    // A name that is taken: said so.
    f.dialogs.textAnswer = QStringLiteral("one.txt");
    f.pane.renameCursor();
    QVERIFY(f.settle());
    QVERIFY(QFileInfo::exists(f.path("a/renamed.txt")));
    QCOMPARE(f.dialogs.told.size(), 1);
}

void TestPaneWidget::deletingAsksFirst()
{
    Fixture f;
    QVERIFY(f.ready);
    f.select("one.txt");
    f.dialogs.answer = Dialogs::Answer::No;
    const int before = f.operations.count();
    f.pane.deleteSelected(true);
    QCOMPARE(f.operations.count(), before);
    QVERIFY(QFileInfo::exists(f.path("a/one.txt")));

    f.dialogs.answer = Dialogs::Answer::Yes;
    f.pane.deleteSelected(true);
    QTRY_COMPARE_WITH_TIMEOUT(f.operations.count(), before + 1, 15000);
    QVERIFY(!QFileInfo::exists(f.path("a/one.txt")));
    QCOMPARE(f.dialogs.questions.size(), 2);
    // The cursor ends on the row where the file was.
    QVERIFY(f.settle());
    QTRY_COMPARE_WITH_TIMEOUT(f.pane.selectedPaths(), QStringList({f.path("a/two.txt")}), 5000);
}

void TestPaneWidget::copyingAndMovingIntoAFolder()
{
    Fixture f;
    QVERIFY(f.ready);
    auto *selection = f.pane.files()->selectionModel();
    selection->setCurrentIndex(f.index("one.txt"),
                               QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selection->select(f.index("two.txt"), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    const int before = f.operations.count();
    f.pane.copyTo(f.path("other"));
    QTRY_COMPARE_WITH_TIMEOUT(f.operations.count(), before + 1, 15000);
    QVERIFY(QFileInfo::exists(f.path("other/one.txt")));
    QVERIFY(QFileInfo::exists(f.path("other/two.txt")));
    QVERIFY(QFileInfo::exists(f.path("a/one.txt")));

    f.select("b");
    f.pane.moveTo(f.path("other"));
    QTRY_COMPARE_WITH_TIMEOUT(f.operations.count(), before + 2, 15000);
    QVERIFY(QFileInfo::exists(f.path("other/b/three.md")));
    QVERIFY(!QFileInfo::exists(f.path("a/b")));
    // The pane shows the folder it is gone from.
    QTRY_COMPARE_WITH_TIMEOUT(f.rows(), 2, 5000);
}

void TestPaneWidget::copyingToTheOtherPaneIsAskedFor()
{
    Fixture f;
    QVERIFY(f.ready);
    f.select("one.txt");
    QSignalSpy transfer(&f.pane, &PaneWidget::transferRequested);
    f.pane.copyToOtherPane();
    QCOMPARE(transfer.count(), 1);
    QCOMPARE(transfer.at(0).at(0).toStringList(), QStringList({f.path("a/one.txt")}));
    QCOMPARE(transfer.at(0).at(1).toBool(), false);
    f.pane.moveToOtherPane();
    QCOMPARE(transfer.count(), 2);
    QCOMPARE(transfer.at(1).at(1).toBool(), true);
    // Nothing selected: nothing asked.
    f.pane.files()->clearSelection();
    f.pane.copyToOtherPane();
    QCOMPARE(transfer.count(), 2);
}

void TestPaneWidget::openingACursorOnAFolderGoesIn()
{
    Fixture f;
    QVERIFY(f.ready);
    f.pane.openCursor();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), f.path("a/b"));
}

void TestPaneWidget::aRefreshKeepsTheSelectionAndFindsNewFiles()
{
    Fixture f;
    QVERIFY(f.ready);
    f.select("two.txt");
    writeFile(f.path("a/new.txt"), "n");
    f.pane.refresh();
    QVERIFY(f.settle());
    QCOMPARE(f.names(), QStringList({"b", "new.txt", "one.txt", "two.txt"}));
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/two.txt")}));
}

void TestPaneWidget::changesShowUpByThemselves()
{
    Fixture f;
    QVERIFY(f.ready);
    writeFile(f.path("a/later.txt"), "l");
    QTRY_COMPARE_WITH_TIMEOUT(f.rows(), 4, 5000);
    QFile::remove(f.path("a/later.txt"));
    QTRY_COMPARE_WITH_TIMEOUT(f.rows(), 3, 5000);
}

void TestPaneWidget::aDropFromOutsideCopies()
{
    Fixture f;
    QVERIFY(f.ready);
    writeFile(f.path("other/dropped.txt"), "d");
    const int before = f.operations.count();
    // Onto the folder b: that is where it goes.
    QVERIFY(dropFromOutside(f, {f.path("other/dropped.txt")}, "b"));
    QTRY_COMPARE_WITH_TIMEOUT(f.operations.count(), before + 1, 15000);
    QVERIFY(QFileInfo::exists(f.path("a/b/dropped.txt")));
    QVERIFY(QFileInfo::exists(f.path("other/dropped.txt")));  // copied, not moved
    // Nothing that can be dropped: refused.
    QMimeData text;
    text.setText(QStringLiteral("hello"));
    QVERIFY(!f.drop(text, "b"));
}

void TestPaneWidget::aDropFromTheOtherPaneAsksFirst()
{
    Fixture f;
    QVERIFY(f.ready);
    writeFile(f.path("other/dropped.txt"), "d");
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(f.path("other/dropped.txt"))});
    mime.setData(dragSourceFormat, f.path("other").toUtf8());
    // The menu is closed with Escape, as the user would.
    bool menuSeen = false;
    QTimer closer;
    connect(&closer, &QTimer::timeout, [&] {
        if (QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            menuSeen = true;
            QTest::keyClick(menu, Qt::Key_Escape);
        }
    });
    closer.start(20);
    const int before = f.operations.count();
    QVERIFY(f.drop(mime, "b"));
    QTRY_VERIFY_WITH_TIMEOUT(menuSeen, 5000);
    QTest::qWait(150);
    closer.stop();
    QCOMPARE(f.operations.count(), before);
    QVERIFY(!QFileInfo::exists(f.path("a/b/dropped.txt")));
}

void TestPaneWidget::aClickMakesThePaneTheActiveOne()
{
    Fixture f;
    QVERIFY(f.ready);
    QSignalSpy activated(&f.pane, &PaneWidget::activated);
    QTest::mouseClick(f.pane.files()->viewport(), Qt::LeftButton, Qt::NoModifier,
                      f.pane.files()->visualRect(f.index("one.txt")).center());
    QVERIFY(f.pane.isActive());
    QVERIFY(activated.count() >= 1);
    QCOMPARE(activated.at(0).at(0).value<PaneWidget *>(), &f.pane);
    QCOMPARE(f.pane.selectedPaths(), QStringList({f.path("a/one.txt")}));
    // The keyboard focus coming from elsewhere into the files counts as working in the pane.
    f.pane.filterField()->setFocus(Qt::OtherFocusReason);
    f.pane.setActive(false);
    QVERIFY(!f.pane.isActive());
    f.pane.setFocusToFiles();
    QVERIFY(f.pane.isActive());
}

// The pane goes to another folder. Until its listing arrives the table still
// shows the rows of the folder left behind, under the new path.
// While the listing of the folder gone to is on its way, the table still
// shows the rows of the folder left. Nothing may be done to them.
void TestPaneWidget::reviewNothingIsDoneToTheFolderLeftBehind()
{
    Fixture f;
    QVERIFY(f.ready);
    f.select("one.txt");
    f.pane.navigateTo(f.path("other"));
    QCOMPARE(f.pane.directory(), f.path("other"));

    // (The listing of the new folder is on its way here.)
    QVERIFY(f.pane.selectedPaths().isEmpty());
    QVERIFY(f.pane.cursorPath().isEmpty());

    // The user asks to delete what the table shows as selected.
    f.dialogs.answer = Dialogs::Answer::Yes;
    const int before = f.operations.count();
    f.pane.deleteSelected(true);
    QTest::qWait(300);
    QCOMPARE(f.dialogs.questions.size(), 0);
    QVERIFY2(QFileInfo::exists(f.path("a/one.txt")), "a file of the folder left behind was deleted");
    QCOMPARE(f.operations.count(), before);

    // Once the new folder is listed, it is shown (empty as it is).
    QVERIFY(f.settle());
    QCOMPARE(f.rows(), 0);
}

// Another program removes the folder the pane shows.
void TestPaneWidget::reviewTheFolderShownIsRemovedByAnotherProgram()
{
#ifdef Q_OS_WIN
    // A folder that a QFileSystemWatcher is watching can't be removed there
    // while it is watched: the scenario doesn't come up.
    QSKIP("Windows holds the watched folder open");
#endif
    Fixture f;
    QVERIFY(f.ready);
    QVERIFY(QDir().rmdir(f.path("a/b/c")));
    QVERIFY(QFile::remove(f.path("a/b/three.md")));
    QVERIFY(QDir().rmdir(f.path("a/b")));
    QVERIFY(QFile::remove(f.path("a/one.txt")));
    QVERIFY(QFile::remove(f.path("a/two.txt")));
    QVERIFY(QDir().rmdir(f.path("a")));
    QTRY_COMPARE_WITH_TIMEOUT(f.rows(), 0, 5000);

    // Said so, and still possible to leave.
    QVERIFY(!f.pane.files()->fileModel()->error().isEmpty());
    QTRY_VERIFY(f.pane.statusLabel()->text().contains(QStringLiteral("Nothing here")));
    f.pane.goUp();
    QVERIFY(f.settle());
    QCOMPARE(f.pane.directory(), f.home);
}

QTEST_MAIN(TestPaneWidget)
#include "panewidget.moc"
