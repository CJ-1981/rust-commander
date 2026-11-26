#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

#include "ops/FileOps.h"

class Dialogs;
class QProgressDialog;
class QFutureWatcherBase;
template <typename T> class QFutureWatcher;
class QWidget;

// Runs file operations on a worker thread, with a progress window that can
// stop the work, and asks about existing targets on the GUI thread. One
// operation runs at a time; what comes in while one runs waits its turn.
class OperationRunner : public QObject {
    Q_OBJECT

public:
    // Called on the GUI thread when the work is over. `error` is empty when
    // all is done, and when the user stopped it.
    using Done = std::function<void(const QString &error)>;

    OperationRunner(Dialogs &dialogs, QWidget *parent);
    ~OperationRunner() override;

    bool isBusy() const { return m_watcher != nullptr; }

    void copy(const QStringList &sources, const QString &targetFolder, Done done = {});
    void move(const QStringList &sources, const QString &targetFolder, Done done = {});
    void remove(const QStringList &paths, Done done = {});
    void trash(const QStringList &paths, Done done = {});
    // Stops the running operation at its next progress report.
    void cancel();

signals:
    // An operation is over, whether it was stopped, failed or done: what it
    // touched may have changed.
    void finished();

private:
    struct Job {
        QString title;
        std::function<bool(FileOps::Progress &progress, FileOps::OverwritePrompt &prompt,
                           QString *error)>
                work;
        Done done;
    };

    // The worker thread's report and question.
    class Bridge;
    friend class Bridge;

    void enqueue(Job job);
    void run(Job job);
    // Called on the worker thread; waits for the GUI thread to answer.
    FileOps::Existing askOnGuiThread(const QString &target);
    void showProgress(qint64 done, qint64 total, const QString &current);
    void updateProgress(qint64 done, qint64 total, const QString &current);

    Dialogs &m_dialogs;
    QWidget *m_parentWidget;
    QList<Job> m_queue;
    QString m_title;
    QProgressDialog *m_progress = nullptr;
    QFutureWatcher<QString> *m_watcher = nullptr;

    std::atomic<bool> m_cancelled{false};  // set by Stop and by the destructor
    std::atomic<bool> m_closing{false};
    // What the worker asked and whether it was answered. When the runner
    // goes away, the question is abandoned and the worker gets on with stopping.
    std::mutex m_mutex;
    std::condition_variable m_answerGiven;
    bool m_answered = false;
    bool m_abandoned = false;
    FileOps::Existing m_answer = FileOps::Existing::Cancel;
};
