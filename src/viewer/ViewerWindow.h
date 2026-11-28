#pragma once

#include <QString>
#include <QWidget>

class Dialogs;
class QLabel;
class QPlainTextEdit;

// A window that shows a text file as it is, or lets it be changed.
class ViewerWindow : public QWidget {
    Q_OBJECT

public:
    enum class Mode {
        View,  // read-only
        Edit,
    };

    // Opens a window for `path`; it closes itself once the user is done.
    static ViewerWindow *open(const QString &path, Mode mode, Dialogs &dialogs, QWidget *parent);

    ViewerWindow(const QString &path, Mode mode, Dialogs &dialogs, QWidget *parent = nullptr);

    QString filePath() const { return m_path; }
    QPlainTextEdit *editor() const { return m_text; }
    // Whether the file is text and could be shown.
    bool isText() const { return m_isText; }
    // Whether the text was changed since it was read or last saved.
    bool isModified() const;
    // Writes the text to the file when it was changed; false when that failed.
    bool save();
    // Asks whether to save when there are changes. False when the user chose
    // to stay, or saving failed.
    bool saveIfAsked();

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    // How the file's bytes and its text go together.
    enum class Encoding {
        Utf8,
        Latin1,  // a file that isn't UTF-8: every byte is one character
    };
    static QString decode(const QByteArray &bytes, Encoding *encoding);

    void updateTitle();

    QString m_path;
    Mode m_mode;
    Dialogs &m_dialogs;
    QPlainTextEdit *m_text;
    QLabel *m_note;
    QByteArray m_lineEnding = "\n";  // what the file had: kept when it is saved
    Encoding m_encoding = Encoding::Utf8;  // likewise
    bool m_isText = true;
};
