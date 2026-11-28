#pragma once

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include "tools/RenameRules.h"

class QCheckBox;
class QComboBox;
class Dialogs;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTimer;
class QWidget;

class PreviewModel;

// Total Commander's multi-rename tool: many files get a new name from a mask
// and a few rules. Before anything on disk is touched, the table shows every
// file next to what it would be called, and what keeps a file from being
// renamed. After a rename it says what became of each file, and Undo gives
// the files their names back, batch by batch.
//
// The window asks nothing: the preview is the question, and the table is the
// answer. It deletes itself when it is closed.
class MultiRenameWindow : public QDialog {
    Q_OBJECT

public:
    // What the table says about a file.
    enum class State {
        Rename,         // it will be renamed
        Unchanged,      // the rules leave its name as it is
        Problem,        // it can't be renamed; `message` says why
        Renamed,        // it was renamed
        Failed,         // renaming it failed; `message` says why
        Restored,       // Undo gave it its old name back
        RestoreFailed,  // that failed; `message` says why
    };

    // One row of the table.
    struct Row {
        QString name;     // the file's name; after a rename or an Undo: the one it had before
        QString path;     // where the file is now
        QString newName;  // what the rules make of its name, or what it was renamed to
        State state = State::Problem;
        QString message;  // Problem, Failed and RestoreFailed: why
    };

    // Opens a window for `files`; it closes itself once the user is done.
    static MultiRenameWindow *open(const QStringList &files, Dialogs &dialogs, QWidget *parent = nullptr);

    // `dialogs` is what every window of the app takes. This one has no use for it.
    explicit MultiRenameWindow(Dialogs &dialogs, QWidget *parent = nullptr);

    // The files to rename. The table shows them, and the counter counts them,
    // in this order; a file that is in the list twice is in the table once.
    void setFiles(const QStringList &files);
    // Where the files are now: those that were renamed have their new paths.
    QStringList files() const;

    // The rules. A change shows in the table at once.
    void setMask(const QString &mask);
    void setFind(const QString &find);
    void setReplace(const QString &replace);
    void setRegularExpression(bool on);
    void setCase(RenameRules::Case mode);
    // The counter goes up by one for each file; its digits at least.
    void setCounterStart(int start);
    void setCounterWidth(int width);
    void setUnderscoresToSpaces(bool on);
    RenameRules::Settings settings() const;
    void resetSettings();

    // The table, row by row, as it is shown.
    QList<Row> rows() const;
    // The rows that will be renamed, and those that can't be.
    int renameCount() const;
    int problemCount() const;
    // What the line under the table says: how many files will be renamed, or
    // what is wrong with the rules, or how the last rename went.
    QString statusText() const;
    // Whether there is a batch that Undo can give back.
    bool canUndo() const;

    // Does what the Rename button does: renames the rows that can be, one
    // after the other. A file that can't be renamed stops nothing; its row
    // says why. Returns how many files were renamed.
    int renameAll();
    // Does what the Undo button does: gives the last batch its old names back,
    // the file that was renamed last first. Returns how many files were.
    int undo();

    // While the rules are being typed, the table follows shortly after the
    // last change; this makes it follow now. Everything above already does.
    void updatePreview();

    // The fields, the table and the buttons, for tests and for a look.
    QLineEdit *maskField() const { return m_mask; }
    QLineEdit *findField() const { return m_find; }
    QLineEdit *replaceField() const { return m_replace; }
    QCheckBox *regularExpressionBox() const { return m_regularExpression; }
    QComboBox *caseBox() const { return m_case; }
    QLineEdit *counterStartField() const { return m_counterStart; }
    QLineEdit *counterWidthField() const { return m_counterWidth; }
    QCheckBox *underscoresBox() const { return m_underscores; }
    QTableView *table() const { return m_table; }
    QPushButton *renameButton() const { return m_renameButton; }
    QPushButton *undoButton() const { return m_undoButton; }
    QLabel *statusLabel() const { return m_status; }

signals:
    // Files were renamed, or got their names back: what their folders show has changed.
    void filesChanged();

private:
    // A file as it is on disk.
    struct File {
        QString path;
        QString folder;
        QString name;
        QDateTime modified;
        bool exists = false;
    };

    // A rename that was made, to be undone.
    struct Renaming {
        QString path;     // where the file is now
        QString oldName;  // what it was called
    };

    // What became of a row in the last rename or Undo. It is shown until the
    // rules change.
    struct Outcome {
        State state = State::Failed;
        QString message;
        QString before;  // the name the file had
        QString after;   // the name it got, or was to get
    };

    void buildUi();
    // Reads the rules from the fields and works out every row. The outcomes
    // of a rename or an Undo are then shown on their rows, until the rules change.
    void rebuild(const QHash<int, Outcome> &outcomes = {});
    // The fields changed: follow shortly.
    void scheduleUpdate();
    // What a change of the rules, or a rename, means for the status line and the buttons.
    void updateStatus();
    int rowOf(const QString &path) const;
    // The number a counter field stands for; one that isn't a number yet is 1.
    static int number(QLineEdit *field, bool atLeastOne);

    friend class PreviewModel;
    QList<File> m_files;
    QList<Row> m_rows;
    QHash<QString, int> m_rowOfPath;         // path -> row
    QList<QList<Renaming>> m_batches;        // what Undo can give back, the last first
    RenameRules::Settings m_settings;
    bool m_updatePending = false;

    QLineEdit *m_mask = nullptr;
    QLineEdit *m_find = nullptr;
    QLineEdit *m_replace = nullptr;
    QCheckBox *m_regularExpression = nullptr;
    QComboBox *m_case = nullptr;
    QLineEdit *m_counterStart = nullptr;
    QLineEdit *m_counterWidth = nullptr;
    QCheckBox *m_underscores = nullptr;
    PreviewModel *m_model = nullptr;
    QTableView *m_table = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_undoButton = nullptr;
    QPushButton *m_renameButton = nullptr;
    QTimer *m_timer = nullptr;
};
