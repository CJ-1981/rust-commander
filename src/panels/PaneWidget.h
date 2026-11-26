#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QWidget>

class QAction;
class Dialogs;
class FileModel;
class FileProxyModel;
class FileView;
class QFileSystemWatcher;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QModelIndex;
class OperationRunner;
class PathBar;
class QTabBar;
class QTimer;

// One side of the window: a row of folder tabs, the path, buttons for what
// can be done without function keys, the file table and a status line.
class PaneWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(bool active READ isActive WRITE setActive)

public:
    PaneWidget(Dialogs &dialogs, OperationRunner &runner, QWidget *parent = nullptr);

    // The folder shown in the current tab.
    QString directory() const;
    // Paths of the selected items, top to bottom.
    QStringList selectedPaths() const;
    // The item under the cursor; if there is none, the first selected one.
    QString cursorPath() const;
    bool isActive() const { return m_active; }
    // The folders of the tabs, for the next start.
    QStringList tabPaths() const;
    bool canGoBack() const;
    bool canGoForward() const;
    FileView *files() const { return m_view; }
    QLineEdit *filterField() const { return m_filter; }
    QLabel *statusLabel() const { return m_status; }

    // The actions the menus and shortcuts stand for; they are shown as
    // buttons and in the context menu, and act on this pane.
    void setActions(const QList<QAction *> &actions);
    // Marks the pane as the one the shortcuts act on.
    void setActive(bool active);
    // Moves the keyboard focus to the file table.
    void setFocusToFiles();

    // Tabs
    void addTab(const QString &path = {});
    void closeTab(int index);

    // Going places
    // A folder; a file is selected in its folder.
    void navigateTo(const QString &path);
    void goBack();
    void goForward();
    void goUp();
    void goHome();
    void refresh();

    // Doing things, on the selection and the cursor of this pane
    void openCursor();
    void openTerminalHere();
    void newFolder();
    void newFile();
    void renameCursor();
    // For good with `permanently`, to the Trash otherwise; after asking.
    void deleteSelected(bool permanently);
    void selectAll();
    void toggleHidden();
    void toggleFolderSizes();
    void showFilter();
    void copyTo(const QString &targetFolder);
    void moveTo(const QString &targetFolder);
    // To the other pane, which decides where that is.
    void copyToOtherPane();
    void moveToOtherPane();

signals:
    // The user worked in this pane.
    void activated(PaneWidget *pane);
    void transferRequested(const QStringList &paths, bool move);
    void viewRequested(const QString &path);
    void editRequested(const QString &path);
    // The folder, the selection or the tabs changed.
    void stateChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Tab {
        QString path;         // the folder shown
        QString title;        // when the user gave the tab a name
        QStringList history;  // where the tab has been, for Back and Forward
        int position = -1;
    };

    // What was selected in a folder, to find it again on the way back.
    struct Selection {
        QString cursor;
        QStringList selected;
    };

    void buildUi();
    Tab &tab();
    const Tab &tab() const;
    QString tabTitle(int index) const;
    // Shows the current tab's folder: path bar, listing and the folder watched for changes.
    void loadTab();
    // Shows `folder` in the current tab, adding it to the tab's history.
    void showFolder(const QString &folder);
    // Selects what was asked for, or what was selected in this folder before.
    void restoreSelection(const QString &folder);
    void selectOnly(const QString &path);
    void selectOnly(const QModelIndex &index);
    void rememberSelection();
    // The name to offer for something new: `base`, or "base 2", "base 3", ...
    QString unusedName(const QString &base) const;
    void renameTab(int index);
    void dropped(const QList<QUrl> &urls, const QString &targetFolder, Qt::DropAction action,
                 bool ask);
    void showContextMenu(const QPoint &position);
    void updateStatus();

    Dialogs &m_dialogs;
    OperationRunner &m_runner;
    FileModel *m_model;
    FileProxyModel *m_proxy;
    FileView *m_view;
    QTabBar *m_tabs;
    PathBar *m_path;
    QLineEdit *m_filter;
    QHBoxLayout *m_actionLayout = nullptr;
    QLabel *m_status;
    QFileSystemWatcher *m_watcher;
    QTimer *m_changeTimer;  // changes come in bursts; one refresh is enough
    QList<Tab> m_tabList;
    QList<QAction *> m_actions;  // the window's, for the buttons and the context menu
    QAction *m_openAction;
    QAction *m_terminalAction;
    QHash<QString, Selection> m_remembered;
    QString m_selectNext;  // what to select once the listing is there
    int m_lastCursorRow = -1;
    bool m_reloading = false;
    bool m_active = false;
    bool m_folderSizes = false;
};
