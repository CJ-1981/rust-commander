#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>

#include "app/Dialogs.h"
#include "helpers.h"
#include "ops/OperationRunner.h"

namespace {

// Answers from a script, and remembers what it was asked.
class ScriptedDialogs : public Dialogs {
public:
    std::optional<QString> askText(const QString &, const QString &, const QString &) override
    {
        return std::nullopt;
    }
    Answer ask(const QString &, const QString &) override { return Answer::Cancel; }
    void tell(const QString &title, const QString &text) override
    {
        told.append(title + ": " + text);
    }
    FileOps::Existing askOverwrite(const QString &target) override
    {
        ++asked;
        askedOn = QThread::currentThread();
        targets.append(target);
        if (whenAsked) {
            whenAsked();
        }
        return script.isEmpty() ? FileOps::Existing::Cancel : script.takeFirst();
    }

    QList<FileOps::Existing> script;
    QStringList targets;
    QStringList told;
    std::function<void()> whenAsked;
    QThread *askedOn = nullptr;
    int asked = 0;
};

// A window for the runner, a source and a destination folder.
struct Fixture {
    QTemporaryDir root;
    QWidget window;
    ScriptedDialogs dialogs;
    OperationRunner runner{dialogs, &window};

    QString source() const { return root.path() + "/src"; }
    QString target() const { return root.path() + "/dst"; }
    Fixture()
    {
        makeFolder(source());
        makeFolder(target());
    }
    // `count` files in the source folder, and in the destination if `there`.
    void files(int count, bool there)
    {
        for (int i = 0; i < count; ++i) {
            writeFile(source() + QString("/f%1.txt").arg(i, 3, 10, QLatin1Char('0')), "new");
            if (there) {
                writeFile(target() + QString("/f%1.txt").arg(i, 3, 10, QLatin1Char('0')), "old");
            }
        }
    }
    // The paths of the first `count` files in the source folder.
    QStringList paths(int count) const
    {
        QStringList result;
        for (int i = 0; i < count; ++i) {
            result.append(source() + QString("/f%1.txt").arg(i, 3, 10, QLatin1Char('0')));
        }
        return result;
    }
    QString read(const QString &name) const
    {
        QFile file(target() + "/" + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString("<none>");
    }
};

}  // namespace

class TestOperationRunner : public QObject {
    Q_OBJECT

private slots:
    void runsTheWorkElsewhere();
    void asksAboutExistingTargetsOnTheGuiThread();
    void stoppingKeepsWhatIsDone();
    void cancellingInTheQuestionStopsTheRest();
    void queuesWhatComesInWhileBusy();
    void reportsFailuresAndGoesOn();
};

void TestOperationRunner::runsTheWorkElsewhere()
{
    Fixture f;
    f.files(400, false);
    QSignalSpy finished(&f.runner, &OperationRunner::finished);
    int done = 0;
    QString error = QStringLiteral("not done");
    f.runner.copy(f.paths(400), f.target(),
                  [&](const QString &why) {
                      ++done;
                      error = why;
                  });
    QVERIFY(f.runner.isBusy());

    // The GUI thread stays free while the work is done.
    int beats = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, [&] { ++beats; });
    heartbeat.start(5);
    QTRY_COMPARE_WITH_TIMEOUT(done, 1, 15000);
    heartbeat.stop();

    QCOMPARE(error, QString());
    QCOMPARE(finished.count(), 1);
    QVERIFY(!f.runner.isBusy());
    QVERIFY(f.dialogs.told.isEmpty());
    QVERIFY(beats > 0);
    QCOMPARE(f.read("f000.txt"), QString("new"));
    QCOMPARE(f.read("f399.txt"), QString("new"));
}

void TestOperationRunner::asksAboutExistingTargetsOnTheGuiThread()
{
    Fixture f;
    f.files(3, true);
    f.dialogs.script = {FileOps::Existing::Overwrite, FileOps::Existing::SkipAll};
    int done = 0;
    f.runner.copy(f.paths(3), f.target(),
                  [&](const QString &) { ++done; });
    QTRY_COMPARE_WITH_TIMEOUT(done, 1, 15000);

    // Asked on the GUI thread, which is this one.
    QCOMPARE(f.dialogs.askedOn, QThread::currentThread());
    // The third is not asked about any more, after "Skip all".
    QCOMPARE(f.dialogs.asked, 2);
    QCOMPARE(f.dialogs.targets, QStringList({f.target() + "/f000.txt", f.target() + "/f001.txt"}));
    QCOMPARE(f.read("f000.txt"), QString("new"));
    QCOMPARE(f.read("f001.txt"), QString("old"));
    QCOMPARE(f.read("f002.txt"), QString("old"));
}

void TestOperationRunner::stoppingKeepsWhatIsDone()
{
    Fixture f;
    writeFile(f.source() + "/f0.txt", "new");
    writeFile(f.source() + "/f1.txt", "new");
    writeFile(f.source() + "/f2.txt", "new");
    writeFile(f.source() + "/f3.txt", "new");
    writeFile(f.target() + "/f1.txt", "old");

    // Stop is pressed while the question about f1 is being answered, with "replace".
    f.dialogs.whenAsked = [&] { f.runner.cancel(); };
    f.dialogs.script = {FileOps::Existing::Overwrite};
    int done = 0;
    QString error = QStringLiteral("not done");
    QSignalSpy finished(&f.runner, &OperationRunner::finished);
    f.runner.copy({f.source() + "/f0.txt", f.source() + "/f1.txt", f.source() + "/f2.txt",
                   f.source() + "/f3.txt"},
                  f.target(), [&](const QString &why) {
                      ++done;
                      error = why;
                  });
    QTRY_COMPARE_WITH_TIMEOUT(done, 1, 15000);

    QCOMPARE(error, QString());  // stopping is not a failure
    QCOMPARE(finished.count(), 1);
    QVERIFY(!f.runner.isBusy());
    QVERIFY(f.dialogs.told.isEmpty());
    // What was done stays done; the rest was left alone.
    QCOMPARE(f.read("f0.txt"), QString("new"));
    QCOMPARE(f.read("f1.txt"), QString("new"));
    QCOMPARE(f.read("f2.txt"), QString("<none>"));
    QCOMPARE(f.read("f3.txt"), QString("<none>"));
    QVERIFY(QFileInfo::exists(f.source() + "/f2.txt"));
}

void TestOperationRunner::cancellingInTheQuestionStopsTheRest()
{
    Fixture f;
    writeFile(f.source() + "/f0.txt", "new");
    writeFile(f.source() + "/f1.txt", "new");
    writeFile(f.source() + "/f2.txt", "new");
    writeFile(f.target() + "/f1.txt", "old");

    // No answer in the script means Cancel.
    int done = 0;
    QString error = QStringLiteral("not done");
    f.runner.copy({f.source() + "/f0.txt", f.source() + "/f1.txt", f.source() + "/f2.txt"},
                  f.target(), [&](const QString &why) {
                      ++done;
                      error = why;
                  });
    QTRY_COMPARE_WITH_TIMEOUT(done, 1, 15000);

    QCOMPARE(error, QString());
    QCOMPARE(f.dialogs.asked, 1);
    QCOMPARE(f.read("f0.txt"), QString("new"));
    QCOMPARE(f.read("f1.txt"), QString("old"));
    QCOMPARE(f.read("f2.txt"), QString("<none>"));
    QVERIFY(!f.runner.isBusy());
}

void TestOperationRunner::queuesWhatComesInWhileBusy()
{
    Fixture f;
    writeFile(f.source() + "/a.txt", "a");
    writeFile(f.source() + "/b.txt", "b");
    QStringList order;
    // Three at once: they take turns, in the order they came in.
    f.runner.copy({f.source() + "/a.txt"}, f.target(), [&](const QString &) { order.append("copy a"); });
    f.runner.copy({f.source() + "/b.txt"}, f.target(), [&](const QString &) { order.append("copy b"); });
    f.runner.remove({f.target() + "/b.txt"}, [&](const QString &) { order.append("remove b"); });

    QTRY_COMPARE_WITH_TIMEOUT(order.size(), 3, 15000);
    QCOMPARE(order, QStringList({"copy a", "copy b", "remove b"}));
    QVERIFY(!f.runner.isBusy());
    QVERIFY(QFileInfo::exists(f.target() + "/a.txt"));
    QVERIFY(!QFileInfo::exists(f.target() + "/b.txt"));
    // The removal only started once the copies were over.
    QVERIFY(QFileInfo::exists(f.source() + "/b.txt"));
}

void TestOperationRunner::reportsFailuresAndGoesOn()
{
    Fixture f;
    writeFile(f.source() + "/real.txt", "r");
    QStringList results;
    f.runner.copy({f.source() + "/no-such-file.txt"}, f.target(),
                  [&](const QString &error) { results.append(error); });
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 15000);
    QVERIFY(!results.first().isEmpty());
    QCOMPARE(f.dialogs.told.size(), 1);
    QVERIFY(f.dialogs.told.first().startsWith("Copying: "));
    QVERIFY(!f.runner.isBusy());

    // The runner is free again.
    f.runner.copy({f.source() + "/real.txt"}, f.target(),
                  [&](const QString &error) { results.append(error); });
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 2, 15000);
    QCOMPARE(results.last(), QString());
    QCOMPARE(f.read("real.txt"), QString("r"));
    QCOMPARE(f.dialogs.told.size(), 1);
}

QTEST_MAIN(TestOperationRunner)
#include "operationrunner.moc"
