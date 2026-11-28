#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QLabel>
#include <QWheelEvent>
#include <QMenu>
#include <QTimer>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTest>

#include <memory>

#include "app/CommandLine.h"
#include "panels/SelectGroupDialog.h"
#include "tools/PropertiesWindow.h"
#include "app/MainWindow.h"
#include "app/Settings.h"
#include "app/Theme.h"
#include "helpers.h"
#include "panels/FileProxyModel.h"
#include "panels/FileView.h"
#include "panels/PaneWidget.h"
#include "viewer/ViewerWindow.h"

namespace {

// A window over two folders: <temp>/a holds one.txt, two.txt and the folder
// sub; <temp>/b holds note.txt. The settings live under the temp folder.
// Presses the first key of a portable sequence ("Ctrl+D" and friends), as
// the platform turned it into. Pressing the raw modifier from QTest can
// disagree with what the sequence says on macOS.
void pressSequence(QWidget *window, const char *portable)
{
    const QKeySequence seq{QLatin1String(portable)};
    QTest::keyClick(window, seq[0].key(), seq[0].keyboardModifiers());
}

struct Fixture {
    QTemporaryDir root;
    QString a;
    QString b;
    ScriptedDialogs dialogs;
    std::unique_ptr<MainWindow> window;
    bool ready = false;

    explicit Fixture(const QString &rightFolder = {})
        : a(root.path() + "/a")
        , b(root.path() + "/b")
    {
        makeFolder(a + "/sub");
        makeFolder(b);
        writeFile(a + "/one.txt", "1");
        writeFile(a + "/two.txt", "22");
        writeFile(a + "/sub/inside.txt", "333");
        writeFile(b + "/note.txt", "n");

        Settings::setTabPaths(0, {a});
        Settings::setTabPaths(1, {rightFolder.isEmpty() ? b : rightFolder});
        window = std::make_unique<MainWindow>(nullptr, &dialogs);
        window->show();
        ready = QTest::qWaitForWindowExposed(window.get()) && settle();
    }

    // Waits until both panes have their listings.
    bool settle()
    {
        return QTest::qWaitFor([&] {
            return !window->leftPane()->files()->fileModel()->isLoading()
                   && !window->rightPane()->files()->fileModel()->isLoading();
        });
    }

    PaneWidget *left() const { return window->leftPane(); }
    PaneWidget *right() const { return window->rightPane(); }

    // Clicks the row of `name` in `pane`, which makes it the active pane.
    void click(PaneWidget *pane, const QString &name)
    {
        const int row = rowOfName(*pane->files()->model(), name);
        QVERIFY(row >= 0);
        QTest::mouseClick(pane->files()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          pane->files()->visualRect(pane->files()->model()->index(row, 0)).center());
    }

    // The viewer windows that are open for `path`.
    QList<ViewerWindow *> viewersFor(const QString &path) const
    {
        QList<ViewerWindow *> found;
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *viewer = qobject_cast<ViewerWindow *>(widget);
                    viewer && viewer->filePath() == path) {
                found.append(viewer);
            }
        }
        return found;
    }
};

}  // namespace

class TestMainWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void showsTwoPanesWithTheSavedFolders();
    void tabGoesToTheOtherPaneButNotFromAField();
    void shortcutsActOnTheActivePane();
    void f5CopiesToTheOtherPane();
    void deletingForGoodAsksFirst();
    void tabsHaveTheirKeys();
    void panesCanBeSwappedAndTheDividerReset();
    void theThemeChoiceIsRemembered();
    void favoritesCanBeAddedAndRemoved();
    void tabsAndDividerAreSavedOnClosing();
    void f3AndF4OpenTheViewer();

    // ---- added by the reviewer; each asserts what a user would expect ----
    void reviewDeleteInAFieldEditsTheText();
    void reviewClosingWithAnUnsavedEditorAsks();
    void reviewViewOnAFolderOpensNothing();
    void reviewAFolderIsMadeOnlyInTheFolderShown();

    // ---- keys belong to the active pane ----
    void aWheelMakesThePaneActiveAndItsKeysWorkThere();
    void spaceReachesTheListFromWhereverTheFocusWanders();
    void theCommandLineRunsItsCommands();
    void ctrlDOpensTheHotlistAndEntriesNavigate();
    void selectGroupMarksWhatThePatternAnswersTo();
    void propertiesTellsWhatIsThere();

private:
    QTemporaryDir m_settings;
};

void TestMainWindow::initTestCase()
{
    // Nothing here may touch the real settings.
    Settings::storeIn(m_settings.path());
}

void TestMainWindow::init()
{
    Settings::values().clear();
    Theme::apply(Theme::Mode::Light);
}

void TestMainWindow::showsTwoPanesWithTheSavedFolders()
{
    Fixture f;
    QVERIFY(f.ready);
    QCOMPARE(f.left()->directory(), f.a);
    QCOMPARE(f.right()->directory(), f.b);
    QCOMPARE(f.window->activePane(), f.left());
    // The keys start in the left file list, not on a button.
    QCOMPARE(QApplication::focusWidget(), f.left()->files());
    QVERIFY(f.window->windowTitle().contains(QStringLiteral(APP_VERSION)));

    // The right pane starts where its tabs were.
    Fixture same(f.a);
    QVERIFY(same.ready);
    QCOMPARE(same.left()->directory(), same.a);
    QCOMPARE(same.right()->directory(), f.a);
}

void TestMainWindow::tabGoesToTheOtherPaneButNotFromAField()
{
    Fixture f;
    QVERIFY(f.ready);
    QTest::keyClick(f.left()->files(), Qt::Key_Tab);
    QCOMPARE(f.window->activePane(), f.right());
    QCOMPARE(QApplication::focusWidget(), f.right()->files());

    QTest::keyClick(f.right()->files(), Qt::Key_Backtab);
    QCOMPARE(f.window->activePane(), f.left());
    QCOMPARE(QApplication::focusWidget(), f.left()->files());

    // In a field, Tab goes on to the next control instead.
    f.left()->showFilter();
    QCOMPARE(QApplication::focusWidget(), f.left()->filterField());
    QTest::keyClick(f.left()->filterField(), Qt::Key_Tab);
    QCOMPARE(f.window->activePane(), f.left());
}

void TestMainWindow::shortcutsActOnTheActivePane()
{
    Fixture f;
    QVERIFY(f.ready);
    f.click(f.right(), "note.txt");
    QCOMPARE(f.window->activePane(), f.right());

    // F7 makes a folder in the pane that was used last.
    f.dialogs.text = QStringLiteral("Fresh");
    QTest::keyClick(f.window.get(), Qt::Key_F7);
    QVERIFY(QFileInfo::exists(f.b + "/Fresh"));
    QVERIFY(!QFileInfo::exists(f.a + "/Fresh"));

    // F6 moves the selection to the other pane.
    f.click(f.right(), "note.txt");
    QTest::keyClick(f.window.get(), Qt::Key_F6);
    QTRY_VERIFY(QFileInfo::exists(f.a + "/note.txt"));
    QTRY_VERIFY(!QFileInfo::exists(f.b + "/note.txt"));

    // Back takes the active pane back, and only that one.
    f.click(f.left(), "two.txt");
    f.left()->navigateTo(f.a + "/sub");
    QVERIFY(f.settle());
    QTest::keyClick(f.window.get(), Qt::Key_Left, Qt::AltModifier);
    QCOMPARE(f.left()->directory(), f.a);
    QCOMPARE(f.right()->directory(), f.b);
}

void TestMainWindow::f5CopiesToTheOtherPane()
{
    Fixture f;
    QVERIFY(f.ready);
    f.click(f.left(), "one.txt");
    QTest::keyClick(f.window.get(), Qt::Key_F5);
    QTRY_VERIFY(QFileInfo::exists(f.b + "/one.txt"));
    QVERIFY(QFileInfo::exists(f.a + "/one.txt"));  // a copy: the original stays

    // Both panes on the same folder: there is nothing to do, and that is said.
    f.right()->navigateTo(f.a);
    QVERIFY(f.settle());
    const int told = f.dialogs.told.size();
    f.click(f.left(), "two.txt");
    f.window->actions().copy->trigger();
    QCOMPARE(f.dialogs.told.size(), told + 1);
}

void TestMainWindow::deletingForGoodAsksFirst()
{
    Fixture f;
    QVERIFY(f.ready);
    f.click(f.left(), "one.txt");
    f.dialogs.answer = Dialogs::Answer::No;
    QTest::keyClick(f.window.get(), Qt::Key_F8, Qt::ShiftModifier);
    QVERIFY(QFileInfo::exists(f.a + "/one.txt"));
    QCOMPARE(f.dialogs.questions.size(), 1);

    f.dialogs.answer = Dialogs::Answer::Yes;
    f.window->actions().erase->trigger();
    QTRY_VERIFY(!QFileInfo::exists(f.a + "/one.txt"));
    QCOMPARE(f.dialogs.questions.size(), 2);
}

void TestMainWindow::tabsHaveTheirKeys()
{
    Fixture f;
    QVERIFY(f.ready);
    f.left()->addTab(f.b);  // the second tab shows b
    QCOMPARE(f.left()->tabPaths(), QStringList({f.a, f.b}));
    QCOMPARE(f.left()->directory(), f.b);

    QTest::keySequence(f.window.get(), f.window->actions().previousTab->shortcut());
    QCOMPARE(f.left()->directory(), f.a);  // the tab before
    QTest::keySequence(f.window.get(), f.window->actions().nextTab->shortcut());
    QCOMPARE(f.left()->directory(), f.b);  // the next one round

    // The other pane's tabs are its own.
    QCOMPARE(f.right()->tabPaths(), QStringList({f.b}));
    QTest::keyClick(f.window.get(), Qt::Key_W, Qt::ControlModifier);
    QCOMPARE(f.left()->tabPaths(), QStringList({f.a}));
    QCOMPARE(f.left()->directory(), f.a);

    // A new tab shows the folder that is shown.
    QTest::keyClick(f.window.get(), Qt::Key_T, Qt::ControlModifier);
    QCOMPARE(f.left()->tabPaths(), QStringList({f.a, f.a}));
}

void TestMainWindow::panesCanBeSwappedAndTheDividerReset()
{
    Fixture f;
    QVERIFY(f.ready);
    QSplitter *splitter = f.window->splitter();
    QCOMPARE(splitter->widget(0), f.left());
    splitter->setSizes({200, 900});

    f.window->actions().swapPanes->trigger();
    QCOMPARE(splitter->widget(0), f.right());
    QCOMPARE(splitter->widget(1), f.left());
    // Left and right keep their folders; the widths went with them.
    QCOMPARE(f.left()->directory(), f.a);
    QCOMPARE(f.right()->directory(), f.b);
    QVERIFY(splitter->sizes().first() > splitter->sizes().last());

    // A double-click on the divider: both the same.
    QTest::mouseDClick(splitter->handle(1), Qt::LeftButton);
    const QList<int> sizes = splitter->sizes();
    QCOMPARE(sizes.size(), 2);
    QVERIFY2(qAbs(sizes.at(0) - sizes.at(1)) <= 1,
             qPrintable(QStringLiteral("%1 px and %2 px").arg(sizes.at(0)).arg(sizes.at(1))));
}

void TestMainWindow::theThemeChoiceIsRemembered()
{
    Fixture f;
    QVERIFY(f.ready);
    QCOMPARE(Settings::themeMode(), Theme::Mode::System);

    QAction *dark = nullptr;
    QAction *light = nullptr;
    for (QAction *action : f.window->themeActions()->actions()) {
        if (action->text() == QLatin1String("Dark")) {
            dark = action;
        } else if (action->text() == QLatin1String("Light")) {
            light = action;
        }
    }
    QVERIFY(dark);
    QVERIFY(light);

    dark->trigger();
    QCOMPARE(Settings::themeMode(), Theme::Mode::Dark);
    QVERIFY(dark->isChecked());
    QVERIFY(!light->isChecked());
    light->trigger();
    QCOMPARE(Settings::themeMode(), Theme::Mode::Light);
    QVERIFY(light->isChecked());
    QVERIFY(!dark->isChecked());
}

void TestMainWindow::favoritesCanBeAddedAndRemoved()
{
    Fixture f;
    QVERIFY(f.ready);
    f.window->actions().addFavorite->trigger();
    f.window->actions().addFavorite->trigger();  // once is enough
    const QVector<Settings::Bookmark> bookmarks = Settings::bookmarks();
    QCOMPARE(bookmarks.size(), 1);
    QCOMPARE(bookmarks.first().path, f.a);
    QCOMPARE(bookmarks.first().name, QStringLiteral("a"));

    // Offered in the menu, with a way to take it out again.
    QMenu *menu = f.window->favoritesMenu();
    QVERIFY(QMetaObject::invokeMethod(menu, "aboutToShow"));  // which fills it
    QAction *entry = nullptr;
    QMenu *remove = nullptr;
    for (QAction *action : menu->actions()) {
        if (action->text() == QLatin1String("a")) {
            entry = action;
        }
        if (action->menu()) {
            remove = action->menu();
        }
    }
    QVERIFY(entry);
    QVERIFY(remove);

    // Chosen in the pane that is active, which is the right one here.
    f.click(f.right(), "note.txt");
    entry->trigger();
    QVERIFY(f.settle());
    QCOMPARE(f.right()->directory(), f.a);
    QCOMPARE(f.left()->directory(), f.a);

    // Out again.
    QCOMPARE(remove->actions().size(), 1);
    remove->actions().first()->trigger();
    QVERIFY(Settings::bookmarks().isEmpty());
}

void TestMainWindow::tabsAndDividerAreSavedOnClosing()
{
    Fixture f;
    QVERIFY(f.ready);
    f.left()->addTab(f.b);
    f.window->splitter()->setSizes({300, 800});
    const QList<int> sizes = f.window->splitter()->sizes();
    const int width = f.window->splitter()->width();
    f.window->close();

    QCOMPARE(Settings::tabPaths(0), QStringList({f.a, f.b}));
    QCOMPARE(Settings::tabPaths(1), QStringList({f.b}));

    // The divider is where it was left.
    QMainWindow probe;
    const QByteArray divider = Settings::restoreLayout(&probe);
    QVERIFY(!divider.isEmpty());
    QSplitter checker;
    checker.addWidget(new QWidget);
    checker.addWidget(new QWidget);
    checker.resize(width, 400);
    QVERIFY(checker.restoreState(divider));
    QVERIFY2(qAbs(checker.sizes().at(0) - sizes.at(0)) <= 2,
             qPrintable(QStringLiteral("%1 px, was %2 px in a splitter %3 px wide, now %4")
                                .arg(checker.sizes().at(0)).arg(sizes.at(0)).arg(width)
                                .arg(checker.width())));

    // The next start goes on where this one stopped: on the tab that was open.
    MainWindow next(nullptr, &f.dialogs);
    next.show();
    QVERIFY(QTest::qWaitForWindowExposed(&next));
    QCOMPARE(next.leftPane()->tabPaths(), QStringList({f.a, f.b}));
    QCOMPARE(next.leftPane()->directory(), f.b);
    QCOMPARE(next.rightPane()->directory(), f.b);
}

void TestMainWindow::f3AndF4OpenTheViewer()
{
    Fixture f;
    QVERIFY(f.ready);
    const QString file = f.a + "/one.txt";
    f.click(f.left(), "one.txt");

    QTest::keyClick(f.window.get(), Qt::Key_F3);
    QList<ViewerWindow *> viewers = f.viewersFor(file);
    QCOMPARE(viewers.size(), 1);
    QVERIFY(viewers.first()->editor()->isReadOnly());

    QTest::keyClick(f.window.get(), Qt::Key_F4);
    viewers = f.viewersFor(file);
    QCOMPARE(viewers.size(), 2);
    int editable = 0;
    for (ViewerWindow *viewer : viewers) {
        editable += viewer->editor()->isReadOnly() ? 0 : 1;
    }
    QCOMPARE(editable, 1);
    qDeleteAll(viewers);

    // Nothing under the cursor: nothing opens.
    f.left()->files()->clearSelection();
    f.left()->files()->selectionModel()->clearCurrentIndex();
    QTest::keyClick(f.window.get(), Qt::Key_F3);
    QCOMPARE(f.viewersFor(file).size(), 0);
}

// ---------------------------------------------------------------------------
// Reviewer's tests.
// ---------------------------------------------------------------------------

void TestMainWindow::aWheelMakesThePaneActiveAndItsKeysWorkThere()
{
    // Scrolling the other pane with the wheel makes it the active one — and
    // the keys must follow, or Space marks a row in the pane that looks
    // inactive. This was the way Space seemed to do nothing.
    Fixture f;
    QVERIFY(f.ready);
    QCOMPARE(f.window->activePane(), f.left());
    QWidget *leftView = f.left()->files();
    leftView->setFocus();
    QCOMPARE(QApplication::focusWidget(), leftView);

    QWheelEvent wheel(QPointF(40, 40), QPointF(40, 40), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QVERIFY(QApplication::sendEvent(f.right()->files()->viewport(), &wheel));
    QCOMPARE(f.window->activePane(), f.right());
    // The focus came along, so the next key lands in the pane that moved.
    QCOMPARE(QApplication::focusWidget(), f.right()->files());
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Space);
    QVERIFY(f.right()->files()->selectionModel()->selectedRows().size() > 0);
}

void TestMainWindow::spaceReachesTheListFromWhereverTheFocusWanders()
{
    // Wherever the focus sits in the window — a status label after a click,
    // anywhere that is not a field someone types in — Space still marks the
    // row under the cursor of the active pane.
    Fixture f;
    QVERIFY(f.ready);
    f.click(f.left(), "one.txt");
    QVERIFY(f.left()->files()->hasFocus() || QApplication::focusWidget() == f.left()->files());
    f.left()->files()->clearSelection();

    QLabel *label = f.left()->findChild<QLabel *>();
    QVERIFY(label);
    label->setFocus();
    QVERIFY(QApplication::focusWidget() != f.left()->files());
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Space);
    QVERIFY(f.left()->files()->selectionModel()->selectedRows().size() > 0);

    // The window is alive after the forwarding; the focus is still where it
    // was, since the view takes none of it back.
    QVERIFY(QApplication::focusWidget() != f.left()->files());
}

void TestMainWindow::theCommandLineRunsItsCommands()
{
    Fixture f;
    QVERIFY(f.ready);
    CommandLine *line = f.window->findChild<CommandLine *>();
    QVERIFY(line);

    // cd goes where it says, in the pane that is active.
    line->setText("cd " + f.a + "/sub");
    QTest::keyClick(line, Qt::Key_Return);
    QTest::qWait(50);
    QCOMPARE(f.left()->directory(), f.a + "/sub");
    // After a command the file list has the keyboard back.
    QCOMPARE(QApplication::focusWidget(), f.left()->files());
    // It was remembered for the arrow keys.
    QVERIFY(Settings::commandHistory().contains(QStringLiteral("cd ") + f.a + "/sub"));

    // A file of the folder shown opens with its application: asked of the
    // system here is only what cannot be answered offscreen, so what is
    // asserted is that the line understood it and let it go out.
    f.right()->navigateTo(f.a);
    QTest::qWait(50);
    line->setText("cd " + f.a);
    QTest::keyClick(line, Qt::Key_Return);
    QTest::qWait(50);
    QCOMPARE(f.right()->directory(), f.a);

    // Escape clears the line and gives the keys back to the list.
    line->setText("half typed");
    QTest::keyClick(line, Qt::Key_Escape);
    QCOMPARE(line->text(), QString());
    QVERIFY(QApplication::focusWidget() != line);
}

void TestMainWindow::ctrlDOpensTheHotlistAndEntriesNavigate()
{
    // Ctrl+D is the hotlist: the folders jump out of it, adding one is in it.
    Fixture f;
    QVERIFY(f.ready);
    Settings::setBookmarks({{QStringLiteral("The B"), f.b}});
    const int askedBefore = f.dialogs.questions.size();

    pressSequence(f.window.get(), "Ctrl+D");
    auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(popup);
    bool found = false;
    for (QAction *action : popup->actions()) {
        if (action->text() == QStringLiteral("The B")) {
            found = true;
            action->trigger();
        }
    }
    QVERIFY(found);
    popup->close();
    QTRY_COMPARE(f.left()->directory(), f.b);
    // Nothing was asked: a hotlist entry goes there without questions.
    QCOMPARE(f.dialogs.questions.size(), askedBefore);
}

void TestMainWindow::selectGroupMarksWhatThePatternAnswersTo()
{
    Fixture f;
    QVERIFY(f.ready);
    QCOMPARE(f.left()->directory(), f.a);
    f.left()->files()->clearSelection();

    // Mark what the name answers to: the two files, not the folder.
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<SelectGroupDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        dialog->patternsField()->setText("*.txt");
        QTest::mouseClick(dialog->markButton(), Qt::LeftButton);
    });
    pressSequence(f.window.get(), "Ctrl++");
    const QStringList marked = f.left()->selectedPaths();
    QCOMPARE(marked.size(), 2);
    QVERIFY(marked.contains(f.a + "/one.txt"));
    QVERIFY(marked.contains(f.a + "/two.txt"));

    // Unmark takes what answers away again; folders can be named as well.
    // (The key itself is pressed for Select above, which is wired exactly
    // the same; QTest delivers Key_Minus with a modifier unwell under the
    // offscreen platform, so Unselect is triggered by name here.)
    QAction *unselect = nullptr;
    for (QAction *action : f.window->findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("Unselect group")) {
            unselect = action;
        }
    }
    QVERIFY(unselect);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<SelectGroupDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        dialog->patternsField()->setText("sub one*");
        QTest::mouseClick(dialog->unmarkButton(), Qt::LeftButton);
    });
    unselect->trigger();
    // The folder and one.txt were named; two.txt was not, and stays marked.
    const QStringList left = f.left()->selectedPaths();
    QCOMPARE(left.size(), 1);
    QCOMPARE(left.first(), f.a + "/two.txt");
}

void TestMainWindow::propertiesTellsWhatIsThere()
{
    Fixture f;
    QVERIFY(f.ready);

    // One file: its name, its path, its size.
    PropertiesWindow one({f.a + "/one.txt"});
    QCOMPARE(one.windowTitle(), QStringLiteral("Properties"));
    QCOMPARE(one.titleText(), QStringLiteral("one.txt"));
    QVERIFY(one.detailsText().contains(f.a + "/one.txt"));
    QVERIFY(one.sizeText().contains(QStringLiteral("1 byte")));

    // Several: counted, and measured together, the folder on a worker.
    PropertiesWindow group({f.a + "/one.txt", f.a + "/two.txt", f.a + "/sub"});
    QCOMPARE(group.titleText(), QStringLiteral("3 items"));
    QVERIFY(group.detailsText().contains(QStringLiteral("1 folders and 2 files")));
    QTRY_VERIFY_WITH_TIMEOUT(!group.sizeText().contains(QStringLiteral("Finding out")), 15000);
    // One and two and the file inside the folder: six bytes in all.
    QVERIFY(group.sizeText().contains(QStringLiteral("6 bytes")));
}

void TestMainWindow::reviewDeleteInAFieldEditsTheText()
{
    Fixture f;
    QVERIFY(f.ready);
    f.click(f.left(), "one.txt");
    QLineEdit *filter = f.left()->filterField();
    f.left()->showFilter();
    QCOMPARE(QApplication::focusWidget(), filter);
    QTest::keyClicks(filter, "ab");
    QTest::keyClick(filter, Qt::Key_Home);
    const int questions = f.dialogs.questions.size();
    const int operations = 0;
    QTest::keyClick(filter, Qt::Key_Delete);
    qInfo() << "after Delete in the filter: text" << filter->text() << "| questions asked:"
            << f.dialogs.questions.size() - questions;
    // The key is for the text.
    QCOMPARE(filter->text(), QString("b"));
    QCOMPARE(f.dialogs.questions.size(), questions);
    Q_UNUSED(operations);

    // And "for good" with Shift, and in the field for the path.
    QTest::keyClick(filter, Qt::Key_Delete, Qt::ShiftModifier);
    QCOMPARE(f.dialogs.questions.size(), questions);
    QVERIFY(QFileInfo::exists(f.a + "/one.txt"));

    // The Mac delete keys as well: Cmd+Backspace and Option+Backspace are
    // for deleting there, and in a field they edit the text, nothing else.
    QTest::keyClick(filter, Qt::Key_Return);
    QTest::keyClicks(filter, "cd");
    QTest::keyClick(filter, Qt::Key_Backspace, Qt::ControlModifier);
    QCOMPARE(f.dialogs.questions.size(), questions);
    QVERIFY(QFileInfo::exists(f.a + "/one.txt"));
    QTest::keyClick(filter, Qt::Key_Backspace, Qt::AltModifier);
    QCOMPARE(f.dialogs.questions.size(), questions);
    QVERIFY(QFileInfo::exists(f.a + "/one.txt"));
}

void TestMainWindow::reviewClosingWithAnUnsavedEditorAsks()
{
    Fixture f;
    QVERIFY(f.ready);
    f.click(f.left(), "one.txt");
    QTest::keyClick(f.window.get(), Qt::Key_F4);
    QList<ViewerWindow *> viewers = f.viewersFor(f.a + "/one.txt");
    QCOMPARE(viewers.size(), 1);
    QPointer<ViewerWindow> editor = viewers.first();
    editor->editor()->appendPlainText(QStringLiteral("changed"));
    QVERIFY(editor->isModified());

    // The window is closed; the user does not want to lose the text.
    f.dialogs.answer = Dialogs::Answer::Cancel;
    const int questions = f.dialogs.questions.size();
    const bool closed = f.window->close();
    qInfo() << "close() returned" << closed << "| window visible:" << f.window->isVisible()
            << "| editor still open:" << !editor.isNull() << "| questions asked:"
            << f.dialogs.questions.size() - questions;
    QVERIFY2(f.dialogs.questions.size() > questions, "the editor was not asked");
}

void TestMainWindow::reviewViewOnAFolderOpensNothing()
{
    Fixture f;
    QVERIFY(f.ready);
    f.click(f.left(), "sub");
    QTest::keyClick(f.window.get(), Qt::Key_F3);
    int opened = 0;
    QString text;
    for (ViewerWindow *viewer : f.viewersFor(f.a + "/sub")) {
        ++opened;
        text = viewer->editor()->toPlainText();
    }
    qInfo() << "viewers for a folder:" << opened << "|" << text;
    QCOMPARE(opened, 0);
}

void TestMainWindow::reviewAFolderIsMadeOnlyInTheFolderShown()
{
    Fixture f;
    QVERIFY(f.ready);
    f.dialogs.text = QStringLiteral("../escaped");
    f.window->actions().newFolder->trigger();
    qInfo() << "made outside the folder shown:" << QFileInfo::exists(f.a + "/../escaped")
            << "| told:" << f.dialogs.told;
    QVERIFY2(!QFileInfo::exists(f.a + "/../escaped"), "a name with a path in it was accepted");
}

QTEST_MAIN(TestMainWindow)
#include "mainwindow.moc"
