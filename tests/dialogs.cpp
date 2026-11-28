// The questions the app asks, answered from the keyboard: Return says yes
// (it is the default button), Esc says nothing was agreed to, and the letter
// keys sit on the buttons themselves — Y and Cmd+Y for yes, N for no, one
// letter per answer of the overwrite question. A key sent straight to the
// dialog is what QDialog itself answers on; the letter shortcuts belong to
// the shortcut map, and are checked on the buttons.
#include <QAbstractButton>
#include <QApplication>
#include <functional>
#include <QMessageBox>
#include <QPushButton>
#include <QTest>
#include <QTimer>

#include "app/Dialogs.h"
#include "ops/FileOps.h"

namespace {

// The question comes up and is closed with a key QDialog answers itself.
Dialogs::Answer asked(GuiDialogs &dialogs, Qt::Key key)
{
    QTimer::singleShot(0, [key] {
        QWidget *dialog = QApplication::activeModalWidget();
        Q_ASSERT(dialog);
        QTest::keyClick(dialog, key);
    });
    return dialogs.ask(QStringLiteral("A question"), QStringLiteral("Yes or no?"));
}

// The overwrite question comes up; what its buttons are is looked at from a
// scheduled callback, and it is closed with Esc.
FileOps::Existing overwriteClosedWithEscape(GuiDialogs &dialogs,
                                            const std::function<void(QMessageBox *)> &inspect)
{
    QTimer::singleShot(0, [inspect] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        Q_ASSERT(box);
        inspect(box);
        QTest::keyClick(box, Qt::Key_Escape);
    });
    return dialogs.askOverwrite(QStringLiteral("/tmp/some-file.txt"));
}

// The button of a QMessageBox by its caption.
QAbstractButton *buttonOf(QMessageBox *box, const QString &caption)
{
    const QList<QAbstractButton *> buttons = box->buttons();
    for (QAbstractButton *button : buttons) {
        if (button->text() == caption) {
            return button;
        }
    }
    return nullptr;
}

// One-sequence text of a button's shortcut, to compare against.
QString shortcutOf(QAbstractButton *button)
{
    return button->shortcut().toString(QKeySequence::PortableText);
}

}  // namespace

class TestDialogs : public QObject {
    Q_OBJECT

private slots:
    void returnIsYes();
    void escapeIsCancel();
    void theLetterKeysSitOnTheButtons();
    void theOverwriteKeysSitOnItsButtons();
};

void TestDialogs::returnIsYes()
{
    GuiDialogs dialogs(nullptr);
    // Return goes to the default button, and the default button is Yes, so
    // that the keyboard goes along: Y, Cmd+Y or Return all say yes.
    QCOMPARE(asked(dialogs, Qt::Key_Return), Dialogs::Answer::Yes);
    QCOMPARE(asked(dialogs, Qt::Key_Enter), Dialogs::Answer::Yes);
}

void TestDialogs::escapeIsCancel()
{
    GuiDialogs dialogs(nullptr);
    QCOMPARE(asked(dialogs, Qt::Key_Escape), Dialogs::Answer::Cancel);
}

void TestDialogs::theLetterKeysSitOnTheButtons()
{
    GuiDialogs dialogs(nullptr);
    QTimer::singleShot(0, [] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        Q_ASSERT(box);
        QAbstractButton *yes = buttonOf(box, QStringLiteral("&Yes"));
        QAbstractButton *no = buttonOf(box, QStringLiteral("&No"));
        QVERIFY(yes);
        QVERIFY(no);
        QCOMPARE(shortcutOf(yes), QStringLiteral("Y, Ctrl+Y"));
        QCOMPARE(shortcutOf(no), QStringLiteral("N, Ctrl+N"));
        QCOMPARE(box->defaultButton(), qobject_cast<QPushButton *>(yes));
        QTest::keyClick(box, Qt::Key_Escape);
    });
    QCOMPARE(dialogs.ask(QStringLiteral("A question"), QStringLiteral("Yes or no?")),
             Dialogs::Answer::Cancel);
}

void TestDialogs::theOverwriteKeysSitOnItsButtons()
{
    GuiDialogs dialogs(nullptr);
    const FileOps::Existing answer = overwriteClosedWithEscape(dialogs, [](QMessageBox *box) {
        QAbstractButton *replace = buttonOf(box, QStringLiteral("Replace"));
        QVERIFY(replace);
        QCOMPARE(shortcutOf(replace), QStringLiteral("R, Ctrl+R"));
        QCOMPARE(shortcutOf(buttonOf(box, QStringLiteral("Skip"))), QStringLiteral("S, Ctrl+S"));
        QCOMPARE(shortcutOf(buttonOf(box, QStringLiteral("Replace all"))),
                 QStringLiteral("A, Ctrl+A"));
        QCOMPARE(shortcutOf(buttonOf(box, QStringLiteral("Skip all"))),
                 QStringLiteral("L, Ctrl+L"));
        QCOMPARE(box->defaultButton(), qobject_cast<QPushButton *>(replace));
    });
    QCOMPARE(answer, FileOps::Existing::Cancel);
}

QTEST_MAIN(TestDialogs)
#include "dialogs.moc"
