#include "ViewerWindow.h"

#include "app/Dialogs.h"
#include "ops/Shell.h"

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr qsizetype kSniffBytes = 8192;  // this much is looked at to tell text from the rest

QString tr(const char *text)
{
    return QCoreApplication::translate("ViewerWindow", text);
}

// A zero byte in what was looked at: not text.
bool looksBinary(const QByteArray &bytes)
{
    return bytes.left(kSniffBytes).contains('\0');
}

// UTF-8 if it is that; else what the system uses for files.
QString decode(const QByteArray &bytes)
{
    const QString utf8 = QString::fromUtf8(bytes);
    if (!utf8.contains(QChar::ReplacementCharacter)) {
        return utf8;
    }
    return QString::fromLocal8Bit(bytes);
}

}  // namespace

ViewerWindow *ViewerWindow::open(const QString &path, Mode mode, Dialogs &dialogs, QWidget *parent)
{
    auto *window = new ViewerWindow(path, mode, dialogs, parent);
    window->show();
    return window;
}

ViewerWindow::ViewerWindow(const QString &path, Mode mode, Dialogs &dialogs, QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_path(QFileInfo(path).absoluteFilePath())
    , m_mode(mode)
    , m_dialogs(dialogs)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(760, 560);

    auto *layout = new QVBoxLayout(this);
    m_text = new QPlainTextEdit;
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    // Tab indents here, as in an editor; the cursor stays in the text.
    m_text->setTabChangesFocus(false);
    layout->addWidget(m_text, 1);

    auto *row = new QHBoxLayout;
    auto *wrap = new QToolButton;
    wrap->setText(tr("Wrap long lines"));
    wrap->setCheckable(true);
    wrap->setChecked(mode == Mode::View);
    const auto setWrap = [this](bool on) {
        m_text->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    };
    setWrap(wrap->isChecked());
    connect(wrap, &QToolButton::toggled, this, setWrap);
    row->addWidget(wrap);
    m_note = new QLabel;
    row->addWidget(m_note, 1);
    layout->addLayout(row);

    // The file. Saved as UTF-8, whatever it was, with the line endings it had.
    QFile file(m_path);
    const bool opened = file.open(QIODevice::ReadOnly);
    const QByteArray bytes = opened ? file.readAll() : QByteArray();
    m_isText = opened && !looksBinary(bytes);
    if (m_isText) {
        m_text->setPlainText(decode(bytes));
        if (bytes.contains("\r\n")) {
            m_lineEnding = "\r\n";
        }
    } else {
        m_text->setPlainText(opened ? tr("(Not a text file: it is not shown.)")
                                    : tr("(The file could not be read.)"));
        if (opened) {
            auto *openWith = new QToolButton;
            openWith->setText(tr("Open it with its application"));
            connect(openWith, &QToolButton::clicked, this, [this] {
                Shell::open(m_path);
                close();
            });
            row->addWidget(openWith);
            m_note->setText(tr("This file is not text."));
        } else {
            m_note->setText(file.errorString());
        }
    }
    m_text->setReadOnly(!m_isText || m_mode == Mode::View);
    m_text->document()->setModified(false);
    m_text->moveCursor(QTextCursor::Start);
    if (m_isText && m_mode == Mode::View) {
        m_note->setText(tr("Read-only. To change the file, choose Edit (F4)."));
    }

    connect(m_text->document(), &QTextDocument::modificationChanged, this, &ViewerWindow::updateTitle);

    auto *save = new QAction(tr("Save"), this);
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, [this] { ViewerWindow::save(); });
    addAction(save);

    updateTitle();
    m_text->setFocus();
}

bool ViewerWindow::isModified() const
{
    return m_text->document()->isModified() && !m_text->isReadOnly();
}

bool ViewerWindow::save()
{
    if (!isModified()) {
        return true;
    }
    // The text shows every line ending the same way; the file gets its own back.
    QByteArray bytes = m_text->toPlainText().toUtf8();
    if (m_lineEnding == QByteArrayLiteral("\r\n")) {
        bytes.replace("\n", "\r\n");
    }
    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) == -1) {
        m_dialogs.tell(tr("Save"), tr("%1 could not be saved: %2").arg(m_path, file.errorString()));
        return false;
    }
    m_text->document()->setModified(false);
    updateTitle();
    return true;
}

bool ViewerWindow::saveIfAsked()
{
    if (!isModified()) {
        return true;
    }
    const Dialogs::Answer answer = m_dialogs.ask(
            tr("Save changes?"), tr("%1 was changed. Save it?").arg(QFileInfo(m_path).fileName()));
    switch (answer) {
    case Dialogs::Answer::Yes:
        return save();
    case Dialogs::Answer::No:
        return true;
    case Dialogs::Answer::Cancel:
        return false;
    }
    return false;
}

void ViewerWindow::closeEvent(QCloseEvent *event)
{
    if (!saveIfAsked()) {
        event->ignore();
        return;
    }
    event->accept();
}

void ViewerWindow::updateTitle()
{
    setWindowTitle(QStringLiteral("%1%2 \u2014 %3")
                           .arg(isModified() ? QStringLiteral("* ") : QString(),
                                QFileInfo(m_path).fileName(),
                                m_mode == Mode::View ? tr("Viewer") : tr("Editor")));
}
