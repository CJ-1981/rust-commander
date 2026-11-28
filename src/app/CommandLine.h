#pragma once

#include <QLineEdit>
#include <QStringList>

class QCompleter;

// The field at the right of the key row: a path or a command, as in Total
// Commander. The arrow keys walk the history of what was run, Escape gives
// the keyboard back to the file list, and what Enter says is told to the
// main window, which carries it out.
class CommandLine : public QLineEdit {
    Q_OBJECT

public:
    explicit CommandLine(QWidget *parent = nullptr);

    // What a typed line asks for. `folder` is where a relative path is taken
    // from and where completion looks. An empty line asks for nothing; what
    // cannot be understood is said in `error`.
    enum class Kind {
        Navigate,  // go to a folder: `cd`, or a path that is a folder
        View,      // `view <file>`: the read-only viewer
        Edit,      // `edit <file>`: the editor
        Open,      // a file that is there: with its application
        Launch,    // the first word is a program, the rest its arguments
    };
    struct Command {
        Kind kind = Kind::Launch;
        QString path;          // for Navigate, View, Edit, Open
        QString program;       // for Launch
        QStringList arguments; // for Launch
    };
    static bool parse(const QString &line, const QString &folder, Command *command,
                      QString *error);

    // What Up and Down walk through.
    void setHistory(const QStringList &history);
    QStringList history() const { return m_history; }

signals:
    // A line was entered; the main window parses and runs it.
    void commandEntered(const QString &line);
    // Escape: the file list is to have the keyboard back.
    void focusRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;

private:
    void stepHistory(int step);
    // The text of a path without the quotes around it.
    static QString unquoted(const QString &text);

    QStringList m_history;
    int m_historyAt = 0;   // where in the history the field stands
    QString m_draft;       // what was typed before the history was opened
};
