#include <QApplication>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTest>
#include <QTextCursor>

#include "helpers.h"
#include "viewer/ViewerWindow.h"

namespace {

// A text file in a temp folder, and the dialogs the viewer asks.
struct Fixture {
    QTemporaryDir root;
    QString path;
    QByteArray original;
    ScriptedDialogs dialogs;

    explicit Fixture(const QByteArray &content = "first line\nsecond line\n")
        : path(root.path() + "/notes.txt")
        , original(content)
    {
        writeFile(path, content);
    }

    // The file as it is on disk, byte for byte.
    QByteArray onDisk() const
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
};

}  // namespace

class TestViewer : public QObject {
    Q_OBJECT

private slots:
    void showsAFileReadOnly();
    void editsAndSaves();
    void tabIndentsAndTheKeysStayInTheText();
    void closingWithChangesAsksFirst();
    void aFileThatIsNotText();
};

void TestViewer::showsAFileReadOnly()
{
    Fixture f;
    ViewerWindow viewer(f.path, ViewerWindow::Mode::View, f.dialogs);
    QVERIFY(viewer.isText());
    QCOMPARE(viewer.editor()->toPlainText(), readFile(f.path));
    QVERIFY(viewer.editor()->isReadOnly());
    QVERIFY(!viewer.isModified());
    QVERIFY(viewer.windowTitle().contains(QStringLiteral("notes.txt")));
    QVERIFY(viewer.windowTitle().contains(QStringLiteral("Viewer")));
}

void TestViewer::editsAndSaves()
{
    // Korean, and a line ending that isn't the usual one.
    const QString text = QString::fromUtf8("첫 줄\r\nsecond\r\n");
    Fixture f(text.toUtf8());
    ViewerWindow viewer(f.path, ViewerWindow::Mode::Edit, f.dialogs);
    QVERIFY(!viewer.editor()->isReadOnly());
    // The text shows every line ending the same way.
    QCOMPARE(viewer.editor()->toPlainText(), QString::fromUtf8("첫 줄\nsecond\n"));
    QVERIFY(!viewer.isModified());
    // A newly opened file starts from the top.
    QCOMPARE(viewer.editor()->textCursor().position(), 0);

    // The whole text replaced by two lines.
    viewer.editor()->selectAll();
    viewer.editor()->insertPlainText(QStringLiteral("changed\nagain"));
    QVERIFY(viewer.isModified());
    QVERIFY(viewer.windowTitle().contains(QStringLiteral("*")));
    // Nothing is written until it is saved.
    QCOMPARE(f.onDisk(), f.original);

    QVERIFY(viewer.save());
    // ... with the line endings the file had.
    QCOMPARE(f.onDisk(), QByteArray("changed\r\nagain"));
    QVERIFY(!viewer.isModified());
    QVERIFY(!viewer.windowTitle().contains(QStringLiteral("*")));

    // Another new text, and again with the file's line endings.
    viewer.editor()->selectAll();
    viewer.editor()->insertPlainText(QStringLiteral("a\nb\nc\n"));
    QVERIFY(viewer.isModified());
    QVERIFY(viewer.save());
    QCOMPARE(f.onDisk(), QByteArray("a\r\nb\r\nc\r\n"));
}

void TestViewer::tabIndentsAndTheKeysStayInTheText()
{
    Fixture f;
    ViewerWindow viewer(f.path, ViewerWindow::Mode::Edit, f.dialogs);
    viewer.show();
    QVERIFY(QTest::qWaitForWindowExposed(&viewer));
    viewer.editor()->setFocus();
    viewer.editor()->moveCursor(QTextCursor::End);
    QCOMPARE(QApplication::focusWidget(), viewer.editor());

    QTest::keyClicks(viewer.editor(), "ab");
    QCOMPARE(QApplication::focusWidget(), viewer.editor());
    QTest::keyClick(viewer.editor(), Qt::Key_Tab);
    QTest::keyClicks(viewer.editor(), "c");

    QCOMPARE(viewer.editor()->toPlainText(), QStringLiteral("first line\nsecond line\nab\tc"));
    // After all that typing, the keys are still in the text.
    QCOMPARE(QApplication::focusWidget(), viewer.editor());
}

void TestViewer::closingWithChangesAsksFirst()
{
    // Cancel: the window stays.
    {
        Fixture f;
        QPointer<ViewerWindow> viewer = new ViewerWindow(f.path, ViewerWindow::Mode::Edit, f.dialogs);
        viewer->show();
        QVERIFY(QTest::qWaitForWindowExposed(viewer));
        viewer->editor()->appendPlainText(QStringLiteral("kept"));
        QVERIFY2(viewer->isModified(), "appending a line is a change");
        f.dialogs.answer = Dialogs::Answer::Cancel;
        viewer->close();
        QVERIFY(!viewer.isNull());
        QVERIFY(viewer->isVisible());
        QCOMPARE(readFile(f.path), QStringLiteral("first line\nsecond line\n"));
        QCOMPARE(f.dialogs.questions.size(), 1);
        // Nothing more is asked when there is nothing to save.
        viewer->editor()->document()->setModified(false);
        viewer->close();
        QVERIFY(viewer.isNull() || !viewer->isVisible());
    }

    // Don't save: the changes are dropped.
    {
        Fixture f;
        QPointer<ViewerWindow> viewer = ViewerWindow::open(f.path, ViewerWindow::Mode::Edit, f.dialogs, nullptr);
        viewer->editor()->appendPlainText(QStringLiteral("dropped"));
        QVERIFY(viewer->isModified());
        f.dialogs.answer = Dialogs::Answer::No;
        viewer->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(viewer.isNull());
        QCOMPARE(readFile(f.path), QStringLiteral("first line\nsecond line\n"));
    }

    // Save: the changes are in the file.
    {
        Fixture f;
        QPointer<ViewerWindow> viewer = ViewerWindow::open(f.path, ViewerWindow::Mode::Edit, f.dialogs, nullptr);
        viewer->editor()->appendPlainText(QStringLiteral("saved"));
        QVERIFY(viewer->isModified());
        f.dialogs.answer = Dialogs::Answer::Yes;
        viewer->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(viewer.isNull());
        // A new paragraph was added after the empty last line of the file.
        QCOMPARE(readFile(f.path), QStringLiteral("first line\nsecond line\n\nsaved"));
    }
}

void TestViewer::aFileThatIsNotText()
{
    Fixture f(QByteArray("text first\x00 then not", 20));
    ViewerWindow viewer(f.path, ViewerWindow::Mode::Edit, f.dialogs);
    QVERIFY(!viewer.isText());
    QVERIFY(viewer.editor()->isReadOnly());  // not even to edit
    QVERIFY(!viewer.isModified());
    // Nothing is saved over it.
    QVERIFY(viewer.save());
    QCOMPARE(f.onDisk(), f.original);

    // What can't be read is said so, and not written over either.
    ViewerWindow missing(f.root.path() + "/no-such-file", ViewerWindow::Mode::View, f.dialogs);
    QVERIFY(!missing.isText());
    QVERIFY(missing.editor()->toPlainText().contains(QStringLiteral("could not be read")));
}

QTEST_MAIN(TestViewer)
#include "viewer.moc"
