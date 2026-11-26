#pragma once

#include <QMainWindow>
#include <QStringList>

class QAction;
class QActionGroup;
class QLabel;
class QMenu;
class QSplitter;
class Dialogs;
class OperationRunner;
class PaneWidget;

// The application window: two file panes side by side, and menus and
// shortcuts, which act on the pane that was used last.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // The actions the menus and shortcuts stand for.
    struct Actions {
        QAction *open = nullptr;
        QAction *view = nullptr;
        QAction *edit = nullptr;
        QAction *newFolder = nullptr;
        QAction *newFile = nullptr;
        QAction *rename = nullptr;
        QAction *copy = nullptr;  // to the other pane
        QAction *move = nullptr;  // to the other pane
        QAction *trash = nullptr;
        QAction *erase = nullptr;  // for good
        QAction *terminal = nullptr;

        QAction *back = nullptr;
        QAction *forward = nullptr;
        QAction *up = nullptr;
        QAction *home = nullptr;
        QAction *typePath = nullptr;
        QAction *refresh = nullptr;
        QAction *filter = nullptr;
        QAction *selectAll = nullptr;
        QAction *hidden = nullptr;
        QAction *folderSizes = nullptr;

        QAction *newTab = nullptr;
        QAction *nextTab = nullptr;
        QAction *previousTab = nullptr;
        QAction *closeTab = nullptr;
        QAction *swapPanes = nullptr;
        QAction *addFavorite = nullptr;
        QAction *about = nullptr;
    };

    // `dialogs` gets the questions; without it, window dialogs are shown.
    explicit MainWindow(QWidget *parent = nullptr, Dialogs *dialogs = nullptr);
    ~MainWindow() override;

    PaneWidget *leftPane() const { return m_left; }
    PaneWidget *rightPane() const { return m_right; }
    // The pane the shortcuts act on: the one that was used last.
    PaneWidget *activePane() const { return m_active; }
    const Actions &actions() const { return m_actions; }
    QMenu *volumesMenu() const { return m_volumes; }
    QMenu *favoritesMenu() const { return m_favorites; }
    QActionGroup *themeActions() const { return m_theme; }
    QSplitter *splitter() const { return m_splitter; }

protected:
    void closeEvent(QCloseEvent *event) override;
    // Watches for Tab, which goes to the other pane unless a field is being typed in.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    PaneWidget *addPane();
    void createActions();
    void createMenus();
    void createKeyRow();
    void setActivePane(PaneWidget *pane);
    void switchPane(bool backwards);
    void transfer(PaneWidget *from, const QStringList &paths, bool move);
    // Enables what the active pane can do, and marks what is on there.
    void refreshActions();
    void showViewer(const QString &path, bool edit);
    void fillVolumes();
    void fillFavorites();
    void removeFavorite(const QString &path);

    QSplitter *m_splitter = nullptr;
    PaneWidget *m_left = nullptr;
    PaneWidget *m_right = nullptr;
    PaneWidget *m_active = nullptr;
    Dialogs *m_dialogs = nullptr;
    bool m_ownsDialogs = false;
    OperationRunner *m_runner = nullptr;
    Actions m_actions;
    QMenu *m_volumes = nullptr;
    QMenu *m_favorites = nullptr;
    QActionGroup *m_theme = nullptr;
    QWidget *m_keyRow = nullptr;
    bool m_dividerJustReset = false;  // the release after a double-click on the divider is swallowed
    QAction *m_quit = nullptr;
    QLabel *m_version = nullptr;
};
