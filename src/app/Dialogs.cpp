#include "Dialogs.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QWidget>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Dialogs", text);
}

}  // namespace

std::optional<QString> GuiDialogs::askText(const QString &title, const QString &label,
                                            const QString &value)
{
    bool ok = false;
    const QString text = QInputDialog::getText(m_parent, title, label, QLineEdit::Normal, value, &ok);
    if (!ok) {
        return std::nullopt;
    }
    return text;
}

Dialogs::Answer GuiDialogs::ask(const QString &title, const QString &text)
{
    QMessageBox box(m_parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(title);
    box.setText(text);
    auto *yes = box.addButton(QMessageBox::Yes);
    auto *no = box.addButton(QMessageBox::No);
    auto *cancel = box.addButton(QMessageBox::Cancel);
    // Yes is what the keys say: Y, Cmd+Y, or plain Return. N says no, and
    // Esc stays where it always was, on Cancel. The shortcuts are the
    // dialog's alone, so they stand in no one's way elsewhere.
    yes->setShortcut(QKeySequence(QStringLiteral("Y, Ctrl+Y")));
    no->setShortcut(QKeySequence(QStringLiteral("N, Ctrl+N")));
    box.setDefaultButton(yes);
    box.exec();

    QAbstractButton *clicked = box.clickedButton();
    if (clicked == yes) {
        return Answer::Yes;
    }
    if (clicked == no) {
        return Answer::No;
    }
    Q_UNUSED(cancel)
    return Answer::Cancel;
}

void GuiDialogs::tell(const QString &title, const QString &text)
{
    QMessageBox::warning(m_parent, title, text);
}

FileOps::Existing GuiDialogs::askOverwrite(const QString &target)
{
    QMessageBox box(m_parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Already there"));
    box.setText(tr("Something with this name is already there. Replace it?"));
    box.setInformativeText(target);
    box.setStandardButtons(QMessageBox::Cancel);
    QAbstractButton *replace = box.addButton(tr("Replace"), QMessageBox::ActionRole);
    QAbstractButton *skip = box.addButton(tr("Skip"), QMessageBox::ActionRole);
    QAbstractButton *replaceAll = box.addButton(tr("Replace all"), QMessageBox::ActionRole);
    QAbstractButton *skipAll = box.addButton(tr("Skip all"), QMessageBox::ActionRole);
    // One letter per answer, Esc for Cancel; the shortcuts live only while
    // the question is up.
    replace->setShortcut(QKeySequence(QStringLiteral("R, Ctrl+R")));
    skip->setShortcut(QKeySequence(QStringLiteral("S, Ctrl+S")));
    replaceAll->setShortcut(QKeySequence(QStringLiteral("A, Ctrl+A")));
    skipAll->setShortcut(QKeySequence(QStringLiteral("L, Ctrl+L")));
    if (auto *pushReplace = qobject_cast<QPushButton *>(replace)) {
        box.setDefaultButton(pushReplace);
    }
    box.exec();

    QAbstractButton *clicked = box.clickedButton();
    if (clicked == replace) {
        return FileOps::Existing::Overwrite;
    }
    if (clicked == skip) {
        return FileOps::Existing::Skip;
    }
    if (clicked == replaceAll) {
        return FileOps::Existing::OverwriteAll;
    }
    if (clicked == skipAll) {
        return FileOps::Existing::SkipAll;
    }
    return FileOps::Existing::Cancel;
}
