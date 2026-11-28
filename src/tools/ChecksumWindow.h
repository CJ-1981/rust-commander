#pragma once

#include <QDialog>
#include <QFont>
#include <QList>
#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <memory>

#include "tools/Checksums.h"

class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QFutureWatcherBase;
template <typename T> class QFutureWatcher;
class QLabel;
class QLineEdit;
class QProgressBar;
class QStackedWidget;
class QTabBar;
class QTableWidget;
class QThreadPool;
class QToolButton;
class Dialogs;

// The hashes of files: whether a file has the hash it should have, whether
// the files a checksum file lists still have theirs, and a checksum file for
// files of one's own. The work is done on a worker thread, so the window stays
// usable while it goes and can stop it.
class ChecksumWindow : public QDialog {
    Q_OBJECT

public:
    // What the Compare page says about the hash in the field for the expected one.
    enum class Verdict {
        Nothing,  // nothing to compare with yet, or nothing hashed yet
        Match,
        Different,
        NotHash,  // not a hash of the kind that was computed
    };

    // Opens a window for `file` and shows it: a checksum file is checked
    // against the files around it, anything else is hashed. The window deletes
    // itself when it is closed.
    static ChecksumWindow *open(const QString &file, Dialogs &dialogs, QWidget *parent = nullptr);

    explicit ChecksumWindow(Dialogs &dialogs, QWidget *parent = nullptr);
    ~ChecksumWindow() override;

    // Comparing a file with a hash
    // Hashes the files with the chosen algorithm, one after the other, and
    // shows the Compare page. What was hashed before is stopped.
    void setFiles(const QStringList &paths);
    // The files of the Compare page.
    QStringList files() const;
    // Hashes the files again, with this one.
    void setAlgorithm(Checksums::Algorithm algorithm);
    Checksums::Algorithm algorithm() const { return m_algorithm; }
    // The hash of the file whose row is selected in the table, or of the only
    // one. Empty while it is being hashed, and when it could not be read.
    QString hash() const;
    // While files are hashed on a worker thread, or wait for their turn.
    bool isHashing() const;
    // Stops it; what was found so far stays.
    void stop();
    // The field for the hash the file should have, and what is said about it.
    QLineEdit *expectedField() const { return m_expected; }
    QLabel *verdictLabel() const { return m_verdict; }
    Verdict verdict() const { return m_verdictNow; }

    // Checking a checksum file
    // Reads the checksum file, takes its folder for the one with the files,
    // and checks it. False when the file could not be read as a list of
    // checksums; the status line says why.
    bool setChecksumFile(const QString &path);
    // The folder with the files the checksum file lists; the check starts over.
    void setFolder(const QString &folder);
    QString checksumFile() const { return m_list.path; }
    QString folder() const { return m_folder; }
    // The verification table: what was found for every line of the checksum
    // file, in the order of the file.
    QList<Checksums::Check> checks() const;
    QTableWidget *table() const { return m_table; }

    // Writes a checksum file with the files of the page that is shown and the
    // hashes that were found for them. A file that could not be read is left
    // out. Asks before a file that is there is replaced.
    bool saveChecksumFile(const QString &path);

    // The line along the bottom: how far the work is, how it came out.
    QLabel *statusLabel() const { return m_status; }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    enum class Page {
        Compare,  // a file and the hash it should have
        Verify,   // a checksum file against a folder
    };

    // What the worker thread is asked to do: hash these files, and say whether
    // they have the hashes that are expected of them.
    struct Request {
        quint64 id = 0;  // which request this is: a page's newest one is the one that counts
        Page page = Page::Compare;
        QList<Checksums::Item> items;
    };

    // How far the running job has got, as the worker thread reports it.
    struct Advance {
        qint64 done = 0;  // bytes
        qint64 total = 0;
        int files = 0;    // files looked at
        int count = 0;
        QString current;
    };

    // The worker thread's report while a job runs.
    class Bridge;
    friend class Bridge;

    void buildUi();
    QWidget *buildComparePage();
    QWidget *buildVerifyPage();
    // A checksum file is checked against the files around it; anything else is hashed.
    void takeFile(const QString &path);
    void showPage(Page page);
    Page page() const;
    // What the page shows: one row per file on the Compare page, per line of the checksum file on the Verify page.
    QList<Checksums::Check> *rowsFor(Page page);
    const QList<Checksums::Check> *rowsFor(Page page) const;
    // Whether a job of `page` is running, or waiting for its turn.
    bool waiting(Page page) const;
    // Whether the job `id` is the one running, and still the one its page wants to see.
    bool isCurrent(quint64 id) const;
    // Has the worker thread do `request`, after what is running has stopped.
    void request(Request request);
    // What was asked of `page` is no longer wanted.
    void withdraw(Page page);
    void startNext();
    void finishedJob(const Request &request, const QList<Checksums::Check> &checks);
    void showAdvance(quint64 id, const Advance &advance);
    void showCheck(quint64 id, int row, const Checksums::Check &check);
    void fillTable();
    void setRow(int row, const Checksums::Check &check);
    // What the table says about a row, with the hash in the expected field where this is the Compare page.
    Checksums::Status shownStatus(const Checksums::Check &check) const;
    void updateCompare();
    void updateStatus();
    void updateProgress();
    void updateTitle();
    void refresh();
    void expectedChanged();
    // The expected hash is typed to its end.
    void expectedSettled();
    // Computes another algorithm when the expected hash is of that kind.
    void followExpectedAlgorithm();
    // Says something for a moment in the status line, then the state of the page again.
    void notice(const QString &text);
    QString workText(Page page) const;
    bool canSave() const;

    void browseForFile();
    void browseForList();
    void browseForFolder();
    // Asks where, then saves.
    void browseForTarget();
    void verify();
    void copyHash();
    void paste();

    Dialogs &m_dialogs;
    // Its own, with one thread: the work doesn't wait for whatever else in the app uses threads.
    QThreadPool *m_pool = nullptr;

    QTabBar *m_tabs = nullptr;
    QStackedWidget *m_pages = nullptr;

    // The Compare page
    QLineEdit *m_fileField = nullptr;
    QToolButton *m_browseFile = nullptr;
    QComboBox *m_algorithmBox = nullptr;
    QLabel *m_sizeLabel = nullptr;
    QLineEdit *m_digestField = nullptr;
    QToolButton *m_copy = nullptr;
    QLineEdit *m_expected = nullptr;
    QToolButton *m_paste = nullptr;
    QToolButton *m_clear = nullptr;
    QLabel *m_verdictIcon = nullptr;
    QLabel *m_verdict = nullptr;

    // The Verify page
    QLineEdit *m_listField = nullptr;
    QToolButton *m_browseList = nullptr;
    QLineEdit *m_folderField = nullptr;
    QToolButton *m_browseFolder = nullptr;

    // Shared
    QProgressBar *m_progress = nullptr;
    QLabel *m_progressText = nullptr;
    QToolButton *m_stop = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QToolButton *m_verify = nullptr;
    QToolButton *m_save = nullptr;

    QFont m_monospace;

    // What the pages show
    QList<Checksums::Check> m_compare;  // one row per file of the Compare page
    Checksums::ChecksumFile m_list;     // what was read from the checksum file
    QString m_folder;                   // where its files are looked for
    QString m_problem;                  // why the last checksum file could not be read
    QList<Checksums::Check> m_checks;   // one row per line of the checksum file
    Checksums::Algorithm m_algorithm = Checksums::Algorithm::Sha256;
    int m_current = 0;                  // the row of the Compare page whose hash is shown
    qint64 m_compareBytes = 0;          // how much there is to hash on the Compare page
    QString m_wanted;                   // the hash in the expected field, in lower case
    qsizetype m_expectedSize = 0;       // how long that field was before it changed last
    bool m_settled = true;              // the expected hash was pasted at once, or typed to its end
    Verdict m_verdictNow = Verdict::Nothing;
    Verdict m_badgeFor = Verdict::Nothing;
    QColor m_badgeColor;
    QString m_notice;                   // what the status line says for the moment

    // The job
    QFutureWatcher<QList<Checksums::Check>> *m_watcher = nullptr;  // set while the worker thread runs
    std::shared_ptr<std::atomic<bool>> m_cancel;  // set by Stop, by a newer request and by the destructor
    Request m_running;
    QList<Request> m_queue;             // waiting for the running job
    std::array<quint64, 2> m_newest = {0, 0};  // the newest request of each page
    quint64 m_nextId = 1;
    quint64 m_halted = 0;               // the job the user stopped
    Advance m_advance;
};
