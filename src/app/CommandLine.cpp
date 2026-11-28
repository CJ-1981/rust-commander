#include "CommandLine.h"

#include <QCompleter>
#include <QDir>
#include <QEvent>
#include <QFileSystemModel>
#include <QFileInfo>
#include <QKeyEvent>

namespace {

// Words of a command line, with quotes kept together: splitArguments("app
// \"my folder\" -x") is app, my folder, -x.
QStringList splitArguments(const QString &text)
{
    QStringList words;
    QString word;
    bool inQuotes = false;
    bool started = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == u'"') {
            inQuotes = !inQuotes;
            started = true;
            continue;
        }
        if (c.isSpace() && !inQuotes) {
            if (started) {
                words.append(word);
                word.clear();
                started = false;
            }
            continue;
        }
        word.append(c);
        started = true;
    }
    if (started) {
        words.append(word);
    }
    return words;
}

// A path as the field was given it, with ~ and relative paths resolved.
QString resolvePath(const QString &typed, const QString &folder)
{
    QString text = typed.trimmed();
    if (text.startsWith(QLatin1Char('~'))) {
        text = QDir::homePath() + text.mid(1);
    }
    return QDir::cleanPath(QDir::isAbsolutePath(text) ? text : QDir(folder).absoluteFilePath(text));
}

}  // namespace

CommandLine::CommandLine(QWidget *parent)
    : QLineEdit(parent)
{
    setObjectName("commandLine");
    setClearButtonEnabled(true);
    setPlaceholderText(tr("Command"));
    setToolTip(tr("A path or a command: cd, view, edit, a file to open, a program to start"));
    setMaximumWidth(420);

    // What can be named here: the files and folders of the folder shown. The
    // main window says which folder that is as the field is used.
    auto *files = new QFileSystemModel(this);
    files->setRootPath(QDir::rootPath());
    auto *completer = new QCompleter(files, this);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    setCompleter(completer);

    connect(this, &QLineEdit::returnPressed, this, [this] { emit commandEntered(text()); });
}

void CommandLine::setHistory(const QStringList &history)
{
    m_history = history;
    m_historyAt = m_history.size();
}

void CommandLine::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        clear();
        emit focusRequested();
        return;
    }
    if (event->key() == Qt::Key_Up) {
        stepHistory(-1);
        return;
    }
    if (event->key() == Qt::Key_Down) {
        stepHistory(1);
        return;
    }
    QLineEdit::keyPressEvent(event);
}

void CommandLine::focusInEvent(QFocusEvent *event)
{
    QLineEdit::focusInEvent(event);
    selectAll();
}

void CommandLine::stepHistory(int step)
{
    if (m_history.isEmpty()) {
        return;
    }
    // First step up from what is typed: it is kept, and comes back on the
    // way down past the newest entry.
    if (m_historyAt == m_history.size()) {
        m_draft = text();
    }
    const int at = m_historyAt + step;
    if (at < 0 || at > m_history.size()) {
        return;
    }
    m_historyAt = at;
    setText(at == m_history.size() ? m_draft : m_history.at(at));
}

QString CommandLine::unquoted(const QString &text)
{
    QString clean = text.trimmed();
    if (clean.size() >= 2 && clean.startsWith(QLatin1Char('"')) && clean.endsWith(QLatin1Char('"'))) {
        clean = clean.mid(1, clean.size() - 2);
    }
    return clean;
}

bool CommandLine::parse(const QString &line, const QString &folder, Command *command,
                        QString *error)
{
    *error = QString();
    *command = Command();
    const QString typed = line.trimmed();
    if (typed.isEmpty()) {
        return false;
    }

    // cd view edit: the words the field itself answers for.
    QString rest;
    Kind kind = Kind::Launch;
    QString word;
    const auto takes = [&](const char *key, Kind what) {
        const QString head = QLatin1String(key);
        if (typed.compare(head, Qt::CaseInsensitive) == 0) {
            word = head;
            rest = QString();
            kind = what;
            return true;
        }
        if (typed.size() > head.size() && typed.at(head.size()).isSpace()
            && typed.left(head.size()).compare(head, Qt::CaseInsensitive) == 0) {
            word = head;
            rest = typed.mid(head.size() + 1).trimmed();
            kind = what;
            return true;
        }
        return false;
    };
    if (takes("cd", Kind::Navigate) || takes("view", Kind::View) || takes("edit", Kind::Edit)) {
        if (rest.isEmpty()) {
            *error = CommandLine::tr("Name a folder or a file after %1.").arg(word);
            return false;
        }
        command->kind = kind;
        command->path = resolvePath(unquoted(rest), folder);
        return true;
    }

    // A path that is there: a folder to enter, a file to open.
    const QString asPath = resolvePath(unquoted(typed), folder);
    const QFileInfo info(asPath);
    if (info.exists()) {
        command->kind = info.isDir() ? Kind::Navigate : Kind::Open;
        command->path = asPath;
        return true;
    }

    // Anything else is a program to start, with its arguments.
    const QStringList words = splitArguments(typed);
    if (words.isEmpty()) {
        *error = CommandLine::tr("Nothing to run.");
        return false;
    }
    command->kind = Kind::Launch;
    command->program = words.first();
    command->arguments = words.mid(1);
    return true;
}
