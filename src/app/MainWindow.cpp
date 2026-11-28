#include "MainWindow.h"

#include "AboutDialog.h"
#include "CommandLine.h"
#include "Dialogs.h"
#include "Icons.h"
#include "Settings.h"
#include "Theme.h"
#include "ops/OperationRunner.h"
#include "ops/Shell.h"
#include "panels/FileModel.h"
#include "panels/FileProxyModel.h"
#include "panels/SelectGroupDialog.h"
#include "panels/FileView.h"
#include "panels/PaneWidget.h"
#include "tools/ChecksumWindow.h"
#include "tools/CompareWindow.h"
#include "tools/SearchWindow.h"
#include "tools/DuplicatesWindow.h"
#include "tools/MultiRenameWindow.h"
#include "tools/PropertiesWindow.h"
#include "viewer/ViewerWindow.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>

#include <algorithm>
#include <QItemSelectionModel>
#include <QProcess>
#include <QRegularExpression>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStatusBar>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("MainWindow", text);
}

// Where Tab means "next field" rather than "other pane".
bool typesText(QWidget *widget)
{
    return qobject_cast<QLineEdit *>(widget) || qobject_cast<QPlainTextEdit *>(widget)
           || qobject_cast<QTextEdit *>(widget);
}

// The folders a pane starts with: where its tabs were, or the home folder.
QStringList startFolders(int pane)
{
    QStringList paths = Settings::tabPaths(pane);
    if (paths.isEmpty()) {
        paths = {QDir::homePath()};
    }
    return paths;
}

QKeySequence key(const char *keys)
{
    return QKeySequence(QLatin1String(keys));
}

}  // namespace

MainWindow::MainWindow(QWidget *parent, Dialogs *dialogs)
    : QMainWindow(parent)
    , m_dialogs(dialogs ? dialogs : new GuiDialogs(this))
    , m_ownsDialogs(dialogs == nullptr)
    , m_runner(new OperationRunner(*m_dialogs, this))
{
    setWindowTitle(tr("Rust Commander %1").arg(QLatin1String(APP_VERSION)));
    resize(1180, 720);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(7);
    m_left = addPane();
    m_right = addPane();

    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_splitter, 1);
    createActions();
    createMenus();
    createCommandLine();
    createKeyRow();
    layout->addWidget(m_keyRow);
    setCentralWidget(central);
    statusBar()->hide();

    // A double-click on the divider gives both panes the same width.
    m_splitter->handle(1)->installEventFilter(this);
    // Tab goes to the other pane, wherever in the window it is pressed.
    qApp->installEventFilter(this);

    // After a copy, a move or a delete, both panes show what is there now.
    connect(m_runner, &OperationRunner::finished, this, [this] {
        m_left->refresh();
        m_right->refresh();
    });

    for (const QString &path : startFolders(0)) {
        m_left->addTab(path);
    }
    for (const QString &path : startFolders(1)) {
        m_right->addTab(path);
    }
    const QByteArray divider = Settings::restoreLayout(this);
    if (!divider.isEmpty()) {
        m_splitter->restoreState(divider);
    } else {
        m_splitter->setSizes({1, 1});
    }
    // The keys go to the file list, not to a button.
    m_left->setFocusToFiles();
    setActivePane(m_left);
}

MainWindow::~MainWindow() = default;

PaneWidget *MainWindow::addPane()
{
    auto *pane = new PaneWidget(*m_dialogs, *m_runner, m_splitter);
    m_splitter->addWidget(pane);
    connect(pane, &PaneWidget::activated, this, &MainWindow::setActivePane);
    connect(pane, &PaneWidget::stateChanged, this, &MainWindow::refreshActions);
    connect(pane, &PaneWidget::transferRequested, this,
            [this, pane](const QStringList &paths, bool move) { transfer(pane, paths, move); });
    connect(pane, &PaneWidget::viewRequested, this,
            [this](const QString &path) { showViewer(path, false); });
    connect(pane, &PaneWidget::editRequested, this,
            [this](const QString &path) { showViewer(path, true); });
    return pane;
}

void MainWindow::createActions()
{
    Actions &a = m_actions;
    // An action is made with the name of its icon, which stays with it: that
    // is how the icons find their way when the theme changes.
    const auto make = [this](QAction **slot, const char *icon, const QString &text,
                             const QList<QKeySequence> &keys, auto handler) {
        QAction *action = new QAction(text, this);
        Icons::setIcon(action, QLatin1String(icon));
        action->setShortcuts(keys);
        action->setToolTip(keys.isEmpty()
                                   ? text
                                   : QStringLiteral("%1 (%2)").arg(text, keys.first().toString()));
        connect(action, &QAction::triggered, this, handler);
        if (slot) {
            *slot = action;
        }
        return action;
    };
    const auto pane = [this]() -> PaneWidget * { return m_active; };

    // The Total Commander keys; the function keys are shown along the bottom.
    make(&a.open, "folder-open", tr("Open"), {}, [pane] { pane()->openCursor(); });
    make(&a.view, "eye", tr("View"), {key("F3")}, [this, pane] { showViewer(pane()->cursorPath(), false); });
    make(&a.edit, "square-pen", tr("Edit"), {key("F4")}, [this, pane] { showViewer(pane()->cursorPath(), true); });
    make(&a.newFolder, "folder-plus", tr("New folder"), {key("F7")}, [pane] { pane()->newFolder(); });
    make(&a.newFile, "file-plus", tr("New file"), {key("Ctrl+N")}, [pane] { pane()->newFile(); });
    make(&a.rename, "text-cursor-input", tr("Rename"), {key("Shift+F6"), key("F2")},
         [pane] { pane()->renameCursor(); });
    make(&a.copy, "copy", tr("Copy"), {key("F5")}, [pane] { pane()->copyToOtherPane(); });
    a.copy->setToolTip(tr("Copy the selected items to the other pane (F5)"));
    make(&a.move, "folder-input", tr("Move"), {key("F6")}, [pane] { pane()->moveToOtherPane(); });
    a.move->setToolTip(tr("Move the selected items to the other pane (F6)"));
    QList<QKeySequence> trashKeys = {key("F8"), key("Del")};
    QList<QKeySequence> eraseKeys = {key("Shift+F8"), key("Shift+Del")};
#ifdef Q_OS_MACOS
    // What a Mac keyboard has: Cmd+Backspace moves to the Trash as Finder
    // does, Option+Backspace is free here. The Del of the first two keys is
    // fn+Backspace there, which is no way to work.
    trashKeys += {key("Ctrl+Backspace"), key("Alt+Backspace")};
    eraseKeys += {key("Ctrl+Shift+Backspace")};
#endif
    make(&a.trash, "trash", tr("Delete"), trashKeys, [pane] { pane()->deleteSelected(false); });
    a.trash->setToolTip(tr("Move the selected items to the Trash (F8)"));
    make(&a.erase, "eraser", tr("Delete for good"), eraseKeys,
         [pane] { pane()->deleteSelected(true); });
    a.erase->setToolTip(tr("Delete the selected items for good (Shift+F8)"));
    make(&a.terminal, "terminal", tr("Open terminal here"), {key("Ctrl+Alt+T")},
         [pane] { pane()->openTerminalHere(); });

    // The tools act on this pane's selection and folder.
    make(&a.multiRename, "square-pen", tr("Multi-rename"), {key("Ctrl+M")}, [this, pane] {
        QStringList files = pane()->selectedPaths();
        if (files.isEmpty() && !pane()->cursorPath().isEmpty()) {
            files = {pane()->cursorPath()};
        }
        MultiRenameWindow::open(files, *m_dialogs, this);
    });
    make(&a.duplicates, "copy", tr("Find duplicate files"), {key("Ctrl+Shift+D")}, [this, pane] {
        DuplicatesWindow::open(pane()->directory(), *m_dialogs, this);
    });
    make(&a.search, "search", tr("Search files"), {key("Ctrl+F3")}, [this, pane] {
        SearchWindow::open(pane()->directory(), *m_dialogs, this);
    });
    make(&a.checksum, "list-checks", tr("Checksums"), {}, [this, pane] {
        // The file under the cursor is hashed, or checked against a checksum
        // file when it is one.
        ChecksumWindow::open(pane()->cursorPath().isEmpty() ? pane()->directory()
                                                            : pane()->cursorPath(),
                             *m_dialogs, this);
    });
    // Two selected files are compared; with less chosen, the window opens
    // with what there is and the sides are filled in there.
    make(&a.compare, "git-compare", tr("Compare files"), {}, [this, pane] {
        const QStringList files = pane()->selectedPaths();
        CompareWindow::open(files.value(0), files.value(1), *m_dialogs, this);
    });

    a.commandLine = make(nullptr, "terminal", tr("Command line"), {key("Ctrl+K")},
                         [this] {
                             m_commandLine->setFocus(Qt::OtherFocusReason);
                             m_commandLine->setHistory(Settings::commandHistory());
                         });
    make(&a.back, "arrow-left", tr("Back"), {key("Alt+Left")}, [pane] { pane()->goBack(); });
    make(&a.forward, "arrow-right", tr("Forward"), {key("Alt+Right")}, [pane] { pane()->goForward(); });
    make(&a.up, "arrow-up", tr("Up"), {key("Alt+Up")}, [pane] { pane()->goUp(); });
    make(&a.home, "house", tr("Home"), {key("Alt+Home")}, [pane] { pane()->goHome(); });
    make(&a.typePath, "corner-down-left", tr("Type a path"), {key("Ctrl+G")}, [pane] { pane()->typePath(); });
    make(&a.selectGroup, "list-checks", tr("Select group"), {key("Ctrl++"), key("Ctrl+=")},
         [this] { selectGroup(true); });
    make(&a.unselectGroup, "list-checks", tr("Unselect group"), {key("Ctrl+-")},
         [this] { selectGroup(false); });
    make(&a.properties, "info", tr("Properties"),
         {key("Alt+Enter"),
#ifdef Q_OS_MACOS
          key("Ctrl+I")
#endif
         },
         [this, pane] {
             QStringList paths = pane()->selectedPaths();
             if (paths.isEmpty() && !pane()->cursorPath().isEmpty()) {
                 paths = {pane()->cursorPath()};
             }
             auto *properties = new PropertiesWindow(paths, this);
             properties->setAttribute(Qt::WA_DeleteOnClose);
             properties->show();
         });
    make(&a.refresh, "refresh-cw", tr("Refresh"), {key("Ctrl+R")}, [pane] { pane()->refresh(); });
    make(&a.filter, "list-filter", tr("Filter"), {key("Ctrl+F")}, [pane] { pane()->showFilter(); });
    make(&a.selectAll, "list-checks", tr("Select all"), {key("Ctrl+A")}, [pane] { pane()->selectAll(); });

#ifdef Q_OS_MACOS
    const char *hiddenKeys = "Ctrl+Shift+H";  // Cmd+H hides the app there
#else
    const char *hiddenKeys = "Ctrl+H";
#endif
    // An open eye once hidden files are shown; refreshActions() swaps them.
    a.hidden = make(nullptr, "eye-off", tr("Show hidden files"), {key(hiddenKeys)},
                    [pane] { pane()->toggleHidden(); });
    a.hidden->setCheckable(true);
    a.folderSizes = make(nullptr, "folder-tree", tr("Show folder sizes"), {key("Ctrl+L")},
                         [pane] { pane()->toggleFolderSizes(); });
    a.folderSizes->setCheckable(true);

    make(&a.newTab, "plus", tr("New tab"), {key("Ctrl+T")}, [pane] { pane()->addTab(); });
#ifdef Q_OS_MACOS
    // Cmd+Tab and Cmd+Shift+Tab switch applications and windows there.
    const char *nextTabKeys = "Ctrl+Alt+Right";
    const char *previousTabKeys = "Ctrl+Alt+Left";
#else
    const char *nextTabKeys = "Ctrl+Tab";
    const char *previousTabKeys = "Ctrl+Shift+Tab";
#endif
    make(&a.nextTab, "chevrons-right", tr("Next tab"), {key(nextTabKeys)}, [pane] { pane()->nextTab(); });
    make(&a.previousTab, "chevrons-left", tr("Previous tab"), {key(previousTabKeys)},
         [pane] { pane()->previousTab(); });
    make(&a.closeTab, "x", tr("Close tab"), {key("Ctrl+W")}, [pane] { pane()->closeCurrentTab(); });
    make(&a.swapPanes, "arrow-left-right", tr("Swap panes"), {key("Ctrl+U")}, [this] {
        // The first pane goes to the end, which makes it the right one.
        m_splitter->addWidget(m_splitter->widget(0));
    });
    // Ctrl+D is Total Commander's hotlist: the list of folders, from where
    // the active pane jumps to one. Adding a folder is in the list itself.
    make(&a.hotlist, "star", tr("Directory hotlist"), {key("Ctrl+D")}, [this] {
        m_favorites->popup(m_active->mapToGlobal(QPoint(6, 0)));
    });
    make(&a.addFavorite, "star", tr("Add this folder to the favorites"), {}, [this] {
        const QString folder = m_active->directory();
        if (folder.isEmpty()) {
            return;
        }
        const QVector<Settings::Bookmark> bookmarks = Settings::bookmarks();
        for (const Settings::Bookmark &bookmark : bookmarks) {
            if (bookmark.path == folder) {
                return;
            }
        }
        QVector<Settings::Bookmark> updated = bookmarks;
        Settings::Bookmark added;
        added.name = QDir(folder).dirName();
        added.path = folder;
        updated.append(added);
        Settings::setBookmarks(updated);
    });
    make(&a.about, "info", tr("About Rust Commander"), {}, [this] { AboutDialog(this).exec(); });

    m_quit = new QAction(tr("Quit"), this);
    Icons::setIcon(m_quit, "log-out");
    m_quit->setMenuRole(QAction::QuitRole);
    m_quit->setShortcuts({QKeySequence(QKeySequence::Quit), key("Ctrl+Q")});
    connect(m_quit, &QAction::triggered, this, &QWidget::close);

    // What the panes' context menus offer, besides their own.
    const QList<QAction *> menuActions = {a.view,    a.edit,     a.rename,  a.copy,    a.move,
                                          a.trash,  a.erase,    a.newFolder, a.newFile, a.selectAll,
                                          a.properties};
    m_left->setMenuActions(menuActions);
    m_right->setMenuActions(menuActions);

    // What each pane's row of buttons holds, with what of that pane says
    // whether there is something for the button to do.
    const QList<PaneWidget::Button> row = {
        {a.back, &PaneWidget::canGoBack},
        {a.forward, &PaneWidget::canGoForward},
        {a.up, nullptr},
        {a.home, nullptr},
        {a.refresh, nullptr},
        {},  // a gap between the groups
        {a.newFolder, nullptr},
        {a.newFile, nullptr},
        {a.rename, &PaneWidget::hasCursor},
        {a.copy, &PaneWidget::hasSelection},
        {a.move, &PaneWidget::hasSelection},
        {a.trash, &PaneWidget::hasSelection},
        {},
        {a.hidden, &PaneWidget::showsHidden},
        {a.folderSizes, &PaneWidget::showsFolderSizes},
    };
    m_left->setActions(row);
    m_right->setActions(row);
}

void MainWindow::createMenus()
{
    const Actions &a = m_actions;
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(a.open);
    file->addAction(a.view);
    file->addAction(a.edit);
    file->addSeparator();
    file->addAction(a.newFolder);
    file->addAction(a.newFile);
    file->addAction(a.rename);
    file->addSeparator();
    file->addAction(a.copy);
    file->addAction(a.move);
    file->addAction(a.trash);
    file->addAction(a.erase);
    file->addSeparator();
    file->addAction(a.terminal);
    file->addSeparator();
    file->addAction(a.search);
    file->addAction(a.multiRename);
    file->addAction(a.duplicates);
    file->addAction(a.checksum);
    file->addAction(a.compare);
    file->addSeparator();
    file->addAction(a.selectGroup);
    file->addAction(a.unselectGroup);
    file->addAction(a.properties);
    file->addSeparator();
    file->addAction(a.closeTab);
    file->addAction(m_quit);

    QMenu *go = menuBar()->addMenu(tr("&Go"));
    go->addAction(a.back);
    go->addAction(a.forward);
    go->addAction(a.up);
    go->addAction(a.home);
    go->addAction(a.typePath);
    go->addSeparator();
    m_volumes = go->addMenu(tr("&Volumes"));
    Icons::setIcon(m_volumes->menuAction(), "hard-drive");
    connect(m_volumes, &QMenu::aboutToShow, this, &MainWindow::fillVolumes);
    m_favorites = go->addMenu(tr("F&avorites"));
    connect(m_favorites, &QMenu::aboutToShow, this, &MainWindow::fillFavorites);
    go->addAction(a.hotlist);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(a.refresh);
    view->addAction(a.filter);
    view->addAction(a.selectAll);
    view->addSeparator();
    view->addAction(a.hidden);
    view->addAction(a.folderSizes);
    view->addSeparator();
    view->addAction(a.newTab);
    view->addAction(a.nextTab);
    view->addAction(a.previousTab);
    view->addAction(a.swapPanes);

    QMenu *theme = view->addMenu(tr("&Theme"));
    m_theme = new QActionGroup(this);
    for (Theme::Mode mode : {Theme::Mode::System, Theme::Mode::Light, Theme::Mode::Dark}) {
        const QString name = mode == Theme::Mode::System  ? tr("Like the system")
                             : mode == Theme::Mode::Light ? tr("Light")
                                                          : tr("Dark");
        QAction *action = m_theme->addAction(name);
        action->setCheckable(true);
        action->setData(Theme::modeName(mode));
    }
    for (QAction *action : m_theme->actions()) {
        action->setChecked(action->data().toString() == Theme::modeName(Settings::themeMode()));
    }
    connect(m_theme, &QActionGroup::triggered, this, [this](QAction *action) {
        const Theme::Mode mode = Theme::modeFromName(action->data().toString(), Theme::Mode::System);
        Settings::setThemeMode(mode);
        Theme::apply(mode);
    });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(a.about);
}

void MainWindow::createKeyRow()
{
    // Along the bottom, like Total Commander's row: the function keys as
    // buttons, for anyone who would rather click.
    m_keyRow = new QWidget;
    m_keyRow->setObjectName("keyRow");
    auto *layout = new QHBoxLayout(m_keyRow);
    layout->setContentsMargins(6, 4, 6, 6);
    layout->setSpacing(4);
    const QList<QAction *> row = {m_actions.view, m_actions.edit,     m_actions.copy, m_actions.move,
                                  m_actions.newFolder, m_actions.rename, m_actions.trash, m_actions.erase};
    for (QAction *action : row) {
        // Not the action's own button: that shows the action's text, which
        // has no key in it.
        auto *button = new QToolButton;
        button->setText(action->shortcuts().first().toString() + QLatin1String("  ") + action->text());
        // The keys belong to the file list; a button that keeps focus would
        // stand between Space and the row under the cursor.
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QToolButton::clicked, action, &QAction::trigger);
        const auto follow = [button, action] {
            button->setEnabled(action->isEnabled());
            button->setToolTip(action->toolTip());
        };
        connect(action, &QAction::changed, button, follow);
        follow();
        layout->addWidget(button);
    }
    layout->addStretch(1);
    layout->addWidget(m_commandLine, 1);
    m_version = new QLabel(tr("Rust Commander %1").arg(QLatin1String(APP_VERSION)));
    layout->addWidget(m_version);
}

// What was typed is parsed against the folder shown of the active pane, and
// carried out: cd, view, edit, a file that is there, or a program to start.
void MainWindow::createCommandLine()
{
    m_commandLine = new CommandLine;
    connect(m_commandLine, &CommandLine::commandEntered, this, [this](const QString &line) {
        runCommandLine();
        Q_UNUSED(line)
    });
    connect(m_commandLine, &CommandLine::focusRequested, this,
            [this] { m_active->setFocusToFiles(); });
    // The focus of the key row belongs to the file list; the command line is
    // reached by its key, and its Esc hands the keys back.
    m_commandLine->setFocusPolicy(Qt::StrongFocus);
}

// Marks or unmarks every visible row whose name answers to any of the
// patterns the dialog holds.
void MainWindow::selectGroup(bool select)
{
    qWarning() << "PROBE selectGroup" << select;
    SelectGroupDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QStringList patterns = dialog.patterns();
    if (patterns.isEmpty()) {
        return;
    }
    const SelectGroupDialog::What what = dialog.what();
    QAbstractItemModel *list = m_active->files()->model();
    QItemSelectionModel *marks = m_active->files()->selectionModel();
    for (int row = 0; row < list->rowCount(); ++row) {
        const QModelIndex first = list->index(row, 0);
        const bool isDir = first.data(FileModel::IsDirRole).toBool();
        if ((what == SelectGroupDialog::What::Files && isDir)
            || (what == SelectGroupDialog::What::Folders && !isDir)) {
            continue;
        }
        const QString name = first.data().toString();
        const bool matches = std::any_of(patterns.cbegin(), patterns.cend(),
                                         [&name](const QString &pattern) {
                                             return QRegularExpression::fromWildcard(
                                                        pattern, Qt::CaseInsensitive)
                                                        .match(name)
                                                        .hasMatch();
                                         });
        if (!matches) {
            continue;
        }
        QItemSelection range;
        range.select(first.siblingAtColumn(0), first.siblingAtColumn(FileModel::ColumnCount - 1));
        marks->select(range, select ? QItemSelectionModel::Select : QItemSelectionModel::Deselect);
    }
}

void MainWindow::runCommandLine()
{
    const QString typed = m_commandLine->text();
    m_commandLine->clear();
    CommandLine::Command command;
    QString error;
    if (!CommandLine::parse(typed, m_active->directory(), &command, &error)) {
        if (!error.isEmpty()) {
            m_dialogs->tell(tr("Command line"), error);
        }
        return;
    }
    // Remembered for the arrow keys, with the newest first, at most 30.
    QStringList history = Settings::commandHistory();
    history.removeAll(typed);
    history.append(typed);
    while (history.size() > 30) {
        history.removeFirst();
    }
    Settings::setCommandHistory(history);

    switch (command.kind) {
    case CommandLine::Kind::Navigate:
        m_active->navigateTo(command.path);
        break;
    case CommandLine::Kind::View:
        showViewer(command.path, false);
        break;
    case CommandLine::Kind::Edit:
        showViewer(command.path, true);
        break;
    case CommandLine::Kind::Open:
        if (!Shell::open(command.path)) {
            m_dialogs->tell(tr("Command line"), tr("%1 could not be opened.").arg(command.path));
        }
        break;
    case CommandLine::Kind::Launch:
        if (!QProcess::startDetached(command.program, command.arguments)) {
            m_dialogs->tell(tr("Command line"),
                            tr("%1 could not be started.").arg(command.program));
        }
        break;
    }
    m_commandLine->setHistory(Settings::commandHistory());
    m_active->setFocusToFiles();
}

void MainWindow::setActivePane(PaneWidget *pane)
{
    if (!pane || m_active == pane) {
        return;
    }
    m_active = pane;
    m_left->setActive(pane == m_left);
    m_right->setActive(pane == m_right);
    // The active pane is where the keys go. A field someone is typing in
    // keeps its place; anything else in this window that held the focus
    // gives it to the file list, so that Space marks and Enter opens there.
    QWidget *focus = QApplication::focusWidget();
    if (!focus || (focus->window() == this && !typesText(focus))) {
        pane->setFocusToFiles();
    }
    refreshActions();
}

void MainWindow::switchPane(bool backwards)
{
    setActivePane(m_active == m_left ? m_right : m_left);
    m_active->setFocusToFiles();
}

void MainWindow::transfer(PaneWidget *from, const QStringList &paths, bool move)
{
    PaneWidget *to = from == m_left ? m_right : m_left;
    const QString target = to->directory();
    if (target.isEmpty() || paths.isEmpty()) {
        return;
    }
    if (target == from->directory()) {
        m_dialogs->tell(move ? tr("Move") : tr("Copy"), tr("Both panes show the same folder."));
        return;
    }
    if (move) {
        from->moveTo(target);
    } else {
        from->copyTo(target);
    }
}

void MainWindow::refreshActions()
{
    PaneWidget *pane = m_active;
    if (!pane) {
        return;
    }
    const bool selected = !pane->selectedPaths().isEmpty();
    const bool cursor = !pane->cursorPath().isEmpty();
    for (QAction *action : {m_actions.copy, m_actions.move, m_actions.trash, m_actions.erase}) {
        action->setEnabled(selected);
    }
    for (QAction *action : {m_actions.open, m_actions.view, m_actions.edit, m_actions.rename}) {
        action->setEnabled(cursor);
    }
    m_actions.back->setEnabled(pane->canGoBack());
    m_actions.forward->setEnabled(pane->canGoForward());
    m_actions.hidden->setChecked(pane->files()->fileModel()->showHidden());
    m_actions.folderSizes->setChecked(pane->showsFolderSizes());
    // An open eye over files that are shown, a closed one over files that are not.
    Icons::setIcon(m_actions.hidden, m_actions.hidden->isChecked() ? "eye" : "eye-off");
}

void MainWindow::showViewer(const QString &path, bool edit)
{
    // Only files: there is nothing to show as text in a folder.
    const QFileInfo info(path);
    if (info.exists() && info.isFile()) {
        ViewerWindow::open(path, edit ? ViewerWindow::Mode::Edit : ViewerWindow::Mode::View,
                           *m_dialogs, this);
    }
}

void MainWindow::fillVolumes()
{
    m_volumes->clear();
    for (const Shell::Volume &volume : Shell::volumes()) {
        QAction *action = m_volumes->addAction(volume.name);
        action->setToolTip(volume.root);
        const QString root = QDir::fromNativeSeparators(volume.root);
        connect(action, &QAction::triggered, this,
                [this, root] { m_active->navigateTo(root); });
    }
}

void MainWindow::fillFavorites()
{
    m_favorites->clear();
    m_favorites->addAction(m_actions.addFavorite);
    const QVector<Settings::Bookmark> bookmarks = Settings::bookmarks();
    if (bookmarks.isEmpty()) {
        QAction *none = m_favorites->addAction(tr("(nothing yet)"));
        none->setEnabled(false);
        return;
    }
    m_favorites->addSeparator();
    QMenu *remove = m_favorites->addMenu(tr("Remove"));
    for (const Settings::Bookmark &bookmark : bookmarks) {
        const QString path = bookmark.path;
        QAction *open = m_favorites->addAction(bookmark.name);
        open->setToolTip(path);
        connect(open, &QAction::triggered, this, [this, path] { m_active->navigateTo(path); });
        QAction *gone = remove->addAction(bookmark.name);
        connect(gone, &QAction::triggered, this, [this, path] { removeFavorite(path); });
    }
}

void MainWindow::removeFavorite(const QString &path)
{
    QVector<Settings::Bookmark> kept;
    for (const Settings::Bookmark &bookmark : Settings::bookmarks()) {
        if (bookmark.path != path) {
            kept.append(bookmark);
        }
    }
    Settings::setBookmarks(kept);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_splitter->handle(1)) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            m_splitter->setSizes({1, 1});
            m_dividerJustReset = true;
            return true;
        }
        // The release after that double-click would end a drag of the
        // divider to where the pointer is, undoing the reset.
        if (event->type() == QEvent::MouseButtonRelease && m_dividerJustReset) {
            m_dividerJustReset = false;
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            m_dividerJustReset = false;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto *press = static_cast<QKeyEvent *>(event);
        const bool tab = press->key() == Qt::Key_Tab && press->modifiers() == Qt::NoModifier;
        // Ctrl+Tab arrives as Backtab with Ctrl held: that one is a shortcut.
        const bool backwards = (press->key() == Qt::Key_Backtab && !press->modifiers().testFlag(Qt::ControlModifier)
                                && !press->modifiers().testFlag(Qt::AltModifier))
                               || (press->key() == Qt::Key_Tab
                                   && press->modifiers() == Qt::ShiftModifier);
        if (tab || backwards) {
            QWidget *focus = QApplication::focusWidget();
            if (focus && focus->window() == this && !typesText(focus)
                && !QApplication::activePopupWidget()) {
                switchPane(backwards);
                return true;
            }
            return QMainWindow::eventFilter(watched, event);
        }
        // A key that belongs to the file list, pressed while the focus sits
        // elsewhere in this window — on a button, a label, the pane chrome —
        // goes to the file list of the active pane, as in Total Commander.
        // Fields someone types in keep their keys; a view that has the focus
        // already handles its own.
        static const QList<Qt::Key> listKeys = {Qt::Key_Space,     Qt::Key_Insert,
                                                Qt::Key_Return,    Qt::Key_Enter,
                                                Qt::Key_Backspace, Qt::Key_Menu};
        QWidget *focus = QApplication::focusWidget();
        // The last check is for this very forwarding: the event handed to the
        // view comes through the filter once more, and forwarding it again
        // would go on for ever.
        if (focus && focus->window() == this && !typesText(focus)
            && !qobject_cast<FileView *>(focus) && !qobject_cast<FileView *>(watched)
            && !QApplication::activePopupWidget()) {
            const bool bare = press->modifiers() == Qt::NoModifier;
            const bool printable = bare && !press->text().isEmpty()
                                   && press->text().at(0).isPrint()
                                   && press->text().at(0).row() == 0;
            if ((bare && listKeys.contains(press->key())) || printable) {
                if (m_active && m_active->files()) {
                    FileView *view = m_active->files();
                    QKeyEvent forwarded(press->type(), press->key(), press->modifiers(),
                                        press->text(), press->isAutoRepeat(), press->count());
                    const bool taken = QApplication::sendEvent(view, &forwarded);
                    if (taken && forwarded.isAccepted()) {
                        return true;
                    }
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Editors with changes are asked first; one that stays open keeps the window.
    for (ViewerWindow *viewer : findChildren<ViewerWindow *>()) {
        if (!viewer->close()) {
            event->ignore();
            return;
        }
    }
    Settings::setTabPaths(0, m_left->tabPaths());
    Settings::setTabPaths(1, m_right->tabPaths());
    Settings::saveLayout(this, m_splitter->saveState());
    event->accept();
}
