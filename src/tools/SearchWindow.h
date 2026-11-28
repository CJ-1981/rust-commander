#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMetaMethod>
#include <QObject>
#include <QSpinBox>
#include <QStringList>
#include <QToolButton>

#include <atomic>
#include <memory>
#include <optional>

#include "FileSearch.h"

class QAbstractItemModel;
class QProgressBar;
class QModelIndex;
class QTableView;
template <typename T> class QFutureWatcher;
class QTimer;
class Dialogs;
class ResultProxy;
class ResultsModel;

// Finds files and folders by name, and on request by what is in them, by size
// and by date, starting from a folder. The search runs on a worker thread;
// what it finds arrives in batches, so that the window keeps responding even
// when there are many results.
class SearchWindow : public QDialog {
    Q_OBJECT

public:
    // Opens a window that starts from `startFolder` (a file: from its folder);
    // it deletes itself when closed.
    static SearchWindow *open(const QString &startFolder, Dialogs &dialogs, QWidget *parent = nullptr);

    explicit SearchWindow(Dialogs &dialogs, QWidget *parent = nullptr);
    ~SearchWindow() override;

    // The fields, to read and to fill in.
    QLineEdit *folderField() const { return m_folder; }
    QLineEdit *nameField() const { return m_name; }
    QLineEdit *contentField() const { return m_content; }
    QCheckBox *matchCaseBox() const { return m_matchCase; }
    QCheckBox *regexBox() const { return m_regex; }
    QToolButton *moreOptionsButton() const { return m_more; }
    // Behind "More options":
    QComboBox *lookForBox() const { return m_lookFor; }
    QCheckBox *subfoldersBox() const { return m_subfolders; }
    QCheckBox *hiddenBox() const { return m_hidden; }
    QLineEdit *minSizeField() const { return m_minSize; }
    QLineEdit *maxSizeField() const { return m_maxSize; }
    QCheckBox *modifiedBox() const { return m_modified; }
    QDateEdit *modifiedFromField() const { return m_modifiedFrom; }
    QDateEdit *modifiedToField() const { return m_modifiedTo; }
    QComboBox *contentLimitBox() const { return m_contentLimit; }
    QSpinBox *limitBox() const { return m_limit; }
    // Says "Stop" while a search runs.
    QToolButton *searchButton() const { return m_search; }

    // Whether a search is running, or just being stopped.
    bool isSearching() const { return m_watcher != nullptr; }
    // What the status line says.
    QString statusText() const { return m_status->text(); }
    // The results, top to bottom as they are listed: their paths.
    QStringList resultPaths() const;
    // What the table shows; sorted by the column its header points at.
    QAbstractItemModel *resultsModel() const;
    QTableView *resultsView() const { return m_results; }

    // Searches with what the fields say; a search that runs is told to stop
    // first. What is wrong with the fields is told, and not searched.
    void startSearch();
    // Ends the search that is running; what was found by then stays.
    void stopSearch();

signals:
    // The user asked to see `path` in the folder it is in. The main window
    // can go there; when nobody listens, the folder is opened in the
    // system's own file manager.
    void revealRequested(const QString &path);
    // A search is over, and all of it is on screen.
    void searchFinished();

private:
    // What the worker thread fills, and the window empties.
    struct Shared;

    void buildUi();
    void setStartFolder(const QString &startFolder);
    // The fields as a query; `error` says what is wrong with them.
    bool readQuery(FileSearch::Query *query, QString *error);
    // Moves what the search has found so far into the table; called every so
    // often while results are on their way.
    void takeResults();
    // The worker is through; what it left goes on screen.
    void searchEnded(const FileSearch::Summary &summary);
    void searchButtonClicked();
    // The result behind a row of the table, or null.
    const FileSearch::Match *matchAt(const QModelIndex &index) const;
    // The results that are selected, top to bottom.
    QList<FileSearch::Match> selectedMatches() const;
    void openIndex(const QModelIndex &index);
    void showContextMenu(const QPoint &position);
    void copyToClipboard(bool namesOnly);
    // What the status line says, for how far the search is and how it ended.
    void updateStatus();

    Dialogs &m_dialogs;
    QLineEdit *m_folder = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_content = nullptr;
    QCheckBox *m_matchCase = nullptr;
    QCheckBox *m_regex = nullptr;
    QToolButton *m_more = nullptr;
    QWidget *m_options = nullptr;
    QComboBox *m_lookFor = nullptr;
    QCheckBox *m_subfolders = nullptr;
    QCheckBox *m_hidden = nullptr;
    QLineEdit *m_minSize = nullptr;
    QLineEdit *m_maxSize = nullptr;
    QCheckBox *m_modified = nullptr;
    QDateEdit *m_modifiedFrom = nullptr;
    QDateEdit *m_modifiedTo = nullptr;
    QComboBox *m_contentLimit = nullptr;
    QSpinBox *m_limit = nullptr;
    QToolButton *m_search = nullptr;
    QProgressBar *m_progress = nullptr;
    ResultsModel *m_model = nullptr;
    ResultProxy *m_proxy = nullptr;
    QTableView *m_results = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_current = nullptr;
    QTimer *m_timer = nullptr;

    // The search that is running; null when none is.
    QFutureWatcher<FileSearch::Summary> *m_watcher = nullptr;
    std::shared_ptr<Shared> m_shared;
    // Set once the worker is through: the window then only waits for the
    // results to reach the table.
    std::optional<FileSearch::Summary> m_summary;
    QElapsedTimer m_elapsed;
    double m_seconds = 0;  // how long the search took
    std::shared_ptr<std::atomic<bool>> m_stop;  // set to stop the search that runs
};
