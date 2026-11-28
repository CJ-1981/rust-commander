#pragma once

#include <QDialog>

class QDialogButtonBox;
class QPushButton;

class QLineEdit;
class QRadioButton;

// What a group of names is, and whether it is to be marked or unmarked:
// Total Commander's gray plus and minus. Patterns are wildcards, several of
// them separated by spaces, and the group can be files, folders or both.
class SelectGroupDialog : public QDialog {
    Q_OBJECT

public:
    explicit SelectGroupDialog(QWidget *parent = nullptr);

    // What was asked for. `select` says mark (true) or unmark (false).
    QStringList patterns() const;
    bool select() const { return m_select; }
    enum class What { Files, Folders, Both };
    What what() const;

    // Fields, for tests and for opening with what was asked before.
    QLineEdit *patternsField() const { return m_patterns; }
    QDialogButtonBox *dialogButtonBox() const { return m_buttons; }
    QPushButton *markButton() const { return m_mark; }
    QPushButton *unmarkButton() const { return m_unmark; }
    QRadioButton *filesBox() const { return m_files; }
    QRadioButton *foldersBox() const { return m_folders; }
    QRadioButton *bothBox() const { return m_both; }

private:
    QDialogButtonBox *m_buttons;
    QPushButton *m_mark = nullptr;
    QPushButton *m_unmark = nullptr;
    QLineEdit *m_patterns;
    QRadioButton *m_files;
    QRadioButton *m_folders;
    QRadioButton *m_both;
    bool m_select = true;
};
