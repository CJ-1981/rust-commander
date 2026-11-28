// What the command line says, and what it does: cd, view and edit, a path
// that is there, a program with its arguments, quotes around names with
// spaces, ~ for home and relative paths from the folder shown; the arrow
// keys walk the history, and Escape gives the keyboard back to the files.
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "app/CommandLine.h"

namespace {

using Kind = CommandLine::Kind;

}  // namespace

class TestCommandLine : public QObject {
    Q_OBJECT

private slots:
    void parsesTheWordsOfItsOwn();
    void aPathThatIsThereIsOpenedOrEntered();
    void quotesAndHomesAndRelativePaths();
    void anythingElseStartsAProgram();
    void nothingOrHalfOfSomethingIsSaid();
    void theHistoryWalksUpAndDown();
    void escapeAsksForTheFileList();
};

void TestCommandLine::parsesTheWordsOfItsOwn()
{
    CommandLine::Command command;
    QString error;

    QVERIFY(CommandLine::parse("cd /tmp/somewhere", "/", &command, &error));
    QCOMPARE(command.kind, Kind::Navigate);
    QCOMPARE(command.path, QStringLiteral("/tmp/somewhere"));

    // Case does not matter for the word; it does for what follows it.
    QVERIFY(CommandLine::parse("CD ../up", "/tmp/docs", &command, &error));
    QCOMPARE(command.kind, Kind::Navigate);
    QCOMPARE(command.path, QStringLiteral("/tmp/up"));

    QVERIFY(CommandLine::parse("view photo.jpg", "/home", &command, &error));
    QCOMPARE(command.kind, Kind::View);
    QCOMPARE(command.path, QStringLiteral("/home/photo.jpg"));

    QVERIFY(CommandLine::parse("edit notes.txt", "/home/docs", &command, &error));
    QCOMPARE(command.kind, Kind::Edit);
    QCOMPARE(command.path, QStringLiteral("/home/docs/notes.txt"));

    // The word alone, with nothing after it: said, not done.
    QVERIFY(!CommandLine::parse("cd", "/", &command, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!CommandLine::parse("view   ", "/", &command, &error));
    QVERIFY(!error.isEmpty());
}

void TestCommandLine::aPathThatIsThereIsOpenedOrEntered()
{
    QTemporaryDir root;
    const QString folder = root.path();
    QFile file(folder + "/notes.txt");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    CommandLine::Command command;
    QString error;
    QVERIFY(CommandLine::parse(folder + "/notes.txt", "/elsewhere", &command, &error));
    QCOMPARE(command.kind, Kind::Open);
    QCOMPARE(command.path, folder + "/notes.txt");

    QVERIFY(CommandLine::parse(folder, "/elsewhere", &command, &error));
    QCOMPARE(command.kind, Kind::Navigate);
    QCOMPARE(command.path, folder);

    // A relative name that names a file of the folder shown.
    QVERIFY(CommandLine::parse("notes.txt", folder, &command, &error));
    QCOMPARE(command.kind, Kind::Open);
    QCOMPARE(command.path, folder + "/notes.txt");

    // A path that is not there is not open or enter; it is a program name.
    QVERIFY(CommandLine::parse(folder + "/no-such-file", folder, &command, &error));
    QCOMPARE(command.kind, Kind::Launch);
    QCOMPARE(command.program, folder + "/no-such-file");
}

void TestCommandLine::quotesAndHomesAndRelativePaths()
{
    CommandLine::Command command;
    QString error;

    // Quotes come off a name with spaces in it, which is then taken from the
    // folder shown.
    QTemporaryDir root;
    const QString folder = root.path();
    QFile file(folder + "/my notes.txt");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QVERIFY(CommandLine::parse("\"my notes.txt\"", folder, &command, &error));
    QCOMPARE(command.kind, Kind::Open);
    QCOMPARE(command.path, folder + "/my notes.txt");

    QVERIFY(CommandLine::parse("view \"my notes.txt\"", folder, &command, &error));
    QCOMPARE(command.kind, Kind::View);
    QCOMPARE(command.path, folder + "/my notes.txt");

    // ~ alone is the home folder, which is always there.
    QVERIFY(CommandLine::parse("cd ~", folder, &command, &error));
    QCOMPARE(command.kind, Kind::Navigate);
    QCOMPARE(command.path, QDir::homePath());

    QVERIFY(CommandLine::parse("cd ..", "/home/docs/sub", &command, &error));
    QCOMPARE(command.path, QStringLiteral("/home/docs"));

    QVERIFY(CommandLine::parse("cd .", "/home/docs/sub", &command, &error));
    QCOMPARE(command.path, QStringLiteral("/home/docs/sub"));
}

void TestCommandLine::anythingElseStartsAProgram()
{
    CommandLine::Command command;
    QString error;

    QVERIFY(CommandLine::parse("clang-format -i file.cpp", "/", &command, &error));
    QCOMPARE(command.kind, Kind::Launch);
    QCOMPARE(command.program, QStringLiteral("clang-format"));
    QCOMPARE(command.arguments, QStringList({"-i", "file.cpp"}));

    // Quotes keep a word with spaces together.
    QVERIFY(CommandLine::parse("open \"my folder/file name\" --wait", "/", &command, &error));
    QCOMPARE(command.kind, Kind::Launch);
    QCOMPARE(command.program, QStringLiteral("open"));
    QCOMPARE(command.arguments, QStringList({"my folder/file name", "--wait"}));

    QVERIFY(CommandLine::parse("just-one-word", "/", &command, &error));
    QCOMPARE(command.kind, Kind::Launch);
    QCOMPARE(command.arguments, QStringList());
}

void TestCommandLine::nothingOrHalfOfSomethingIsSaid()
{
    CommandLine::Command command;
    QString error;

    // An empty line is no command at all, and nothing is said about it.
    QVERIFY(!CommandLine::parse("", "/", &command, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(!CommandLine::parse("   ", "/", &command, &error));
    QVERIFY(error.isEmpty());

    QVERIFY(CommandLine::parse("clang-format", "/", &command, &error));
    QCOMPARE(command.kind, Kind::Launch);
}

void TestCommandLine::theHistoryWalksUpAndDown()
{
    CommandLine field;
    field.setHistory({"first", "second", "third"});

    field.setText("typed now");
    QTest::keyClick(&field, Qt::Key_Up);
    QCOMPARE(field.text(), QStringLiteral("third"));
    QTest::keyClick(&field, Qt::Key_Up);
    QCOMPARE(field.text(), QStringLiteral("second"));
    QTest::keyClick(&field, Qt::Key_Up);
    QCOMPARE(field.text(), QStringLiteral("first"));
    // The oldest is where it stops.
    QTest::keyClick(&field, Qt::Key_Up);
    QCOMPARE(field.text(), QStringLiteral("first"));

    // Down past the newest brings back what was typed.
    QTest::keyClick(&field, Qt::Key_Down);
    QTest::keyClick(&field, Qt::Key_Down);
    QTest::keyClick(&field, Qt::Key_Down);
    QTest::keyClick(&field, Qt::Key_Down);
    QCOMPARE(field.text(), QStringLiteral("typed now"));

    // An empty history changes nothing.
    field.setHistory({});
    QTest::keyClick(&field, Qt::Key_Up);
    QCOMPARE(field.text(), QStringLiteral("typed now"));
}

void TestCommandLine::escapeAsksForTheFileList()
{
    CommandLine field;
    QSignalSpy focus(&field, &CommandLine::focusRequested);
    field.setText("half typed");
    QTest::keyClick(&field, Qt::Key_Escape);
    QCOMPARE(focus.size(), 1);
    // What was half typed is gone as well.
    QCOMPARE(field.text(), QString());
}

QTEST_MAIN(TestCommandLine)
#include "commandline.moc"
