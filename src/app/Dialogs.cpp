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

QString GuiDialogs::askText(const QString &title, const QString &label, const QString &value)
{
    bool ok = false;
    const QString text = QInputDialog::getText(m_parent, title, label, QLineEdit::Normal, value, &ok);
    return ok ? text : QString();
}

Dialogs::Answer GuiDialogs::ask(const QString &title, const QString &text)
{
    const auto answer = QMessageBox::question(m_parent, title, text,
                                              QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                                              QMessageBox::Cancel);
    switch (answer) {
    case QMessageBox::Yes:
        return Answer::Yes;
    case QMessageBox::No:
        return Answer::No;
    default:
        return Answer::Cancel;
    }
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
