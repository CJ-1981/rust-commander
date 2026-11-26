#include "FileView.h"

#include "FileModel.h"
#include "FileProxyModel.h"
#include "app/Theme.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTimerEvent>
#include <QUrl>

#include <algorithm>
#include <functional>

namespace {

constexpr int kEdgeMargin = 24;      // distance from an edge at which a rubber band scrolls
constexpr int kScrollInterval = 60;  // ms between scroll steps
constexpr int kMaxDragImageRows = 6;
constexpr int kMinNameWidth = 150;  // what the Name column keeps in a narrow pane
constexpr int kScrollBarWidth = 14;  // room left for a scrollbar

// Paths differ in case only on Windows.
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

bool samePath(const QString &a, const QString &b)
{
    return a.compare(b, kPathCase) == 0;
}

// Whether `path` is `folder` itself or lies inside it.
bool isAtOrInside(const QString &path, const QString &folder)
{
    return samePath(path, folder) || path.startsWith(folder + QLatin1Char('/'), kPathCase);
}

QStringList localFiles(const QMimeData &mime)
{
    QStringList paths;
    for (const QUrl &url : mime.urls()) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    return paths;
}

// Whether `folder` is one of `paths`, or lies inside one of them (those
// being folders): a folder can't go into itself or into its own contents.
bool targetInsideDragged(const QStringList &paths, const QString &folder)
{
    for (const QString &path : paths) {
        if (isAtOrInside(folder, path)) {
            return true;
        }
    }
    return false;
}

class FileDelegate : public QStyledItemDelegate {
public:
    // `cursorColor` tells which color to outline a row with; an invalid
    // color means no outline.
    FileDelegate(QObject *parent, std::function<QColor(int)> cursorColor,
                 std::function<QRect(const QModelIndex &)> rowRect)
        : QStyledItemDelegate(parent)
        , m_cursorColor(std::move(cursorColor))
        , m_rowRect(std::move(rowRect))
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        // Only names keep their end in view when they don't fit; for a type
        // such as "Portable Document Format" the start matters more.
        if (index.column() != FileModel::NameColumn) {
            opt.textElideMode = Qt::ElideRight;
        }
        const QWidget *widget = opt.widget;
        const QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        // An outline marks the cursor row while it isn't selected, so that it
        // can be seen where the arrow keys will act. It is drawn over the whole
        // row; each cell shows the part that falls on it.
        const QColor line = m_cursorColor(index.row());
        if (!line.isValid() || (opt.state & QStyle::State_Selected)) {
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(line, 1.0));
        painter->drawRoundedRect(QRectF(m_rowRect(index)).adjusted(1.5, 1.5, -1.5, -1.5), 4, 4);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), option.fontMetrics.height() + 10));
        return size;
    }

private:
    std::function<QColor(int)> m_cursorColor;
    std::function<QRect(const QModelIndex &)> m_rowRect;
};

}  // namespace

FileView::FileView(QWidget *parent)
    : QTableView(parent)
{
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
    setDropIndicatorShown(false);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setShowGrid(false);
    setWordWrap(false);
    setAlternatingRowColors(true);
    // Keep the end of a long name, where the extension is.
    setTextElideMode(Qt::ElideMiddle);
    verticalHeader()->hide();
    horizontalHeader()->setHighlightSections(false);

    setItemDelegate(new FileDelegate(this,
                                     [this](int row) -> QColor {
                                         if (currentIndex().row() != row) {
                                             return QColor();
                                         }
                                         if (hasFocus()) {
                                             return Theme::accent();
                                         }
                                         QColor muted = Theme::muted();
                                         muted.setAlpha(150);
                                         return muted;
                                     },
                                     [this](const QModelIndex &index) { return rowRect(index); }));
}

void FileView::setFileModel(FileProxyModel *model)
{
    m_model = model;
    setModel(model);

    QHeaderView *header = horizontalHeader();
    header->setSectionResizeMode(FileModel::NameColumn, QHeaderView::Stretch);
    header->resizeSection(FileModel::SizeColumn, 92);
    header->resizeSection(FileModel::TypeColumn, 196);
    header->resizeSection(FileModel::ModifiedColumn, 136);
    // Clicking a header sorts by that column. A new header indicates
    // "descending", so say "ascending" first or the list would start upside
    // down.
    header->setSortIndicator(FileModel::NameColumn, Qt::AscendingOrder);
    setSortingEnabled(true);
    updateColumns();
}

QStringList FileView::selectedPaths() const
{
    QStringList paths;
    if (!selectionModel()) {
        return paths;
    }
    QModelIndexList rows = selectionModel()->selectedRows(FileModel::NameColumn);
    // The selection model keeps its ranges in the order they came about, not
    // the order they are shown in.
    std::sort(rows.begin(), rows.end(),
              [](const QModelIndex &a, const QModelIndex &b) { return a.row() < b.row(); });
    for (const QModelIndex &index : rows) {
        if (const FileEntry *entry = m_model->entry(index)) {
            paths.append(entry->path);
        }
    }
    return paths;
}

QString FileView::currentPath() const
{
    if (const FileEntry *entry = m_model->entry(currentIndex())) {
        return entry->path;
    }
    return selectedPaths().value(0);
}

void FileView::setDragOutEnabled(bool enabled)
{
    setDragEnabled(enabled);
}

bool FileView::dropIsPossible(const QMimeData &mime, const QString &targetFolder)
{
    const QStringList paths = localFiles(mime);
    if (paths.isEmpty() || targetFolder.isEmpty()) {
        return false;
    }
    // Dragged out of the very folder they would land in: nothing to do.
    const QString source = QString::fromUtf8(mime.data(dragSourceFormat));
    if (!source.isEmpty() && samePath(source, targetFolder)) {
        return false;
    }
    return !targetInsideDragged(paths, targetFolder);
}

QRect FileView::rowRect(const QModelIndex &index) const
{
    // Cell rects, as the delegate paints them, are scrolled by the offsets
    // of the headers.
    const int left = columnViewportPosition(0) - horizontalOffset();
    const int last = FileModel::ColumnCount - 1;
    const int right = columnViewportPosition(last) - horizontalOffset() + columnWidth(last);
    return QRect(left, rowViewportPosition(index.row()) - verticalOffset(), right - left,
                 rowHeight(index.row()));
}

void FileView::paintEvent(QPaintEvent *event)
{
    QTableView::paintEvent(event);

    const bool dropOnRow = m_dropRow >= 0;
    const bool rubber = m_press == Press::Rubber && !m_rubber.isNull();
    if (!dropOnRow && !rubber) {
        return;
    }

    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    QColor fill = Theme::accent();
    fill.setAlpha(36);
    painter.setBrush(fill);
    painter.setPen(QPen(Theme::accent(), 1.0));

    if (dropOnRow) {
        painter.save();
        painter.translate(horizontalOffset(), verticalOffset());
        painter.drawRoundedRect(QRectF(rowRect(m_model->index(m_dropRow, 0))).adjusted(0.5, 0.5, -0.5, -0.5),
                                4, 4);
        painter.restore();
    }
    if (rubber) {
        painter.drawRoundedRect(QRectF(m_rubber).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    }
}

// The name is what matters; the other columns give way, the type first, when
// the pane is too narrow for all of them. Decided by the view's own width so
// that a scrollbar appearing can't make a column flicker on and off.
void FileView::updateColumns()
{
    int room = width() - 2 * frameWidth() - kScrollBarWidth - kMinNameWidth;
    const auto fits = [&](int column) {
        if (room < columnWidth(column)) {
            return false;
        }
        room -= columnWidth(column);
        return true;
    };
    const bool size = fits(FileModel::SizeColumn);
    const bool modified = size && fits(FileModel::ModifiedColumn);
    const bool type = modified && fits(FileModel::TypeColumn);
    setColumnHidden(FileModel::SizeColumn, !size);
    setColumnHidden(FileModel::ModifiedColumn, !modified);
    setColumnHidden(FileModel::TypeColumn, !type);
}

void FileView::resizeEvent(QResizeEvent *event)
{
    QTableView::resizeEvent(event);
    updateColumns();
}

void FileView::mousePressEvent(QMouseEvent *event)
{
    m_press = Press::None;
    if (event->button() == Qt::LeftButton) {
        m_pressPos = event->position().toPoint();
        m_pointer = m_pressPos;
        m_rubber = QRect();
        const QModelIndex index = indexAt(m_pressPos);
        if (!index.isValid()) {
            // The view can't select from empty space by itself, and its
            // scrolling for its own drag selection would get in the way.
            m_press = Press::Rubber;
            setAutoScroll(false);
        } else if (event->modifiers() != Qt::NoModifier) {
            m_press = Press::None;
        } else if (selectionModel()->isSelected(index)) {
            m_press = Press::Drag;
        } else {
            m_press = Press::Sweep;
        }
    }
    emit activated();
    QTableView::mousePressEvent(event);
}

void FileView::mouseMoveEvent(QMouseEvent *event)
{
    const Press press = m_press;
    if (press == Press::Rubber) {
        const QPoint pos = event->position().toPoint();
        m_pointer = pos;
        m_rubber = QRect(m_pressPos, pos).normalized();
        selectRubberRows();
        viewport()->update();

        // Scroll while the band reaches past an edge of the viewport.
        const QRect area = viewport()->rect();
        const bool atEdge = pos.y() <= area.top() + kEdgeMargin || pos.y() >= area.bottom() - kEdgeMargin;
        QScrollBar *bar = verticalScrollBar();
        if (atEdge && bar->maximum() > bar->minimum()) {
            m_scrollTimer.start(kScrollInterval, this);
        } else {
            m_scrollTimer.stop();
        }
    }
    QTableView::mouseMoveEvent(event);
    if (press == Press::Sweep) {
        sweepTo(event->position().toPoint());
    }
}

void FileView::mouseReleaseEvent(QMouseEvent *event)
{
    const Press press = m_press;
    m_press = Press::None;
    m_scrollTimer.stop();
    QTableView::mouseReleaseEvent(event);

    if (press == Press::Sweep) {
        sweepTo(event->position().toPoint());
    }
    if (press == Press::Rubber) {
        setAutoScroll(true);
        // The base class ends a press with its own idea of what should be
        // selected; the band has the last word.
        if (!m_rubber.isNull()) {
            selectRubberRows();
        }
        m_rubber = QRect();
        viewport()->update();
    }
}

void FileView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const QModelIndex index = indexAt(event->position().toPoint());
        if (index.isValid()) {
            emit openRequested(index);
        }
    }
    QTableView::mouseDoubleClickEvent(event);
}

void FileView::contextMenuEvent(QContextMenuEvent *event)
{
    const QModelIndex index = indexAt(event->pos());
    if (index.isValid() && !selectionModel()->isSelected(index)) {
        // The menu acts on the row under the pointer, unless it is one of the
        // selected rows.
        selectionModel()->setCurrentIndex(index,
                                          QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    emit activated();
    emit contextMenuRequested(event->pos());
}

void FileView::keyPressEvent(QKeyEvent *event)
{
    if (event->modifiers() == Qt::NoModifier) {
        switch (event->key()) {
        case Qt::Key_Backspace:
            emit upRequested();
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            const QModelIndex current = currentIndex();
            if (current.isValid()) {
                emit openRequested(current);
            }
            return;
        }
        case Qt::Key_Insert:
        case Qt::Key_Space:
            toggleCurrent();
            return;
        case Qt::Key_Menu: {
            // The context menu for the cursor row.
            const QModelIndex current = currentIndex();
            if (current.isValid()) {
                if (!selectionModel()->isSelected(current)) {
                    selectionModel()->setCurrentIndex(
                        current, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                }
                emit contextMenuRequested(visualRect(current).center());
            }
            return;
        }
        default:
            break;
        }
    }
    QTableView::keyPressEvent(event);
}

QModelIndex FileView::moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers)
{
    // In a table, Home and End go to the first and the last column, and only
    // with Ctrl to the first and the last row. Here the cursor stays on one
    // column, so they go to the first and the last row either way.
    if (cursorAction == MoveHome || cursorAction == MoveEnd) {
        return QTableView::moveCursor(cursorAction, modifiers | Qt::ControlModifier);
    }
    return QTableView::moveCursor(cursorAction, modifiers);
}

void FileView::focusInEvent(QFocusEvent *event)
{
    QTableView::focusInEvent(event);
    emit activated();
}

void FileView::timerEvent(QTimerEvent *event)
{
    if (event->timerId() != m_scrollTimer.timerId()) {
        QTableView::timerEvent(event);
        return;
    }
    QScrollBar *bar = verticalScrollBar();
    const int before = bar->value();
    const int step = bar->singleStep() > 0 ? bar->singleStep() : 20;
    bar->setValue(before + (m_pointer.y() > m_pressPos.y() ? step : -step));
    if (bar->value() == before) {
        m_scrollTimer.stop();  // reached the end
        return;
    }
    // The rows under the fixed corners of the band changed.
    m_rubber = QRect(m_pressPos, m_pointer).normalized();
    selectRubberRows();
    viewport()->update();
}

void FileView::toggleCurrent()
{
    auto *selection = selectionModel();
    const QModelIndex current = currentIndex();
    if (!selection || !current.isValid()) {
        return;
    }
    // As in Total Commander, the cursor moves down a row after each key press,
    // so that several rows can be marked from the keyboard. Moving the cursor
    // (Ctrl-arrows do that too) leaves the marks alone.
    const bool marked = selection->isSelected(current);
    // A row that is the only marked one is just where the cursor is, since
    // moving with the arrows marks the row it lands on. It stays marked, so
    // that repeated presses mark one row after the other.
    if (!marked || selection->selectedRows().size() > 1) {
        QItemSelection row;
        row.select(current.siblingAtColumn(0), current.siblingAtColumn(FileModel::ColumnCount - 1));
        selection->select(row, marked ? QItemSelectionModel::Deselect : QItemSelectionModel::Select);
    }
    if (const QModelIndex next = current.sibling(current.row() + 1, current.column());
        next.isValid()) {
        selection->setCurrentIndex(next, QItemSelectionModel::NoUpdate);
        scrollTo(next);
    }
}

void FileView::sweepTo(const QPoint &position)
{
    auto *selection = selectionModel();
    const int anchor = rowAt(m_pressPos.y());
    if (!selection || !m_model || anchor < 0) {
        return;
    }
    // The base class selects the rows the pointer passes over, but leaves the
    // last row out once the pointer is below it or near the bottom edge.
    int row = rowAt(qBound(0, position.y(), viewport()->rect().bottom()));
    if (row < 0) {
        row = m_model->rowCount() - 1;
    }
    const int from = qMin(anchor, row);
    const int to = qMax(anchor, row);
    QItemSelection range;
    range.select(m_model->index(from, 0), m_model->index(to, FileModel::ColumnCount - 1));
    selection->select(range, QItemSelectionModel::ClearAndSelect);
    const int column = currentIndex().isValid() ? currentIndex().column() : FileModel::NameColumn;
    selection->setCurrentIndex(m_model->index(row, column), QItemSelectionModel::NoUpdate);
}

void FileView::selectRubberRows()
{
    auto *selection = selectionModel();
    if (!selection || !m_model || m_rubber.isNull()) {
        return;
    }
    const int yBottom = qMin(m_rubber.bottom(), viewport()->rect().bottom());
    const int top = rowAt(m_rubber.top());
    int bottom = rowAt(yBottom);
    // Below the last row, the band still touches that row.
    if (bottom < 0) {
        bottom = m_model->rowCount() - 1;
    }
    if (top < 0 || top > bottom) {
        // The band shrank back into empty space.
        selection->clear();
        return;
    }
    QItemSelection range;
    range.select(m_model->index(top, 0), m_model->index(bottom, FileModel::ColumnCount - 1));
    selection->select(range, QItemSelectionModel::ClearAndSelect);
    // The cursor sits at the end of the band the pointer is at.
    const int cursorRow = rowAt(qBound(0, m_pointer.y(), yBottom));
    const QModelIndex cursor = m_model->index(cursorRow >= 0 ? cursorRow : bottom, 0);
    selection->setCurrentIndex(cursor, QItemSelectionModel::NoUpdate);
}

void FileView::setDropRow(int row)
{
    if (m_dropRow == row) {
        return;
    }
    m_dropRow = row;
    viewport()->update();
}

void FileView::updateDropRow(const QPoint &position, const QMimeData &mime)
{
    // Only a folder row is highlighted; elsewhere the drop goes to this folder.
    const QModelIndex index = indexAt(position);
    const FileEntry *entry = m_model->entry(index);
    if (entry && entry->isDir && dropIsPossible(mime, entry->path)) {
        setDropRow(index.row());
    } else {
        setDropRow(-1);
    }
}

QString FileView::dropTarget(const QPoint &position) const
{
    // A folder row takes the drop; anything else means this folder.
    const FileEntry *entry = m_model->entry(indexAt(position));
    return entry && entry->isDir ? entry->path : m_model->folder();
}

void FileView::startDrag(Qt::DropActions)
{
    // Only a press on a row that was already selected drags. After any other
    // press the drag is refused, and the base class carries on by extending
    // the selection to where the pointer goes.
    if (m_press != Press::Drag) {
        return;
    }
    m_press = Press::None;
    const QModelIndexList rows = selectionModel()->selectedRows(FileModel::NameColumn);
    if (rows.isEmpty()) {
        return;
    }
    auto *mime = model()->mimeData(selectionModel()->selectedIndexes());
    if (!mime) {
        return;
    }

    // A picture of the first rows of the selection, under the pointer.
    QRect bounds;
    for (const QModelIndex &row : rows) {
        const QRect cell = visualRect(row);
        const QRect strip(0, cell.top(), viewport()->width(), cell.height());
        bounds = bounds.isNull() ? strip : bounds.united(strip);
    }
    const int maxHeight = kMaxDragImageRows * rowHeight(rows.first().row());
    bounds.setHeight(qMin(bounds.height(), maxHeight));

    const qreal ratio = devicePixelRatioF();
    QPixmap pixmap(qRound(bounds.width() * ratio), qRound(bounds.height() * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    viewport()->render(&pixmap, QPoint(), QRegion(bounds));

    emit dragStarted();
    performDrag(mime, pixmap, m_pressPos - bounds.topLeft());
}

void FileView::performDrag(QMimeData *mime, const QPixmap &image, const QPoint &hotSpot)
{
    QDrag drag(this);
    drag.setMimeData(mime);
    drag.setPixmap(image);
    drag.setHotSpot(hotSpot);
    // Copy only, whatever the target asks for: whether the items move is
    // decided where they are dropped, and nothing is ever taken away.
    drag.exec(Qt::CopyAction, Qt::CopyAction);
}

void FileView::dragEnterEvent(QDragEnterEvent *event)
{
    QTableView::dragEnterEvent(event);
    updateDrag(event);
}

void FileView::dragMoveEvent(QDragMoveEvent *event)
{
    QTableView::dragMoveEvent(event);
    updateDrag(event);
}

// After the base class: highlights the folder under the pointer, and decides
// whether the drag may land here at all.
void FileView::updateDrag(QDragMoveEvent *event)
{
    const QPoint pos = event->position().toPoint();
    const QMimeData &mime = *event->mimeData();
    updateDropRow(pos, mime);
    if (dropIsPossible(mime, dropTarget(pos))) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void FileView::dragLeaveEvent(QDragLeaveEvent *event)
{
    QTableView::dragLeaveEvent(event);
    setDropRow(-1);
}

void FileView::dropEvent(QDropEvent *event)
{
    // The base class delivers drops to the model, which doesn't take them.
    stopAutoScroll();
    setState(QAbstractItemView::NoState);
    setDropRow(-1);

    const QMimeData &mime = *event->mimeData();
    const QString target = dropTarget(event->position().toPoint());
    if (!dropIsPossible(mime, target)) {
        event->ignore();
        return;
    }

    Qt::DropAction action = Qt::CopyAction;
    bool ask = false;
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    if (modifiers & Qt::ControlModifier) {
        action = Qt::CopyAction;
    } else if (modifiers & (Qt::ShiftModifier | Qt::AltModifier)) {
        action = Qt::MoveAction;
    } else if (mime.hasFormat(QString::fromLatin1(dragSourceFormat))) {
        // From this app without a modifier: let the user choose. Files from
        // another app are always copied, never removed from where they are.
        ask = true;
    }

    // The action is set on the event for the dragging application's sake; the
    // copy or move itself is done by the pane.
    if (action & event->possibleActions()) {
        event->setDropAction(action);
    }
    event->accept();
    emit dropped(mime.urls(), target, action, ask);
}
