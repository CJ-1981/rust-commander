#pragma once

#include <QBasicTimer>
#include <QLabel>
#include <QList>
#include <QStringList>
#include <QTableView>
#include <QUrl>

class FileProxyModel;
class QLabel;
class QContextMenuEvent;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QFocusEvent;
class QKeyEvent;
class QMimeData;
class QMouseEvent;
class QPaintEvent;
class QTimerEvent;

// The file table of one pane: selecting with mouse and keyboard, opening
// items, and drag and drop to and from other applications.
//
// Mouse: a click selects one row; Ctrl-click toggles and Shift-click extends;
// pressing an unselected row and moving the pointer selects the rows in
// between; pressing empty space draws a rubber band over the rows it touches;
// a double-click opens.
// Keyboard: arrows, Shift-arrows, Home/End, PgUp/PgDn; Insert and Space mark
// the current row and move down; Enter opens; Backspace goes up. Ctrl-arrows
// and Ctrl-Space move the cursor and toggle without touching the marks.
class FileView : public QTableView {
    Q_OBJECT

public:
    explicit FileView(QWidget *parent = nullptr);

    void setFileModel(FileProxyModel *model);
    FileProxyModel *fileModel() const { return m_model; }

    // Paths of the selected rows, top to bottom.
    QStringList selectedPaths() const;
    // Path of the row under the cursor; if there is none, of the first
    // selected row.
    QString currentPath() const;

    // Whether the selected items can be dragged out of the view. Dragging
    // starts only from a press on a row that was already selected.
    void setDragOutEnabled(bool enabled);
    bool isDragOutEnabled() const { return dragEnabled(); }

    // The row highlighted as the target of a drag hovering over it, or -1.
    int dropRow() const { return m_dropRow; }

    // Whether a drop of `mime` onto `targetFolder` would do something.
    static bool dropIsPossible(const QMimeData &mime, const QString &targetFolder);

    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

signals:
    // Enter or a double-click on a row.
    void openRequested(const QModelIndex &index);
    void upRequested();
    // The user asked for the context menu; the position is in the viewport.
    void contextMenuRequested(const QPoint &position);
    // The cursor marked a folder: the pane asks the model for its size.
    void sizeRequested(const QString &path);
    // Files were dropped onto `targetFolder`. `ask` is set when they come from
    // this app and no modifier chose between copying and moving.
    void dropped(const QList<QUrl> &urls, const QString &targetFolder, Qt::DropAction action, bool ask);
    // A drag of the selected items is starting; it ends when the drag ends.
    void dragStarted();
    // The user worked in this view.
    void activated();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    QModelIndex moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers) override;
    void focusInEvent(QFocusEvent *event) override;
    void timerEvent(QTimerEvent *event) override;

    void startDrag(Qt::DropActions supportedActions) override;
    // Runs the drag once it has been decided. Separated out so that tests can
    // see what would be dragged without starting a system drag.
    virtual void performDrag(QMimeData *mime, const QPixmap &image, const QPoint &hotSpot);
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // What a press starts, decided on press, before the base class selects
    // the row.
    enum class Press {
        None,    // a modifier click or another button: dragging extends the
                 // selection, as far as the base class takes it
        Sweep,   // press on an unselected row: dragging selects the rows it
                 // passes over, not a file drag
        Drag,    // press on a selected row: dragging drags the selection
        Rubber,  // press on empty space: dragging draws a rubber band
    };

    // Marks or unmarks the current row and moves the cursor down.
    void toggleCurrent();
    void toggleInPlace();
    // Selects the rows from the pressed one to the one at `position`, also
    // when that is below the last row.
    void sweepTo(const QPoint &position);
    // Hides the columns that don't fit when the pane is narrow.
    void updateColumns();
    // Selects the rows the rubber band touches.
    void selectRubberRows();
    void setDropRow(int row);
    // Shared by dragEnterEvent and dragMoveEvent.
    void updateDrag(QDragMoveEvent *event);
    void updateDropRow(const QPoint &position, const QMimeData &mime);
    // The folder a drop at viewport `position` would land in.
    QString dropTarget(const QPoint &position) const;
    // Rect of the row of `index` in the coordinates the delegate paints in.
    QRect rowRect(const QModelIndex &index) const;

    // Type-ahead quick search, as in Total Commander: typing a letter opens
    // a small field over the list, typing goes on narrowing, Enter keeps
    // where it found, Escape puts the cursor back.
    void openQuickSearch(const QChar &first);
    void quickSearchAppend(const QChar &next);
    void quickSearchBackspace();
    void quickSearchClose(bool restore);
    void quickSearchFind(bool fromStart);
    // Whether a name answers to the pattern: substring, or wildcards when
    // the pattern has * or ?.
    static bool quickMatches(const QString &name, const QString &pattern);

    FileProxyModel *m_model = nullptr;
    Press m_press = Press::None;
    QPoint m_pressPos;
    QLabel *m_quickSearch = nullptr;   // the field with the pattern, over the list
    QString m_quickPattern;
    int m_quickStart = -1;             // where the cursor was when the search began
    int m_quickCursor = -1;            // where it stands now, in the search
    QRect m_rubber;
    QPoint m_pointer;
    QBasicTimer m_scrollTimer;  // scrolls while a rubber band touches an edge
    int m_dropRow = -1;         // proxy row highlighted as drop target
};
