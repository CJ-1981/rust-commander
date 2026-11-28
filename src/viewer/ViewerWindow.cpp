#include "ViewerWindow.h"

#include "app/Dialogs.h"
#include "ops/Shell.h"

#include <QAction>
#include <QCloseEvent>
#include <QStringDecoder>
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

#include <algorithm>

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

}  // namespace

QString ViewerWindow::decode(const QByteArray &bytes, Encoding *encoding)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString utf8 = decoder(bytes);
    if (!decoder.hasError()) {
        *encoding = Encoding::Utf8;
        return utf8;
    }
    // Taken byte for byte. Whatever the file's real encoding, nothing is
    // replaced: written back the same way, it is as it was. (The letters of
    // another encoding than Latin-1 just don't read right.)
    *encoding = Encoding::Latin1;
    return QString::fromLatin1(bytes);
}

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

    // The file, read as it is encoded; saved the same way.
    QFile file(m_path);
    const bool opened = file.open(QIODevice::ReadOnly);
    const QByteArray bytes = opened ? file.readAll() : QByteArray();
    m_isText = opened && !looksBinary(bytes);
    if (m_isText) {
        m_text->setPlainText(decode(bytes, &m_encoding));
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
    QByteArray bytes;
    const QString text = m_text->toPlainText();
    if (m_encoding == Encoding::Latin1
        && std::any_of(text.begin(), text.end(), [](QChar c) { return c.unicode() > 0xff; })) {
        // Something was typed that the file's own encoding has no byte for.
        // Rather than lose it, the file becomes UTF-8.
        m_encoding = Encoding::Utf8;
        m_note->setText(tr("Saved as UTF-8: the new text does not fit the encoding the file had."));
        bytes = text.toUtf8();
    } else if (m_encoding == Encoding::Latin1) {
        bytes = text.toLatin1();
    } else {
        bytes = text.toUtf8();
    }
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

// This window is no QDialog, so Esc would reach it and do nothing. It closes
// like every other window of the app; with changes, the editor asks first,
// as a close with the mouse does.
void ViewerWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        close();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ViewerWindow::updateTitle()
{
    setWindowTitle(QStringLiteral("%1%2 \u2014 %3")
                           .arg(isModified() ? QStringLiteral("* ") : QString(),
                                QFileInfo(m_path).fileName(),
                                m_mode == Mode::View ? tr("Viewer") : tr("Editor")));
}
