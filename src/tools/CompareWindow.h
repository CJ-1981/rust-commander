#pragma once

#include <QAbstractItemModel>
#include <QDialog>
#include <QList>
#include <QStringList>

#include "tools/LineDiff.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class DiffModel;
class Dialogs;

// Two texts side by side: what is the same, what is only on the left or on
// the right, and what changed, with the parts of a changed line marked. The
// files are read here; the comparison itself needs no window and runs where
// compare() is called from.
class CompareWindow : public QDialog {
    Q_OBJECT

public:
    // Opens a window over two files and compares them right away; it deletes
    // itself when closed.
    static CompareWindow *open(const QString &left, const QString &right, Dialogs &dialogs,
                               QWidget *parent = nullptr);

    explicit CompareWindow(Dialogs &dialogs, QWidget *parent = nullptr);

    // Reads the two files and compares them. False when one of them could not
    // be read, or is not text; the status line says which one and why.
    bool compare(const QString &left, const QString &right);

    // What the comparison found.
    QList<LineDiff::Row> rows() const;
    int differences() const;

    // For the tests: the fields, the buttons and the line at the bottom.
    QLineEdit *leftField() const { return m_left; }
    QLineEdit *rightField() const { return m_right; }
    QCheckBox *whitespaceBox() const { return m_whitespace; }
    QCheckBox *lineEndingsBox() const { return m_lineEndings; }
    QCheckBox *blankBox() const { return m_blank; }
    QPushButton *compareButton() const { return m_compare; }
    QPushButton *nextButton() const { return m_next; }
    QPushButton *previousButton() const { return m_previous; }
    QPushButton *swapButton() const { return m_swap; }
    QLabel *statusLabel() const { return m_status; }
    QTableView *table() const { return m_table; }

signals:
    void swapped();

protected:
    void showEvent(QShowEvent *event) override;

private:
    // Scrolls to the next or previous row that is a difference.
    void gotoDifference(int step);
    // The row the cursor is on, or -1.
    int currentDifference() const;

    Dialogs &m_dialogs;
    QLineEdit *m_left = nullptr;
    QLineEdit *m_right = nullptr;
    QCheckBox *m_whitespace = nullptr;
    QCheckBox *m_lineEndings = nullptr;
    QCheckBox *m_blank = nullptr;
    QPushButton *m_compare = nullptr;
    QPushButton *m_next = nullptr;
    QPushButton *m_previous = nullptr;
    QPushButton *m_swap = nullptr;
    QLabel *m_status = nullptr;
    QTableView *m_table = nullptr;
    DiffModel *m_model = nullptr;
};
