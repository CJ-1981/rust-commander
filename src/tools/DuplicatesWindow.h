#pragma once

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

#include "tools/DuplicateFinder.h"

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QSpinBox;
class QTableView;
class QFutureWatcherBase;
class Dialogs;

// The "Duplicates" window: finds the files in one or more folders that have
// the same content, and removes the spare copies of them. Scanning and
// removing run on a worker thread, so the window stays usable meanwhile.
class DuplicatesWindow : public QDialog {
    Q_OBJECT

public:
    // Opens a window for `startFolder`, which is the first folder to scan. It
    // closes itself once the user is done.
    static DuplicatesWindow *open(const QString &startFolder, Dialogs &dialogs, QWidget *parent = nullptr);

    explicit DuplicatesWindow(Dialogs &dialogs, QWidget *parent = nullptr);
    ~DuplicatesWindow() override;

    // What the next scan looks for.
    DuplicateFinder::Options options() const;
    void setOptions(const DuplicateFinder::Options &options);
    // Puts a folder on the list to scan when it is one; a file stands for its
    // folder. Nothing changes when it is on the list already.
    void addFolder(const QString &path);
    void removeFolder(const QString &path);

    // Whether a scan or a removal is running.
    bool isScanning() const;
    bool isRemoving() const;
    // What the status line says: how far a scan is, or what it found.
    QString statusText() const;

    // The groups as the list shows them: largest wasted space first, each
    // without the files that are gone by now.
    QList<DuplicateFinder::Group> groups() const;
    // The group the cursor in the list is on, or -1.
    int currentGroup() const;
    // The files whose box is checked, in the order of the list. One file of
    // every group is never among them.
    QStringList checkedPaths() const;
    // What the line at the bottom says: how many files are checked, and how
    // much they take up.
    QString totalText() const;

    // Checks or unchecks one file. False, and nothing changed, when that
    // would leave its group without a copy.
    bool setChecked(int group, int file, bool checked);
    // Checks what `rule` removes, in group `group` or, when `group` is -1,
    // in every group. What the rule keeps is unchecked.
    void mark(const DuplicateFinder::Rule &rule, int group = -1);
    // The rule chosen in the window.
    DuplicateFinder::Rule rule() const;

    // What the buttons do.
    void startScan();
    // Ends the scan at the next report; what was found by then stays on the list.
    void stopScan();
    // Asks once, then removes the checked files: to the Trash, or for good.
    // One copy of every group always stays, and so does a file that changed
    // since the scan.
    void removeChecked(bool toTrash);

    // The list of groups, and where the rule is chosen.
    QTableView *list() const;
    QComboBox *ruleBox() const { return m_rule; }

signals:
    // A scan is over, whether it ran to the end or was stopped.
    void scanFinished();
    // A removal is over.
    void removalFinished();

private:
    class List;
    class Model;
    class RowDelegate;

    // What a group hands the thread that removes its marked files.
    struct Target {
        qint64 size = 0;
        QList<DuplicateFinder::File> marked;  // as they were scanned
        QList<DuplicateFinder::File> others;  // the copies that stay
    };
    // How a removal went. It is made on the worker thread.
    struct Removal {
        QStringList removed;  // gone, or in the Trash
        QStringList gone;     // no longer there, and so off the list
        QStringList changed;  // left alone, as they are no known copies any more
        int groupsLeft = 0;   // groups where no other copy was left, so nothing was taken
        int failed = 0;
        QString error;  // why the first file couldn't be removed
    };

    // The worker thread's part of removing: a file is only removed while it
    // is still what was scanned, and while another copy of it still is.
    static Removal removeMarked(const QList<Target> &targets, bool toTrash);

    bool isBusy() const;

    void pickFolder();
    void pickRuleFolder();
    void ruleChanged();
    void removeSelectedFolder();

    void scanDone(const DuplicateFinder::Result &result);
    void removalDone(const Removal &removal);
    // Called on the GUI thread with what the worker thread reported.
    void showProgress(const DuplicateFinder::Progress &progress);
    void updateStatus();
    void updatePlaceholder();
    void updateTotals();
    void updateControls();
    // Before the list is rebuilt: where the user was.
    void rememberPlace();
    // After the list is rebuilt: the rows of the groups are one cell wide
    // again, and the user is where they were.
    void rowsChanged();

    Dialogs &m_dialogs;

    class QStringListModel *m_folderList = nullptr;
    QTableView *m_folders = nullptr;
    QAbstractButton *m_addFolder = nullptr;
    QAbstractButton *m_removeFolder = nullptr;
    QLineEdit *m_pattern = nullptr;
    QCheckBox *m_subfolders = nullptr;
    QSpinBox *m_minSize = nullptr;
    QCheckBox *m_empty = nullptr;

    QAbstractButton *m_scanButton = nullptr;
    QAbstractButton *m_stopButton = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;

    Model *m_model = nullptr;
    List *m_list = nullptr;

    QComboBox *m_rule = nullptr;
    QLabel *m_ruleFolderLabel = nullptr;
    QLineEdit *m_ruleFolder = nullptr;
    QAbstractButton *m_ruleBrowse = nullptr;
    QAbstractButton *m_markGroup = nullptr;
    QAbstractButton *m_markAll = nullptr;
    QLabel *m_total = nullptr;
    QAbstractButton *m_trash = nullptr;
    QAbstractButton *m_delete = nullptr;

    // Set to end the scan; the worker thread holds the other reference.
    std::shared_ptr<std::atomic<bool>> m_stopFlag;
    QFutureWatcherBase *m_scanWatcher = nullptr;
    QFutureWatcherBase *m_removeWatcher = nullptr;

    bool m_scanning = false;
    bool m_removing = false;
    bool m_trashing = false;  // what the running removal does
    bool m_scanned = false;
    bool m_stopped = false;
    qint64 m_filesChecked = 0;
    qint64 m_unreadable = 0;
    int m_foldersGiven = 0;
    QString m_notice;  // how the last removal went

    int m_placeScroll = 0;
    int m_placeGroup = -1;
    int m_placeFile = -1;
};
