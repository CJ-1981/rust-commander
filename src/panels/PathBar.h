#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QCompleter;
class QLineEdit;
class QStackedLayout;

// The line above a pane's file table: the folder as a row of buttons to click
// through (the volume, then each folder), which turns into a field to type a
// path in when it is clicked, or typed into.
class PathBar : public QWidget {
    Q_OBJECT

public:
    explicit PathBar(QWidget *parent = nullptr);
    ~PathBar() override;

    QString path() const { return m_path; }
    void setPath(const QString &path);

    // The folders shown, from the volume to the current folder. When the bar
    // is too narrow, the outer ones are not drawn.
    QStringList folders() const;
    // Where the folder at `index` is drawn, in the bar's coordinates.
    QRect folderRect(int index) const;

    // Whether a path is being typed.
    bool isEditing() const;

    QSize sizeHint() const override;

signals:
    // A folder was chosen: by clicking one of the buttons, or by typing a path.
    void pathChosen(const QString &path);
    // Typing the path began.
    void editingStarted();
    // Typing the path ended. `chosen` says whether a path came of it.
    void editingEnded(bool chosen);

public slots:
    // Shows the field with the whole path selected.
    void edit();
    // Back to the buttons; what was typed is dropped.
    void stopEditing();

private:
    class Crumbs;

    void typed(const QString &text);
    void accept();
    void finish(bool chosen);

protected:
    void keyPressEvent(QKeyEvent *event) override;

    Crumbs *m_crumbs;
    QLineEdit *m_edit;
    QStackedLayout *m_layout;
    QString m_path;
    bool m_editing = false;  // not the layout's page: hiding the field must not end the editing twice
};
