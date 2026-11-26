#include "OperationRunner.h"

#include "app/Dialogs.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QtConcurrent/QtConcurrentRun>

#include <chrono>

namespace {

using namespace std::chrono_literals;

constexpr auto kShowProgressAfter = 400ms;   // small jobs never show a window
constexpr auto kUpdateProgressEvery = 80ms;  // not more often than the eye can follow

QString tr(const char *text)
{
    return QCoreApplication::translate("OperationRunner", text);
}

}  // namespace

// The worker thread's report and question while an operation runs.
class OperationRunner::Bridge : public FileOps::Progress, public FileOps::OverwritePrompt {
public:
    explicit Bridge(OperationRunner *runner)
        : m_runner(runner)
        , m_started(std::chrono::steady_clock::now())
    {
    }

    bool tick(qint64 done, qint64 total, const QString &current) override
    {
        if (m_runner->m_cancelled.load()) {
            return false;
        }
        // A window only once the work has taken a moment, so that small jobs
        // don't flash one.
        const auto now = std::chrono::steady_clock::now();
        if (!m_shown) {
            if (now - m_started < kShowProgressAfter) {
                return true;
            }
            m_shown = true;
            QMetaObject::invokeMethod(
                    m_runner,
                    [runner = m_runner, done, total, current] {
                        runner->showProgress(done, total, current);
                    },
                    Qt::QueuedConnection);
        } else if (now - m_lastUpdate >= kUpdateProgressEvery) {
            m_lastUpdate = now;
            QMetaObject::invokeMethod(
                    m_runner,
                    [runner = m_runner, done, total, current] {
                        runner->updateProgress(done, total, current);
                    },
                    Qt::QueuedConnection);
        }
        return !m_runner->m_cancelled.load();
    }

    FileOps::Existing ask(const QString &target) override
    {
        return m_runner->askOnGuiThread(target);
    }

private:
    OperationRunner *m_runner;
    std::chrono::steady_clock::time_point m_started;
    std::chrono::steady_clock::time_point m_lastUpdate;
    bool m_shown = false;
};

OperationRunner::OperationRunner(Dialogs &dialogs, QWidget *parent)
    : QObject(parent)
    , m_dialogs(dialogs)
    , m_parentWidget(parent)
{
}

OperationRunner::~OperationRunner()
{
    // The worker stops at its next report, and a question it is waiting to
    // have answered is abandoned, so that it can get on with stopping.
    m_closing = true;
    m_cancelled = true;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_abandoned = true;
    }
    m_answerGiven.notify_all();
    if (m_watcher) {
        m_watcher->waitForFinished();
    }
}

void OperationRunner::copy(const QStringList &sources, const QString &targetFolder, Done done)
{
    enqueue(Job{tr("Copying"),
                [sources, targetFolder](FileOps::Progress &progress, FileOps::OverwritePrompt &prompt,
                                        QString *error) {
                    return FileOps::copy(sources, targetFolder, prompt, progress, error);
                },
                std::move(done)});
}

void OperationRunner::move(const QStringList &sources, const QString &targetFolder, Done done)
{
    enqueue(Job{tr("Moving"),
                [sources, targetFolder](FileOps::Progress &progress, FileOps::OverwritePrompt &prompt,
                                        QString *error) {
                    return FileOps::move(sources, targetFolder, prompt, progress, error);
                },
                std::move(done)});
}

void OperationRunner::remove(const QStringList &paths, Done done)
{
    enqueue(Job{tr("Deleting"),
                [paths](FileOps::Progress &progress, FileOps::OverwritePrompt &, QString *error) {
                    return FileOps::remove(paths, progress, error);
                },
                std::move(done)});
}

void OperationRunner::trash(const QStringList &paths, Done done)
{
    enqueue(Job{tr("Moving to the Trash"),
                [paths](FileOps::Progress &, FileOps::OverwritePrompt &, QString *error) {
                    return FileOps::trash(paths, error);
                },
                std::move(done)});
}

void OperationRunner::cancel()
{
    m_cancelled = true;
}

void OperationRunner::enqueue(Job job)
{
    if (isBusy()) {
        m_queue.append(job);
        return;
    }
    run(job);
}

void OperationRunner::run(Job job)
{
    m_cancelled = false;
    m_title = job.title;

    auto *watcher = new QFutureWatcher<QString>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, job] {
        const QString error = watcher->result();
        watcher->deleteLater();
        m_watcher = nullptr;
        if (m_progress) {
            m_progress->deleteLater();
            m_progress = nullptr;
        }
        if (!error.isEmpty() && !m_closing) {
            m_dialogs.tell(job.title, error);
        }
        if (job.done) {
            job.done(error);
        }
        emit finished();
        if (!m_queue.isEmpty()) {
            run(m_queue.takeFirst());
        }
    });

    watcher->setFuture(QtConcurrent::run([this, job]() -> QString {
        Bridge bridge(this);
        QString error;
        if (job.work(bridge, bridge, &error)) {
            error.clear();
        }
        return error;
    }));
}

// Called on the worker thread. The question is asked on the GUI thread;
// this thread waits for the answer.
FileOps::Existing OperationRunner::askOnGuiThread(const QString &target)
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_abandoned) {
            return FileOps::Existing::Cancel;
        }
        m_answered = false;
    }

    QMetaObject::invokeMethod(
            this,
            [this, target] {
                const FileOps::Existing answer =
                        m_closing ? FileOps::Existing::Cancel : m_dialogs.askOverwrite(target);
                {
                    const std::lock_guard<std::mutex> lock(m_mutex);
                    m_answer = answer;
                    m_answered = true;
                }
                m_answerGiven.notify_all();
            },
            Qt::QueuedConnection);

    std::unique_lock<std::mutex> lock(m_mutex);
    m_answerGiven.wait(lock, [this] { return m_answered || m_abandoned; });
    return m_abandoned ? FileOps::Existing::Cancel : m_answer;
}

void OperationRunner::showProgress(qint64 done, qint64 total, const QString &current)
{
    if (m_closing) {
        return;
    }
    m_progress = new QProgressDialog(m_title, tr("Stop"), 0, 1000, m_parentWidget);
    m_progress->setWindowTitle(m_title);
    m_progress->setWindowModality(Qt::WindowModal);
    // Kept open until the work is really over, including its last question.
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    m_progress->setMinimumDuration(0);
    connect(m_progress, &QProgressDialog::canceled, this, &OperationRunner::cancel);
    updateProgress(done, total, current);
    m_progress->show();
}

void OperationRunner::updateProgress(qint64 done, qint64 total, const QString &current)
{
    if (!m_progress) {
        return;
    }
    m_progress->setValue(total > 0 ? int(qMin<qint64>(done * 1000 / total, 1000)) : 1000);
    // The file's name is what the user recognises; the folder is the one they chose.
    m_progress->setLabelText(current.isEmpty() ? m_title : QFileInfo(current).fileName());
}
