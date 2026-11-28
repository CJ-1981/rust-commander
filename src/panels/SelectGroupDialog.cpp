#include "SelectGroupDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

SelectGroupDialog::SelectGroupDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Select group"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Mark what the name answers to. Several patterns"
                                    " are separated by spaces; * and ? are wildcards.")));

    m_patterns = new QLineEdit;
    m_patterns->setPlaceholderText(QStringLiteral("*.jpg report-2024*"));
    layout->addWidget(m_patterns);

    auto *what = new QHBoxLayout;
    m_files = new QRadioButton(tr("&Files"));
    m_folders = new QRadioButton(tr("&Folders"));
    m_both = new QRadioButton(tr("&Both"));
    m_files->setChecked(true);
    what->addWidget(m_files);
    what->addWidget(m_folders);
    what->addWidget(m_both);
    what->addStretch(1);
    layout->addLayout(what);

    m_buttons = new QDialogButtonBox;
    m_mark = m_buttons->addButton(tr("&Mark"), QDialogButtonBox::AcceptRole);
    m_unmark = m_buttons->addButton(tr("&Unmark"), QDialogButtonBox::AcceptRole);
    QPushButton *const mark = m_mark;
    QPushButton *const unmark = m_unmark;
    m_buttons->addButton(QDialogButtonBox::Cancel);
    connect(mark, &QPushButton::clicked, this, [this] {
        m_select = true;
        accept();
    });
    connect(unmark, &QPushButton::clicked, this, [this] {
        m_select = false;
        accept();
    });
    layout->addWidget(m_buttons);

    m_patterns->setFocus();
}

QStringList SelectGroupDialog::patterns() const
{
    return m_patterns->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

SelectGroupDialog::What SelectGroupDialog::what() const
{
    if (m_folders->isChecked()) {
        return What::Folders;
    }
    if (m_both->isChecked()) {
        return What::Both;
    }
    return What::Files;
}
