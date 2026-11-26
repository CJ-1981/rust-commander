#include "PaneWidget.h"

#include "FileModel.h"
#include "FileProxyModel.h"
#include "FileView.h"
#include "PathBar.h"
#include "app/Dialogs.h"
#include "ops/FileOps.h"
#include "ops/OperationRunner.h"
#include "ops/Shell.h"

#include <QAction>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kChangeDelay = 150;  // ms to wait for the rest of a burst of changes

QString tr(const char *text)
{
    return QCoreApplication::translate("PaneWidget", text);
}

QString trn(const char *text, int n)
{
    return QCoreApplication::translate("PaneWidget", text, nullptr, n);
}

// A button for an action; the action's text is its tool tip.
QToolButton *buttonFor(QAction *action)
{
    auto *button = new QToolButton;
    button->setDefaultAction(action);
    button->setFocusPolicy(Qt::TabFocus);
    return button;
}

// An application or another package looks like a folder, but is started rather than opened.
bool isPackage(const QString &path)
{
    const QFileInfo info(path);
    return info.isDir() && info.isBundle();
}

}  // namespace

PaneWidget::PaneWidget(Dialogs &dialogs, OperationRunner &runner, QWidget *parent)
    : QWidget(parent)
    , m_dialogs(dialogs)
    , m_runner(runner)
    , m_model(new FileModel(this))
    , m_proxy(new FileProxyModel(this))
    , m_watcher(new QFileSystemWatcher(this))
    , m_changeTimer(new QTimer(this))
{
    m_proxy->setSourceModel(m_model);
    buildUi();

    // What other programs do to the folder shows up by itself.
    m_changeTimer->setSingleShot(true);
    m_changeTimer->setInterval(kChangeDelay);
    connect(m_changeTimer, &QTimer::timeout, this, &PaneWidget::refresh);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, m_changeTimer,
            qOverload<>(&QTimer::start));

    connect(m_model, &FileModel::listingFinished, this, [this](const QString &folder) {
        restoreSelection(folder);
        if (m_folderSizes) {
            m_model->requestFolderSizes();
        }
        updateStatus();
    });
    connect(m_model, &FileModel::listingFailed, this, &PaneWidget::updateStatus);
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, &PaneWidget::updateStatus);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, &PaneWidget::updateStatus);

    // Any click in the pane makes it the one the shortcuts act on.
    for (QWidget *child : findChildren<QWidget *>()) {
        child->installEventFilter(this);
    }
}

void PaneWidget::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The tabs
    auto *tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(0);
    m_tabs = new QTabBar;
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setExpanding(false);
    m_tabs->setElideMode(Qt::ElideMiddle);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setFocusPolicy(Qt::ClickFocus);
    tabRow->addWidget(m_tabs, 1);
    auto *add = new QToolButton;
    add->setText(QStringLiteral("+"));
    add->setToolTip(tr("New tab (Ctrl+T)"));
    add->setFocusPolicy(Qt::TabFocus);
    tabRow->addWidget(add);
    layout->addLayout(tabRow);

    // The path
    m_path = new PathBar;
    layout->addWidget(m_path);

    // The buttons
    m_openAction = new QAction(tr("Open"), this);
    m_terminalAction = new QAction(tr("Terminal"), this);
    m_terminalAction->setToolTip(tr("Open a terminal in this folder"));
    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(4, 2, 4, 2);
    buttons->setSpacing(2);
    buttons->addWidget(buttonFor(m_openAction));
    m_actionLayout = new QHBoxLayout;
    m_actionLayout->setContentsMargins(0, 0, 0, 0);
    m_actionLayout->setSpacing(2);
    buttons->addLayout(m_actionLayout);
    buttons->addWidget(buttonFor(m_terminalAction));
    buttons->addStretch(1);
    m_filter = new QLineEdit;
    m_filter->setObjectName("filter");
    m_filter->setPlaceholderText(tr("Filter (Ctrl+F)"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMinimumWidth(70);
    m_filter->setMaximumWidth(240);
    m_filter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    buttons->addWidget(m_filter, 1);
    layout->addLayout(buttons);

    // The files
    m_view = new FileView;
    m_view->setFileModel(m_proxy);
    layout->addWidget(m_view, 1);

    // The status line
    m_status = new QLabel;
    m_status->setObjectName("paneStatus");
    m_status->setContentsMargins(6, 3, 6, 4);
    layout->addWidget(m_status);

    // Tabs
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index < 0 || index >= m_tabList.size()) {
            return;
        }
        rememberSelection();
        m_selectNext.clear();
        m_lastCursorRow = -1;
        m_reloading = false;
        loadTab();
        emit stateChanged();
    });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, &PaneWidget::closeTab);
    connect(m_tabs, &QTabBar::tabBarDoubleClicked, this, &PaneWidget::renameTab);
    connect(m_tabs, &QTabBar::tabMoved, this, [this](int from, int to) {
        m_tabList.move(from, to);
        emit stateChanged();
    });
    connect(add, &QToolButton::clicked, this, [this] { addTab(); });

    // The path and the filter
    connect(m_path, &PathBar::pathChosen, this, &PaneWidget::navigateTo);
    connect(m_filter, &QLineEdit::textChanged, m_proxy, &FileProxyModel::setFilterText);

    // The files
    connect(m_view, &FileView::openRequested, this, [this](const QModelIndex &index) {
        const QString path = index.data(FileModel::PathRole).toString();
        // A package, an application mostly, is started; a folder is opened.
        if (isPackage(path)) {
            Shell::open(path);
        } else if (index.data(FileModel::IsDirRole).toBool()) {
            navigateTo(path);
        } else {
            Shell::open(path);
        }
    });
    connect(m_view, &FileView::upRequested, this, &PaneWidget::goUp);
    connect(m_view, &FileView::contextMenuRequested, this, &PaneWidget::showContextMenu);
    connect(m_view, &FileView::dropped, this, &PaneWidget::dropped);
    connect(m_view, &FileView::activated, this, [this] {
        setActive(true);
        emit activated(this);
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &PaneWidget::updateStatus);
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &PaneWidget::updateStatus);

    // The pane's own two
    connect(m_openAction, &QAction::triggered, this, &PaneWidget::openCursor);
    connect(m_terminalAction, &QAction::triggered, this, &PaneWidget::openTerminalHere);
}

QString PaneWidget::directory() const
{
    return tab().path;
}

QStringList PaneWidget::selectedPaths() const
{
    return m_view->selectedPaths();
}

QString PaneWidget::cursorPath() const
{
    return m_view->currentPath();
}

QStringList PaneWidget::tabPaths() const
{
    QStringList paths;
    for (const Tab &entry : m_tabList) {
        paths.append(entry.path);
    }
    return paths;
}

bool PaneWidget::canGoBack() const
{
    return tab().position > 0;
}

bool PaneWidget::canGoForward() const
{
    return tab().position >= 0 && tab().position + 1 < tab().history.size();
}

void PaneWidget::setActions(const QList<QAction *> &actions)
{
    m_actions = actions;
    while (QLayoutItem *item = m_actionLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    for (QAction *action : actions) {
        m_actionLayout->addWidget(buttonFor(action));
    }
}

void PaneWidget::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    // The style sheet has rules for the "active" property.
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void PaneWidget::setFocusToFiles()
{
    m_view->setFocus(Qt::OtherFocusReason);
}

bool PaneWidget::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type type = event->type();
    if (!m_active && (type == QEvent::MouseButtonPress || type == QEvent::FocusIn
                      || type == QEvent::Wheel)) {
        setActive(true);
        emit activated(this);
    }
    return QWidget::eventFilter(watched, event);
}

PaneWidget::Tab &PaneWidget::tab()
{
    static Tab nowhere;  // before there is a tab
    const int index = m_tabs->currentIndex();
    return index >= 0 && index < m_tabList.size() ? m_tabList[index] : nowhere;
}

const PaneWidget::Tab &PaneWidget::tab() const
{
    static const Tab nowhere;
    const int index = m_tabs->currentIndex();
    return index >= 0 && index < m_tabList.size() ? m_tabList.at(index) : nowhere;
}

QString PaneWidget::tabTitle(int index) const
{
    const Tab &entry = m_tabList.at(index);
    if (!entry.title.isEmpty()) {
        return entry.title;
    }
    // The folder's name; the root of a volume goes by the volume's name.
    const QString name = QDir(entry.path).dirName();
    return name.isEmpty() ? Shell::volumeName(entry.path) : name;
}

void PaneWidget::addTab(const QString &path)
{
    rememberSelection();
    Tab created;
    created.path = QDir::cleanPath(path.isEmpty() ? directory() : path);
    if (created.path.isEmpty()) {
        created.path = QDir::homePath();
    }
    // A file: its folder is shown, with the file selected.
    QFileInfo info(created.path);
    if (info.isFile()) {
        m_selectNext = created.path;
        created.path = QDir::cleanPath(info.absolutePath());
    }
    created.history = {created.path};
    created.position = 0;
    m_tabList.append(created);

    const QSignalBlocker block(m_tabs);
    const int index = m_tabs->addTab(tabTitle(m_tabList.size() - 1));
    m_tabs->setTabToolTip(index, created.path);
    m_tabs->setCurrentIndex(index);
    // currentChanged was blocked on purpose: the tab is loaded here, once.
    m_lastCursorRow = -1;
    m_reloading = false;
    loadTab();
    emit stateChanged();
}

void PaneWidget::closeTab(int index)
{
    // The last tab stays: a pane always shows a folder.
    if (index < 0 || index >= m_tabList.size() || m_tabList.size() < 2) {
        return;
    }
    const bool current = index == m_tabs->currentIndex();
    m_tabList.removeAt(index);
    {
        const QSignalBlocker block(m_tabs);
        m_tabs->removeTab(index);
    }
    if (current) {
        m_selectNext.clear();
        m_lastCursorRow = -1;
        m_reloading = false;
        loadTab();
    }
    emit stateChanged();
}

void PaneWidget::renameTab(int index)
{
    if (index < 0 || index >= m_tabList.size()) {
        return;
    }
    const std::optional<QString> title =
            m_dialogs.askText(tr("Name the tab"), tr("Name of the tab (empty: the folder's name):"),
                              m_tabList.at(index).title);
    if (!title) {
        return;
    }
    m_tabList[index].title = title->trimmed();
    m_tabs->setTabText(index, tabTitle(index));
    emit stateChanged();
}

void PaneWidget::navigateTo(const QString &path)
{
    QFileInfo info(path);
    QString target = QDir::cleanPath(info.absoluteFilePath());
    if (info.isFile()) {
        m_selectNext = target;
        target = QDir::cleanPath(info.absolutePath());
    } else if (!info.isDir()) {
        m_dialogs.tell(tr("Can't open that"),
                       tr("%1 isn't a folder, and isn't there any more.").arg(path));
        return;
    } else if (!info.isReadable()) {
        m_dialogs.tell(tr("No access"), tr("You are not allowed to open %1.").arg(target));
        return;
    }
    if (m_tabList.isEmpty()) {
        addTab(target);
        return;
    }
    showFolder(target);
}

void PaneWidget::showFolder(const QString &folder)
{
    rememberSelection();
    Tab &current = tab();
    // The same folder again only reloads it.
    if (current.position < 0 || current.history.at(current.position) != folder) {
        // There is no way forward from here any more.
        while (current.history.size() > current.position + 1) {
            current.history.removeLast();
        }
        current.history.append(folder);
        current.position = current.history.size() - 1;
    }
    current.path = folder;
    m_lastCursorRow = -1;
    m_reloading = false;
    loadTab();
    emit stateChanged();
}

void PaneWidget::loadTab()
{
    const QString folder = directory();
    m_path->setPath(folder);
    const int index = m_tabs->currentIndex();
    if (index >= 0 && index < m_tabList.size()) {
        m_tabs->setTabText(index, tabTitle(index));
        m_tabs->setTabToolTip(index, folder);
    }
    const QStringList watched = m_watcher->directories();
    if (!watched.isEmpty()) {
        m_watcher->removePaths(watched);
    }
    if (!folder.isEmpty()) {
        m_watcher->addPath(folder);
    }
    m_model->setPath(folder);
    updateStatus();
}

void PaneWidget::goBack()
{
    Tab &current = tab();
    if (current.position <= 0) {
        return;
    }
    rememberSelection();
    --current.position;
    current.path = current.history.at(current.position);
    m_selectNext.clear();
    m_lastCursorRow = -1;
    m_reloading = false;
    loadTab();
    emit stateChanged();
}

void PaneWidget::goForward()
{
    Tab &current = tab();
    if (current.position < 0 || current.position + 1 >= current.history.size()) {
        return;
    }
    rememberSelection();
    ++current.position;
    current.path = current.history.at(current.position);
    m_selectNext.clear();
    m_lastCursorRow = -1;
    m_reloading = false;
    loadTab();
    emit stateChanged();
}

void PaneWidget::goUp()
{
    const QString folder = directory();
    if (folder.isEmpty()) {
        return;
    }
    QDir dir(folder);
    if (!dir.cdUp()) {
        return;  // the root of the volume
    }
    m_selectNext = folder;  // the folder we came out of
    showFolder(QDir::cleanPath(dir.absolutePath()));
}

void PaneWidget::goHome()
{
    navigateTo(QDir::homePath());
}

void PaneWidget::refresh()
{
    if (directory().isEmpty()) {
        return;
    }
    m_changeTimer->stop();
    // So that the cursor ends up on the row it was on, when what it was on is gone.
    m_lastCursorRow = m_view->currentIndex().row();
    m_reloading = true;
    rememberSelection();
    m_model->reload();
}

void PaneWidget::rememberSelection()
{
    const QString folder = m_model->path();
    if (folder.isEmpty()) {
        return;
    }
    m_remembered.insert(folder, Selection{m_view->currentPath(), m_view->selectedPaths()});
}

void PaneWidget::restoreSelection(const QString &folder)
{
    // What was asked for, if it is there.
    if (!m_selectNext.isEmpty()) {
        const QString pick = m_selectNext;
        m_selectNext.clear();
        if (m_proxy->indexForPath(pick).isValid()) {
            m_reloading = false;
            m_lastCursorRow = -1;
            selectOnly(pick);
            return;
        }
    }

    // What was selected in this folder before, to the extent it still is.
    const Selection saved = m_remembered.value(folder);
    QModelIndexList found;
    for (const QString &path : saved.selected) {
        const QModelIndex index = m_proxy->indexForPath(path);
        if (index.isValid()) {
            found.append(index);
        }
    }
    if (!found.isEmpty()) {
        auto *selection = m_view->selectionModel();
        QItemSelection range;
        for (const QModelIndex &index : found) {
            range.select(index, index.siblingAtColumn(FileModel::ColumnCount - 1));
        }
        selection->select(range, QItemSelectionModel::ClearAndSelect);
        QModelIndex cursor = m_proxy->indexForPath(saved.cursor);
        if (!cursor.isValid()) {
            cursor = found.first();
        }
        selection->setCurrentIndex(cursor, QItemSelectionModel::NoUpdate);
        m_view->scrollTo(cursor);
        m_reloading = false;
        m_lastCursorRow = -1;
        return;
    }

    // Nothing is left to bring back. After a change in the folder, the cursor
    // goes to the row it was on; in a folder seen for the first time, to the first.
    int row = 0;
    if (m_reloading && m_lastCursorRow >= 0) {
        row = m_lastCursorRow;
    }
    m_reloading = false;
    m_lastCursorRow = -1;
    const int rows = m_proxy->rowCount();
    if (rows > 0) {
        selectOnly(m_proxy->index(qMin(row, rows - 1), FileModel::NameColumn));
    }
}

void PaneWidget::selectOnly(const QString &path)
{
    const QModelIndex index = m_proxy->indexForPath(path);
    if (index.isValid()) {
        selectOnly(index);
    }
}

void PaneWidget::selectOnly(const QModelIndex &index)
{
    auto *selection = m_view->selectionModel();
    selection->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_view->scrollTo(index);
}

void PaneWidget::openCursor()
{
    const QString path = cursorPath();
    if (path.isEmpty()) {
        return;
    }
    if (isPackage(path)) {
        Shell::open(path);
        return;
    }
    if (QFileInfo(path).isDir()) {
        navigateTo(path);
        return;
    }
    // Files open with their applications: all that are selected.
    for (const QString &selected : selectedPaths()) {
        if (!QFileInfo(selected).isDir()) {
            Shell::open(selected);
        }
    }
}

void PaneWidget::openTerminalHere()
{
    if (!directory().isEmpty()) {
        Shell::openTerminal(directory());
    }
}

QString PaneWidget::unusedName(const QString &base) const
{
    const QString folder = directory();
    if (!QFileInfo::exists(folder + QLatin1Char('/') + base)) {
        return base;
    }
    for (int n = 2;; ++n) {
        const QString name = QStringLiteral("%1 %2").arg(base).arg(n);
        if (!QFileInfo::exists(folder + QLatin1Char('/') + name)) {
            return name;
        }
    }
}

void PaneWidget::newFolder()
{
    const QString folder = directory();
    if (folder.isEmpty()) {
        return;
    }
    const std::optional<QString> name =
            m_dialogs.askText(tr("New folder"), tr("Name of the new folder:"), unusedName(tr("New folder")));
    if (!name || name->isEmpty()) {
        return;
    }
    QString error;
    if (!FileOps::makeFolder(folder + QLatin1Char('/') + *name, &error)) {
        m_dialogs.tell(tr("New folder"), error);
        return;
    }
    m_selectNext = folder + QLatin1Char('/') + *name;
    refresh();
}

void PaneWidget::newFile()
{
    const QString folder = directory();
    if (folder.isEmpty()) {
        return;
    }
    const std::optional<QString> name =
            m_dialogs.askText(tr("New file"), tr("Name of the new file:"), unusedName(tr("New file")));
    if (!name || name->isEmpty()) {
        return;
    }
    QString error;
    if (!FileOps::makeFile(folder + QLatin1Char('/') + *name, &error)) {
        m_dialogs.tell(tr("New file"), error);
        return;
    }
    m_selectNext = folder + QLatin1Char('/') + *name;
    refresh();
}

void PaneWidget::renameCursor()
{
    const QString path = cursorPath();
    const QString oldName = QFileInfo(path).fileName();
    if (path.isEmpty()) {
        return;
    }
    const std::optional<QString> name = m_dialogs.askText(tr("Rename"), tr("New name:"), oldName);
    if (!name || *name == oldName) {
        return;
    }
    QString error;
    if (!FileOps::rename(path, *name, &error)) {
        m_dialogs.tell(tr("Rename"), error);
        return;
    }
    m_selectNext = QDir(QFileInfo(path).absolutePath()).filePath(*name);
    refresh();
}

void PaneWidget::deleteSelected(bool permanently)
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty()) {
        return;
    }
    const int count = paths.size();
    const QString question =
            permanently ? trn("Delete %n items for good? This can't be undone.", count)
                        : trn("Move %n items to the Trash?", count);
    const Dialogs::Answer answer =
            m_dialogs.ask(permanently ? tr("Delete") : tr("To the Trash"), question);
    if (answer != Dialogs::Answer::Yes) {
        return;
    }
    if (permanently) {
        m_runner.remove(paths);
    } else {
        m_runner.trash(paths);
    }
}

void PaneWidget::selectAll()
{
    m_view->selectAll();
}

void PaneWidget::toggleHidden()
{
    m_proxy->setShowHidden(!m_proxy->showHidden());
}

void PaneWidget::toggleFolderSizes()
{
    m_folderSizes = !m_folderSizes;
    if (m_folderSizes) {
        m_model->requestFolderSizes();
    }
}

void PaneWidget::showFilter()
{
    m_filter->setFocus(Qt::OtherFocusReason);
    m_filter->selectAll();
}

void PaneWidget::copyTo(const QString &targetFolder)
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty() || targetFolder.isEmpty()) {
        return;
    }
    m_runner.copy(paths, targetFolder);
}

void PaneWidget::moveTo(const QString &targetFolder)
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty() || targetFolder.isEmpty()) {
        return;
    }
    m_runner.move(paths, targetFolder);
}

void PaneWidget::copyToOtherPane()
{
    const QStringList paths = selectedPaths();
    if (!paths.isEmpty()) {
        emit transferRequested(paths, false);
    }
}

void PaneWidget::moveToOtherPane()
{
    const QStringList paths = selectedPaths();
    if (!paths.isEmpty()) {
        emit transferRequested(paths, true);
    }
}

void PaneWidget::dropped(const QList<QUrl> &urls, const QString &targetFolder,
                         Qt::DropAction action, bool ask)
{
    QStringList paths;
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    if (paths.isEmpty()) {
        return;
    }
    // Not right away: the drag is still being wound down, and it doesn't
    // take kindly to a menu opening under it.
    const QPoint at = QCursor::pos();
    QMetaObject::invokeMethod(
            this,
            [this, paths, targetFolder, action, ask, at] {
                bool move = action == Qt::MoveAction;
                if (ask) {
                    QMenu menu(this);
                    QAction *copy = menu.addAction(tr("Copy here"));
                    QAction *moveHere = menu.addAction(tr("Move here"));
                    menu.addSeparator();
                    menu.addAction(tr("Cancel"));
                    QAction *picked = menu.exec(at);
                    if (picked != copy && picked != moveHere) {
                        return;
                    }
                    move = picked == moveHere;
                }
                if (move) {
                    m_runner.move(paths, targetFolder);
                } else {
                    m_runner.copy(paths, targetFolder);
                }
            },
            Qt::QueuedConnection);
}

void PaneWidget::showContextMenu(const QPoint &position)
{
    QMenu menu(this);
    menu.addAction(m_openAction);
    for (QAction *action : m_actions) {
        menu.addAction(action);
    }
    menu.addSeparator();
    menu.addAction(m_terminalAction);
    menu.exec(m_view->viewport()->mapToGlobal(position));
}

void PaneWidget::updateStatus()
{
    const int rows = m_proxy->rowCount();
    int folders = 0;
    for (int row = 0; row < rows; ++row) {
        folders += m_proxy->index(row, FileModel::NameColumn).data(FileModel::IsDirRole).toBool() ? 1 : 0;
    }
    const int files = rows - folders;

    QStringList parts;
    if (rows == 0) {
        parts << (m_model->isLoading() ? tr("Loading\u2026") : tr("Nothing here"));
    } else {
        if (folders > 0) {
            parts << trn("%n folders", folders);
        }
        if (files > 0) {
            parts << trn("%n files", files);
        }
    }
    if (!m_proxy->filterText().isEmpty()) {
        parts << tr("%1 of %2 shown").arg(rows).arg(m_model->rowCount());
    }

    const QStringList selected = m_view->selectedPaths();
    if (!selected.isEmpty()) {
        int selectedFolders = 0;
        int selectedFiles = 0;
        qint64 bytes = 0;
        for (const QString &path : selected) {
            const QModelIndex index = m_proxy->indexForPath(path);
            if (!index.isValid()) {
                continue;
            }
            if (index.data(FileModel::IsDirRole).toBool()) {
                ++selectedFolders;
            } else {
                ++selectedFiles;
                bytes += index.siblingAtColumn(FileModel::SizeColumn).data(FileModel::SortRole).toLongLong();
            }
        }
        QStringList what;
        if (selectedFolders > 0) {
            what << trn("%n folders", selectedFolders);
        }
        if (selectedFiles > 0) {
            what << trn("%n files", selectedFiles) + QStringLiteral(" (") + humanSize(bytes) + QLatin1Char(')');
        }
        if (!what.isEmpty()) {
            parts << tr("Selected: %1").arg(what.join(QStringLiteral(", ")));
        }
    }

    const qint64 free = Shell::freeBytes(directory());
    if (free >= 0) {
        parts << tr("%1 free").arg(humanSize(free));
    }
    m_status->setText(parts.join(QStringLiteral("   \u00b7   ")));
    emit stateChanged();
}
