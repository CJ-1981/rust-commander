#include "PaneWidget.h"

#include "FileModel.h"
#include "FileProxyModel.h"
#include "FileView.h"
#include "PathBar.h"
#include "app/Dialogs.h"
#include "app/Icons.h"
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
#include <QPainter>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kChangeDelay = 150;  // ms to wait for the rest of a burst of changes
constexpr int kIconSize = 16;      // the buttons in the row, in logical pixels

QString tr(const char *text)
{
    return QCoreApplication::translate("PaneWidget", text);
}

// "1 folder", "2 folders".
QString counted(int count, const QString &singular, const QString &plural)
{
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? singular : plural);
}

// A line of text that gives up its end, with an ellipsis, when there is no
// room. Its length must not decide how wide a pane at least has to be.
class ElidedLabel : public QLabel {
public:
    using QLabel::QLabel;

    QSize minimumSizeHint() const override
    {
        return QSize(40, QLabel::minimumSizeHint().height());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QRect box = contentsRect();
        const QString line = fontMetrics().elidedText(text(), Qt::ElideMiddle, qMax(0, box.width()));
        style()->drawItemText(&painter, box, Qt::AlignVCenter | Qt::AlignLeft, palette(), isEnabled(),
                              line, foregroundRole());
    }
};

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
    , m_mark(new QWidget)
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

    // The line over the tabs marks the pane the shortcuts act on.
    m_mark->setObjectName("paneMark");
    m_mark->setFixedHeight(2);
    m_mark->setProperty("active", false);
    layout->addWidget(m_mark);

    // The tabs
    auto *tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(0);
    m_tabs = new QTabBar;
    m_tabs->setTabsClosable(false);
    m_tabs->setMovable(true);
    m_tabs->setExpanding(false);
    m_tabs->setElideMode(Qt::ElideMiddle);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setFocusPolicy(Qt::ClickFocus);
    tabRow->addWidget(m_tabs, 1);
    auto *add = new QToolButton;
    add->setObjectName("newTabButton");
    Icons::setIcon(add, "plus");
    add->setIconSize(QSize(kIconSize, kIconSize));  // alone, a button would take the style's smaller size
    add->setToolTip(tr("New tab (Ctrl+T)"));
    add->setFocusPolicy(Qt::TabFocus);
    tabRow->addWidget(add);
    layout->addLayout(tabRow);

    // The path
    m_path = new PathBar;
    layout->addWidget(m_path);

    // The buttons. A tool bar gives what doesn't fit to a menu at its end,
    // so the row doesn't decide how wide a pane has to be.
    m_toolbar = new QToolBar;
    m_toolbar->setObjectName("paneToolBar");
    m_toolbar->setMovable(false);
    m_toolbar->setFloatable(false);
    m_toolbar->setFocusPolicy(Qt::NoFocus);  // its buttons are reached by Tab, it isn't a stop of its own
    m_toolbar->setIconSize(QSize(kIconSize, kIconSize));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    m_openAction = new QAction(tr("Open"), this);
    Icons::setIcon(m_openAction, "folder-open");
    m_terminalAction = new QAction(tr("Terminal"), this);
    m_terminalAction->setToolTip(tr("Open a terminal in this folder"));
    Icons::setIcon(m_terminalAction, "terminal");
    addButton(m_openAction, nullptr, &PaneWidget::hasCursor);
    addButton(m_terminalAction, nullptr, nullptr);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(4, 2, 4, 2);
    row->setSpacing(6);
    row->addWidget(m_toolbar, 1);
    m_filter = new QLineEdit;
    m_filter->setObjectName("filter");
    m_filter->setPlaceholderText(tr("Filter (Ctrl+F)"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMinimumWidth(110);
    m_filter->setMaximumWidth(240);
    m_filter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row->addWidget(m_filter);
    layout->addLayout(row);

    // The files
    m_view = new FileView;
    m_view->setFileModel(m_proxy);
    layout->addWidget(m_view, 1);

    // The status line
    m_status = new ElidedLabel;
    m_status->setObjectName("paneStatus");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
        if (!showsTheFoldersRows()) {
            return;  // a row of the folder before, still on screen
        }
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

// Until the listing of the folder gone to arrives, the table still shows the
// rows of the folder before. They are kept on screen so that it doesn't go
// blank, but nothing may be done to them.
bool PaneWidget::showsTheFoldersRows() const
{
    return m_model->listedPath() == directory();
}

QStringList PaneWidget::selectedPaths() const
{
    return showsTheFoldersRows() ? m_view->selectedPaths() : QStringList();
}

QString PaneWidget::cursorPath() const
{
    return showsTheFoldersRows() ? m_view->currentPath() : QString();
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

bool PaneWidget::showsHidden() const
{
    return m_proxy->showHidden();
}

void PaneWidget::setActions(const QList<Button> &buttons)
{
    // The row is made anew; this pane's versions of the window's actions go with it.
    for (const Placed &placed : m_buttons) {
        if (placed.window) {
            placed.action->deleteLater();
        }
    }
    m_toolbar->clear();
    m_buttons.clear();
    addButton(m_openAction, nullptr, &PaneWidget::hasCursor);
    addButton(m_terminalAction, nullptr, nullptr);
    if (!buttons.isEmpty()) {
        m_toolbar->addSeparator();
    }
    for (const Button &button : buttons) {
        if (button.action) {
            addButton(ownAction(button.action), button.action, button.ready);
        } else {
            m_toolbar->addSeparator();
        }
    }
    updateActions();
}

void PaneWidget::addButton(QAction *action, QAction *window, Ready ready)
{
    m_toolbar->addAction(action);
    m_buttons.append({action, window, ready});
    // The tool bar makes the button for an action when it is first asked for.
    QWidget *button = m_toolbar->widgetForAction(action);
    button->setFocusPolicy(Qt::TabFocus);
    button->installEventFilter(this);
}

QAction *PaneWidget::ownAction(QAction *action)
{
    // The same to look at as the window's action, and under the same icon, so
    // that a new theme finds this one too.
    auto *own = new QAction(this);
    own->setText(action->text());
    own->setToolTip(action->toolTip());
    own->setCheckable(action->isCheckable());
    Icons::setIcon(own, Icons::nameOf(action));
    connect(own, &QAction::triggered, this, [this, action] {
        // The window's action goes by the pane that was used last: this one, from now on.
        setActive(true);
        emit activated(this);
        action->trigger();
        updateActions();
    });
    return own;
}

QToolButton *PaneWidget::buttonFor(const QAction *action) const
{
    for (const Placed &placed : m_buttons) {
        if (placed.window == action) {
            return qobject_cast<QToolButton *>(m_toolbar->widgetForAction(placed.action));
        }
    }
    return nullptr;
}

void PaneWidget::updateActions()
{
    for (const Placed &placed : m_buttons) {
        const bool ready = placed.ready ? (this->*placed.ready)() : true;
        if (placed.action->isCheckable()) {
            placed.action->setChecked(ready);
        } else {
            placed.action->setEnabled(ready);
        }
    }
}

void PaneWidget::setMenuActions(const QList<QAction *> &actions)
{
    m_actions = actions;
}

void PaneWidget::nextTab()
{
    const int count = m_tabs->count();
    if (count > 1) {
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % count);
    }
}

void PaneWidget::previousTab()
{
    const int count = m_tabs->count();
    if (count > 1) {
        m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + count) % count);
    }
}

void PaneWidget::closeCurrentTab()
{
    closeTab(m_tabs->currentIndex());
}

void PaneWidget::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    // Only the line is restyled: the style sheet has rules for its "active"
    // property. Restyling the whole pane would reset the colors its children
    // inherit.
    m_mark->setProperty("active", active);
    m_mark->style()->unpolish(m_mark);
    m_mark->style()->polish(m_mark);
    m_mark->update();

    // The selection of the active pane is shown in full; the other pane's is fainter.
    m_view->setProperty("active", active);
    m_view->style()->unpolish(m_view);
    m_view->style()->polish(m_view);
    m_view->viewport()->update();
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
    m_tabs->setTabButton(index, QTabBar::RightSide, closeButton());
    m_tabs->setCurrentIndex(index);
    // currentChanged was blocked on purpose: the tab is loaded here, once.
    m_lastCursorRow = -1;
    m_reloading = false;
    loadTab();
    emit stateChanged();
}

// The "×" of a tab. It finds its own tab when clicked, so that moving tabs
// doesn't move the closing to another one.
QToolButton *PaneWidget::closeButton()
{
    auto *close = new QToolButton(m_tabs);
    close->setObjectName("tabClose");
    close->setText(QStringLiteral("\u00d7"));
    close->setAutoRaise(true);
    close->setFixedSize(16, 16);
    close->setFocusPolicy(Qt::NoFocus);
    close->setToolTip(tr("Close tab (Ctrl+W)"));
    connect(close, &QToolButton::clicked, this, [this, close] {
        for (int index = 0; index < m_tabs->count(); ++index) {
            if (m_tabs->tabButton(index, QTabBar::RightSide) == close) {
                closeTab(index);
                return;
            }
        }
    });
    return close;
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
    const bool elsewhere = folder != m_model->path();
    m_model->setPath(folder);
    if (elsewhere) {
        // Nothing looks selected, and so nothing looks ready to be worked on.
        m_view->selectionModel()->clear();
    }
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
    QString reason;
    if (!FileOps::isAllowedName(*name, &reason)) {
        m_dialogs.tell(tr("New folder"), reason);
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
    QString reason;
    if (!FileOps::isAllowedName(*name, &reason)) {
        m_dialogs.tell(tr("New file"), reason);
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
    const QString items = count == 1 ? tr("this item") : tr("these %1 items").arg(count);
    const QString question = permanently ? tr("Delete %1 for good? This can't be undone.").arg(items)
                                         : tr("Move %1 to the Trash?").arg(items);
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
    // A folder without hidden files lists the same as before, so nothing else would say.
    updateActions();
}

void PaneWidget::toggleFolderSizes()
{
    m_folderSizes = !m_folderSizes;
    if (m_folderSizes) {
        m_model->requestFolderSizes();
    }
    updateActions();
}

void PaneWidget::showFilter()
{
    m_filter->setFocus(Qt::OtherFocusReason);
    m_filter->selectAll();
}

void PaneWidget::typePath()
{
    m_path->edit();
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
    // What the folder, the selection and the cursor are now decides what the buttons can do.
    updateActions();

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
            parts << counted(folders, tr("folder"), tr("folders"));
        }
        if (files > 0) {
            parts << counted(files, tr("file"), tr("files"));
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
            what << counted(selectedFolders, tr("folder"), tr("folders"));
        }
        if (selectedFiles > 0) {
            what << counted(selectedFiles, tr("file"), tr("files")) + QStringLiteral(" (") + humanSize(bytes) + QLatin1Char(')');
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
