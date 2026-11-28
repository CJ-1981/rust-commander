#include "DuplicatesWindow.h"

#include "app/Dialogs.h"
#include "app/Theme.h"
#include "ops/FileOps.h"
#include "panels/FileModel.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSpinBox>
#include <QStringListModel>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <iterator>

namespace df = DuplicateFinder;

namespace {

constexpr int kIndent = 16;        // how far a file's name sits in from its group's title
constexpr int kBoxColumnWidth = 36;
constexpr int kFolderRows = 3;     // how many folders the list shows before it scrolls
constexpr int kMaxMinimumSize = 1024 * 1024;  // kilobytes: up to a gigabyte

// Paths differ in case only on Windows.
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

QString tr(const char *text)
{
    return QCoreApplication::translate("DuplicatesWindow", text);
}

// As sizes are written too: with the separators of the locale.
QString number(qint64 value)
{
    return QLocale::system().toString(value);
}

QString filesText(qint64 count)
{
    return count == 1 ? tr("1 file") : tr("%1 files").arg(number(count));
}

QString groupsText(qint64 count)
{
    return count == 1 ? tr("1 group") : tr("%1 groups").arg(number(count));
}

// The folder as the list shows it: absolute, without a separator at the end.
QString folderPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

// What a group's row says of it.
QString groupTitle(const df::Group &group)
{
    if (group.size == 0) {
        return tr("%1 empty files").arg(number(group.files.size()));
    }
    return tr("%1 copies of %2").arg(number(group.files.size()), humanSize(group.size));
}

QString wastedText(const df::Group &group)
{
    return tr("%1 wasted").arg(humanSize(group.wasted()));
}

// A button with nothing but a text, as elsewhere in the app.
QToolButton *button(const QString &text, QWidget *parent)
{
    auto *result = new QToolButton(parent);
    result->setText(text);
    return result;
}

// A progress that never stops the work: removing isn't something the user
// waits on for long.
class NoProgress : public FileOps::Progress {
public:
    bool tick(qint64, qint64, const QString &) override { return true; }
};

}  // namespace

// What the list shows: each group with a row of its own above its files. The
// marked files of a group are what gets removed, and one file of a group is
// never marked: a copy always stays.
class DuplicatesWindow::Model : public QAbstractTableModel {
public:
    enum Column {
        BoxColumn = 0,
        NameColumn,
        FolderColumn,
        ModifiedColumn,
        ColumnCount,
    };
    enum Role {
        GroupRowRole = Qt::UserRole + 1,
        CollapsedRole,
        LastKeptRole,  // the one copy that is left, so not to be marked
        WastedRole,
    };

    struct Entry {
        df::Group group;
        QList<bool> checked;
        int row = -1;  // of the group's own row
        bool collapsed = false;
        bool adjusted = false;  // its boxes were changed by hand since the last rule
    };

    using QAbstractTableModel::QAbstractTableModel;

    const QList<Entry> &entries() const { return m_entries; }

    void setGroups(const QList<df::Group> &groups)
    {
        beginResetModel();
        m_entries.clear();
        for (const df::Group &group : groups) {
            m_entries.append(Entry{group, QList<bool>(group.files.size(), false), -1, false});
        }
        relayout();
        endResetModel();
    }

    void clear() { setGroups({}); }

    void setCollapsed(int group, bool collapsed)
    {
        beginResetModel();
        m_entries[group].collapsed = collapsed;
        relayout();
        endResetModel();
    }

    // The group and the file a row shows; the file is -1 for a group's own row.
    QPair<int, int> place(int row) const { return m_rows.value(row, qMakePair(-1, -1)); }

    // Checks or unchecks a file; false when that would leave the group
    // without a copy.
    bool setChecked(int group, int file, bool checked)
    {
        Entry &entry = m_entries[group];
        if (entry.checked.at(file) == checked) {
            return true;
        }
        // One file of the group always stays: marking the last one that
        // still does is refused.
        if (checked && unchecked(entry) == 1) {
            return false;
        }
        entry.checked[file] = checked;
        entry.adjusted = true;
        changed(entry.row, lastRow(entry));
        return true;
    }

    void toggle(int group, int file)
    {
        setChecked(group, file, !m_entries.at(group).checked.at(file));
    }

    void mark(const df::Rule &rule, int group)
    {
        // One group by the rule (the others fresh), or all of them by the
        // rule — except the groups the user has adjusted by hand since the
        // last rule: what they chose stays, until they ask for the rule
        // again. What the boxes show is the rule's selection, and nothing
        // left over from before.
        const bool all = group < 0;
        bool any = false;
        for (int g = 0; g < m_entries.size(); ++g) {
            Entry &entry = m_entries[g];
            QList<bool> checked(entry.group.files.size(), false);
            if (g == group) {
                for (int i : df::toRemove(entry.group, rule)) {
                    checked[i] = true;
                }
            } else if (all) {
                // What the user chose by hand stays; the rest goes by the rule.
                checked = entry.adjusted ? entry.checked : [&] {
                    QList<bool> byRule(entry.group.files.size(), false);
                    for (int i : df::toRemove(entry.group, rule)) {
                        byRule[i] = true;
                    }
                    return byRule;
                }();
            }  // a mark of one group leaves the others fresh
            if (checked != entry.checked) {
                entry.checked = checked;
                any = true;
            }
        }
        // The rules own the selection now.
        for (Entry &entry : m_entries) {
            entry.adjusted = false;
        }
        if (any) {
            changed(0, int(m_rows.size()) - 1);
        }
    }

    // After files were removed: those that are gone come off the list, and a
    // group with one file left is no group any more.
    void forget(const QStringList &gone)
    {
        const QSet<QString> paths(gone.cbegin(), gone.cend());
        beginResetModel();
        for (int g = int(m_entries.size()) - 1; g >= 0; --g) {
            Entry &entry = m_entries[g];
            QList<df::File> files;
            QList<bool> checked;
            for (int f = 0; f < entry.group.files.size(); ++f) {
                if (!paths.contains(entry.group.files.at(f).path)) {
                    files.append(entry.group.files.at(f));
                    checked.append(entry.checked.at(f));
                }
            }
            if (files.size() < 2) {
                m_entries.removeAt(g);
            } else {
                entry.group.files = files;
                entry.checked = checked;
            }
        }
        relayout();
        endResetModel();
    }

    // Unchecks the files at `paths`, without taking them off the list.
    void uncheck(const QSet<QString> &paths)
    {
        bool any = false;
        for (Entry &entry : m_entries) {
            for (int f = 0; f < entry.group.files.size(); ++f) {
                if (entry.checked.at(f) && paths.contains(entry.group.files.at(f).path)) {
                    entry.checked[f] = false;
                    any = true;
                }
            }
        }
        if (any) {
            changed(0, int(m_rows.size()) - 1);
        }
    }

    int checkedCount() const
    {
        int result = 0;
        for (const Entry &entry : m_entries) {
            result += int(std::count(entry.checked.cbegin(), entry.checked.cend(), true));
        }
        return result;
    }

    qint64 checkedBytes() const
    {
        qint64 result = 0;
        for (const Entry &entry : m_entries) {
            result += entry.group.size * std::count(entry.checked.cbegin(), entry.checked.cend(), true);
        }
        return result;
    }

    qint64 wasted() const
    {
        qint64 result = 0;
        for (const Entry &entry : m_entries) {
            result += entry.group.wasted();
        }
        return result;
    }

    QStringList checkedPaths() const
    {
        QStringList result;
        for (const Entry &entry : m_entries) {
            for (int f = 0; f < entry.group.files.size(); ++f) {
                if (entry.checked.at(f)) {
                    result.append(entry.group.files.at(f).path);
                }
            }
        }
        return result;
    }

    QList<df::Group> groups() const
    {
        QList<df::Group> result;
        for (const Entry &entry : m_entries) {
            result.append(entry.group);
        }
        return result;
    }

    QList<Target> targets() const
    {
        QList<Target> result;
        for (const Entry &entry : m_entries) {
            Target target;
            target.size = entry.group.size;
            for (int f = 0; f < entry.group.files.size(); ++f) {
                (entry.checked.at(f) ? target.marked : target.others).append(entry.group.files.at(f));
            }
            if (!target.marked.isEmpty()) {
                result.append(target);
            }
        }
        return result;
    }

    int rowCount(const QModelIndex &parent) const override
    {
        return parent.isValid() ? 0 : int(m_rows.size());
    }

    int columnCount(const QModelIndex &parent) const override
    {
        return parent.isValid() ? 0 : ColumnCount;
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size()) {
            return Qt::NoItemFlags;
        }
        // A group's row can't be selected or checked: it is gone into, not chosen.
        if (m_rows.at(index.row()).second < 0) {
            return Qt::ItemIsEnabled;
        }
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size()) {
            return {};
        }
        const QPair<int, int> place = m_rows.at(index.row());
        const Entry &entry = m_entries.at(place.first);
        if (place.second < 0) {
            switch (role) {
            case Qt::DisplayRole:
                return index.column() == NameColumn ? QVariant(groupTitle(entry.group)) : QVariant();
            case Qt::ToolTipRole:
                return groupTitle(entry.group) + QLatin1String("\n") + tr("Click to show or hide the files.");
            case Qt::FontRole: {
                QFont font = QApplication::font();
                font.setBold(true);
                return font;
            }
            case GroupRowRole:
                return true;
            case CollapsedRole:
                return entry.collapsed;
            case WastedRole:
                return wastedText(entry.group);
            default:
                return {};
            }
        }

        const df::File &file = entry.group.files.at(place.second);
        const bool checked = entry.checked.at(place.second);
        switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
            case NameColumn:
                return QFileInfo(file.path).fileName();
            case FolderColumn:
                return QDir::toNativeSeparators(QFileInfo(file.path).path());
            case ModifiedColumn:
                // The year in full, as in the file lists.
                return QLocale::system().toString(file.modified, QStringLiteral("yyyy-MM-dd HH:mm"));
            default:
                return {};
            }
        case Qt::ToolTipRole:
            return QDir::toNativeSeparators(file.path);
        case Qt::CheckStateRole:
            if (index.column() == BoxColumn) {
                return checked ? Qt::Checked : Qt::Unchecked;
            }
            return {};
        case GroupRowRole:
            return false;
        case LastKeptRole:
            // All the others are marked: this one can't be.
            return !checked && unchecked(entry) == 1;
        default:
            return {};
        }
    }

    bool setData(const QModelIndex &index, const QVariant &value, int role) override
    {
        if (role != Qt::CheckStateRole || index.column() != BoxColumn || !index.isValid()
            || index.row() >= m_rows.size()) {
            return false;
        }
        const QPair<int, int> place = m_rows.at(index.row());
        return place.second >= 0
               && setChecked(place.first, place.second, value.value<Qt::CheckState>() == Qt::Checked);
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || section >= ColumnCount) {
            return {};
        }
        if (role == Qt::ToolTipRole && section == BoxColumn) {
            return tr("Marked files are removed by the buttons at the bottom.");
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        switch (section) {
        case NameColumn:
            return tr("Name");
        case FolderColumn:
            return tr("Folder");
        case ModifiedColumn:
            return tr("Modified");
        default:
            return {};
        }
    }

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override
    {
        return hasIndex(row, column, parent) ? createIndex(row, column) : QModelIndex();
    }

private:
    static int unchecked(const Entry &entry)
    {
        return int(entry.checked.size()) - int(std::count(entry.checked.cbegin(), entry.checked.cend(), true));
    }

    int lastRow(const Entry &entry) const
    {
        return entry.row + (entry.collapsed ? 0 : int(entry.group.files.size()));
    }

    void changed(int firstRow, int lastRow)
    {
        if (firstRow >= 0 && lastRow >= firstRow) {
            emit dataChanged(index(firstRow, 0), index(lastRow, ColumnCount - 1),
                             {Qt::CheckStateRole, LastKeptRole});
        }
    }

    // Makes a row for every group and, unless it is collapsed, for its files.
    void relayout()
    {
        m_rows.clear();
        for (int g = 0; g < m_entries.size(); ++g) {
            Entry &entry = m_entries[g];
            entry.row = int(m_rows.size());
            m_rows.append(qMakePair(g, -1));
            if (!entry.collapsed) {
                for (int f = 0; f < entry.group.files.size(); ++f) {
                    m_rows.append(qMakePair(g, f));
                }
            }
        }
    }

    QList<Entry> m_entries;
    QVector<QPair<int, int>> m_rows;  // group and file of each row
};

// Draws a group's row, the boxes, and the files' names set in a little.
class DuplicatesWindow::RowDelegate : public QStyledItemDelegate {
public:
    explicit RowDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), option.fontMetrics.height() + 10));
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget *widget = opt.widget;
        const QStyle *style = widget ? widget->style() : QApplication::style();

        if (index.data(Model::GroupRowRole).toBool()) {
            // From the Name column on, the row is one cell.
            painter->fillRect(option.rect, option.palette.window());
            painter->setPen(option.palette.color(QPalette::Mid));
            painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
            if (index.column() == Model::BoxColumn) {
                QStyleOptionViewItem arrow = opt;
                arrow.rect = QRect(option.rect.center().x() - 5, option.rect.center().y() - 5, 10, 10);
                style->drawPrimitive(index.data(Model::CollapsedRole).toBool()
                                             ? QStyle::PE_IndicatorArrowRight
                                             : QStyle::PE_IndicatorArrowDown,
                                     &arrow, painter, widget);
                return;
            }
            if (index.column() != Model::NameColumn) {
                return;
            }
            const QRect text = option.rect.adjusted(4, 0, -6, 0);
            const QString gain = index.data(Model::WastedRole).toString();
            const int gainWidth = opt.fontMetrics.horizontalAdvance(gain);
            painter->setPen(option.palette.color(QPalette::Text));
            painter->setFont(opt.font);
            painter->drawText(text, Qt::AlignVCenter | Qt::AlignLeft,
                              opt.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                          Qt::ElideRight,
                                                          qMax(0, text.width() - gainWidth - 16)));
            painter->setPen(Theme::muted());
            painter->drawText(text, Qt::AlignVCenter | Qt::AlignRight, gain);
            return;
        }

        if (index.column() == Model::BoxColumn) {
            // The box sits in the middle of the cell, all of which is where it is clicked.
            style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);
            QRect box = style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, widget);
            box.moveCenter(option.rect.center());
            opt.rect = box;
            opt.state &= ~QStyle::State_HasFocus;
            opt.state |= index.data(Qt::CheckStateRole).value<Qt::CheckState>() == Qt::Checked
                                 ? QStyle::State_On
                                 : QStyle::State_Off;
            if (index.data(Model::LastKeptRole).toBool()) {
                // Shown as out of use, as clicking it does nothing.
                opt.state &= ~QStyle::State_Enabled;
            }
            style->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &opt, painter, widget);
            return;
        }

        QStyleOptionViewItem text = option;
        if (index.column() == Model::NameColumn) {
            text.rect.adjust(kIndent, 0, 0, 0);
        }
        initStyleOption(&text, index);
        text.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        // Of a folder the end matters as much as the start.
        text.textElideMode = index.column() == Model::FolderColumn ? Qt::ElideMiddle : Qt::ElideRight;
        style->drawControl(QStyle::CE_ItemViewItem, &text, painter, widget);
    }

    // The whole cell of a box is clicked to check the file and to uncheck it.
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (event->type() != QEvent::MouseButtonRelease || index.column() != Model::BoxColumn
            || index.data(Model::GroupRowRole).toBool()) {
            return false;
        }
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (!option.rect.contains(mouse->position().toPoint())) {
            return false;
        }
        const bool checked = index.data(Qt::CheckStateRole).value<Qt::CheckState>() == Qt::Checked;
        return model->setData(index, checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
    }
};

// The list itself: a group's row is clicked to show or hide its files, Space
// checks the file the cursor is on, and it says so when there is nothing to show.
class DuplicatesWindow::List : public QTableView {
public:
    List(Model *model, QWidget *parent)
        : QTableView(parent)
        , m_model(model)
    {
        setModel(model);
        setItemDelegate(new RowDelegate(this));
        setSelectionBehavior(QAbstractItemView::SelectRows);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setTabKeyNavigation(false);
        setShowGrid(false);
        setWordWrap(false);
        setSortingEnabled(false);

        QHeaderView *header = horizontalHeader();
        header->setSectionsClickable(false);
        header->setHighlightSections(false);
        header->setStretchLastSection(false);
        header->setSectionResizeMode(Model::BoxColumn, QHeaderView::Fixed);
        header->setSectionResizeMode(Model::FolderColumn, QHeaderView::Stretch);
        header->setSectionResizeMode(Model::ModifiedColumn, QHeaderView::ResizeToContents);
        setColumnWidth(Model::BoxColumn, kBoxColumnWidth);
        setColumnWidth(Model::NameColumn, 230);
        verticalHeader()->setSectionsClickable(false);
        verticalHeader()->setHighlightSections(false);
    }

    void setPlaceholder(const QString &text)
    {
        if (m_placeholder != text) {
            m_placeholder = text;
            viewport()->update();
        }
    }

    // The group of the row the cursor is on, or -1.
    int currentGroup() const { return m_model->place(currentIndex().row()).first; }

    void toggleGroup(int group)
    {
        m_model->setCollapsed(group, !m_model->entries().at(group).collapsed);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QTableView::paintEvent(event);
        if (m_model->rowCount(QModelIndex()) > 0 || m_placeholder.isEmpty()) {
            return;
        }
        QPainter painter(viewport());
        painter.setPen(Theme::muted());
        painter.drawText(viewport()->rect().adjusted(24, 24, -24, -24),
                         Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap, m_placeholder);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const QModelIndex index = indexAt(event->position().toPoint());
        m_pressedGroup = isGroupRow(index) ? index.row() : -1;
        QTableView::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        // The release after a double-click would show the files again that
        // the click before it hid.
        if (m_afterDoubleClick) {
            m_afterDoubleClick = false;
            return;
        }
        const QModelIndex index = indexAt(event->position().toPoint());
        const int pressed = m_pressedGroup;
        m_pressedGroup = -1;
        if (pressed >= 0 && index.row() == pressed) {
            toggleGroup(m_model->place(pressed).first);
            return;
        }
        QTableView::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (isGroupRow(indexAt(event->position().toPoint()))) {
            m_afterDoubleClick = true;
            return;
        }
        QTableView::mouseDoubleClickEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        const QPair<int, int> place = m_model->place(currentIndex().row());
        if (place.first >= 0) {
            if (place.second < 0) {
                // As in a tree.
                const bool collapsed = m_model->entries().at(place.first).collapsed;
                if ((event->key() == Qt::Key_Space || event->key() == Qt::Key_Select
                     || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
                    || (event->key() == Qt::Key_Right && collapsed)
                    || (event->key() == Qt::Key_Left && !collapsed)) {
                    toggleGroup(place.first);
                    event->accept();
                    return;
                }
            } else if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Select) {
                // Handled here, so that it works from every column of the row.
                m_model->toggle(place.first, place.second);
                event->accept();
                return;
            }
        }
        QTableView::keyPressEvent(event);
    }

private:
    static bool isGroupRow(const QModelIndex &index)
    {
        return index.isValid() && index.data(Model::GroupRowRole).toBool();
    }

    Model *m_model;
    QString m_placeholder;
    int m_pressedGroup = -1;
    bool m_afterDoubleClick = false;
};

// A file is only removed while it is still what was scanned, and while
// another copy of it is still there as it was scanned.
DuplicatesWindow::Removal DuplicatesWindow::removeMarked(const QList<Target> &targets, bool toTrash)
{
    Removal result;
    for (const Target &target : targets) {
        const auto stillThere = [&](const df::File &file) { return df::unchanged(file, target.size); };
        if (std::none_of(target.others.cbegin(), target.others.cend(), stillThere)) {
            // Every other copy is gone or changed: these are all there is of it now.
            ++result.groupsLeft;
            continue;
        }
        for (const df::File &file : target.marked) {
            const QFileInfo info(file.path);
            if (!info.isFile() || info.isSymLink()) {
                // Not there any more, or something else is under that name now.
                result.gone.append(file.path);
            } else if (!stillThere(file)) {
                result.changed.append(file.path);
            } else {
                QString error;
                NoProgress progress;
                const bool removed = toTrash ? FileOps::trash({file.path}, &error)
                                             : FileOps::remove({file.path}, progress, &error);
                if (removed) {
                    result.removed.append(file.path);
                } else {
                    ++result.failed;
                    if (result.error.isEmpty()) {
                        result.error = error;
                    }
                }
            }
        }
    }
    return result;
}

DuplicatesWindow *DuplicatesWindow::open(const QString &startFolder, Dialogs &dialogs, QWidget *parent)
{
    auto *window = new DuplicatesWindow(dialogs, parent);
    window->addFolder(startFolder);
    window->show();
    return window;
}

DuplicatesWindow::DuplicatesWindow(Dialogs &dialogs, QWidget *parent)
    : QDialog(parent)
    , m_dialogs(dialogs)
{
    setAttribute(Qt::WA_DeleteOnClose);
    // Resized by its edges, as any window; the grip would sit on the buttons.
    setSizeGripEnabled(false);
    setWindowTitle(tr("Duplicates"));
    resize(980, 620);
    setMinimumSize(760, 480);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);

    // The folders to scan.
    m_folderList = new QStringListModel(this);
    m_folders = new QTableView;
    m_folders->setObjectName("scanFolders");
    m_folders->setModel(m_folderList);
    m_folders->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_folders->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_folders->setSelectionMode(QAbstractItemView::SingleSelection);
    m_folders->setTabKeyNavigation(false);
    m_folders->setShowGrid(false);
    m_folders->horizontalHeader()->hide();
    m_folders->verticalHeader()->hide();
    m_folders->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    const int rowHeight = fontMetrics().height() + 10;
    m_folders->verticalHeader()->setDefaultSectionSize(rowHeight);
    m_folders->setFixedHeight(kFolderRows * rowHeight + 2);

    m_addFolder = button(tr("Add\u2026"), this);
    m_removeFolder = button(tr("Remove"), this);
    connect(m_addFolder, &QAbstractButton::clicked, this, &DuplicatesWindow::pickFolder);
    connect(m_removeFolder, &QAbstractButton::clicked, this, &DuplicatesWindow::removeSelectedFolder);
    auto *folderButtons = new QVBoxLayout;
    folderButtons->setSpacing(4);
    folderButtons->addWidget(m_addFolder);
    folderButtons->addWidget(m_removeFolder);
    folderButtons->addStretch();

    auto *folders = new QHBoxLayout;
    folders->setSpacing(6);
    folders->addWidget(m_folders, 1);
    folders->addLayout(folderButtons);
    layout->addWidget(new QLabel(tr("Folders to scan")));
    layout->addLayout(folders);

    // What is looked for.
    m_pattern = new QLineEdit(QStringLiteral("*"));
    m_pattern->setToolTip(tr("Wild cards such as *.jpg, several of them separated by spaces."));
    m_subfolders = new QCheckBox(tr("Look in subfolders"));
    m_subfolders->setChecked(true);
    m_minSize = new QSpinBox;
    m_minSize->setRange(0, kMaxMinimumSize);
    m_minSize->setSuffix(QStringLiteral(" KB"));
    m_minSize->setValue(int(df::Options{}.minSize / 1024));
    m_minSize->setToolTip(tr("Files smaller than this are left out. Empty files are decided by their own box."));
    m_empty = new QCheckBox(tr("Include empty files"));
    connect(m_pattern, &QLineEdit::returnPressed, this, &DuplicatesWindow::startScan);

    auto *options = new QHBoxLayout;
    options->addWidget(new QLabel(tr("Include files")));
    options->addWidget(m_pattern);
    options->addWidget(new QLabel(tr("Minimum size")));
    options->addWidget(m_minSize);
    options->addWidget(m_subfolders);
    options->addWidget(m_empty);
    options->addStretch();
    layout->addLayout(options);

    // Starting a scan, and how it goes.
    m_scanButton = button(tr("Scan"), this);
    m_stopButton = button(tr("Stop"), this);
    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);
    m_progress->setFixedWidth(200);
    m_status = new QLabel;
    m_status->setWordWrap(true);
    connect(m_scanButton, &QAbstractButton::clicked, this, &DuplicatesWindow::startScan);
    connect(m_stopButton, &QAbstractButton::clicked, this, &DuplicatesWindow::stopScan);

    auto *scan = new QHBoxLayout;
    scan->addWidget(m_scanButton);
    scan->addWidget(m_stopButton);
    scan->addWidget(m_progress);
    scan->addWidget(m_status, 1);
    layout->addLayout(scan);

    // The groups.
    m_model = new Model(this);
    m_list = new List(m_model, this);
    m_list->setObjectName("duplicates");
    layout->addWidget(m_list, 1);
    connect(m_model, &QAbstractItemModel::dataChanged, this, [this] {
        updateTotals();
        updateControls();
    });
    // Where the user was is noted before the list is rebuilt, as the list forgets it.
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, &DuplicatesWindow::rememberPlace);
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] {
        rowsChanged();
        updateTotals();
        updateControls();
    });
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { updateControls(); });

    // Choosing by a rule.
    m_rule = new QComboBox;
    m_rule->addItem(tr("Keeping the first copy"));
    m_rule->addItem(tr("Keeping the newest copy"));
    m_rule->addItem(tr("Keeping the oldest copy"));
    m_rule->addItem(tr("Keeping the copies in a folder"));
    m_rule->addItem(tr("Removing the copies in a folder"));
    m_ruleFolderLabel = new QLabel(tr("Folder"));
    m_ruleFolder = new QLineEdit;
    m_ruleBrowse = button(tr("\u2026"), this);
    m_ruleBrowse->setToolTip(tr("Choose the folder"));
    m_markGroup = button(tr("In this group"), this);
    m_markAll = button(tr("In all groups"), this);
    connect(m_rule, &QComboBox::currentIndexChanged, this, &DuplicatesWindow::ruleChanged);
    connect(m_ruleBrowse, &QAbstractButton::clicked, this, &DuplicatesWindow::pickRuleFolder);
    connect(m_markGroup, &QAbstractButton::clicked, this, [this] { mark(rule(), currentGroup()); });
    connect(m_markAll, &QAbstractButton::clicked, this, [this] { mark(rule(), -1); });

    auto *mark = new QHBoxLayout;
    mark->addWidget(new QLabel(tr("Mark for removal")));
    mark->addWidget(m_rule);
    mark->addWidget(m_ruleFolderLabel);
    mark->addWidget(m_ruleFolder, 1);
    mark->addWidget(m_ruleBrowse);
    mark->addWidget(m_markGroup);
    mark->addWidget(m_markAll);
    layout->addLayout(mark);
    ruleChanged();

    // What comes of it.
    m_total = new QLabel;
    m_trash = button(tr("Move to Trash"), this);
    m_delete = button(tr("Delete for good"), this);
    connect(m_trash, &QAbstractButton::clicked, this, [this] { removeChecked(true); });
    connect(m_delete, &QAbstractButton::clicked, this, [this] { removeChecked(false); });
    auto *remove = new QHBoxLayout;
    remove->addWidget(m_total, 1);
    remove->addWidget(m_trash);
    remove->addWidget(m_delete);
    layout->addLayout(remove);

    // The folder that is selected goes with the Delete key, as in the file lists.
    auto *erase = new QShortcut(QKeySequence::Delete, m_folders);
    erase->setContext(Qt::WidgetWithChildrenShortcut);
    connect(erase, &QShortcut::activated, this, &DuplicatesWindow::removeSelectedFolder);

    m_scanButton->setFocus();
    updateStatus();
    updateStatus();
    updateTotals();
    updateControls();
}

DuplicatesWindow::~DuplicatesWindow()
{
    // A scan stops at its next look, and the window is not destroyed from under
    // the thread that reports for it: it is waited for first.
    if (m_stopFlag) {
        *m_stopFlag = true;
    }
    if (m_scanWatcher) {
        m_scanWatcher->waitForFinished();
    }
    if (m_removeWatcher) {
        m_removeWatcher->waitForFinished();
    }
}

df::Options DuplicatesWindow::options() const
{
    df::Options result;
    result.folders = m_folderList->stringList();
    result.pattern = m_pattern->text().trimmed();
    result.subfolders = m_subfolders->isChecked();
    result.minSize = qint64(m_minSize->value()) * 1024;
    result.includeEmpty = m_empty->isChecked();
    return result;
}

void DuplicatesWindow::setOptions(const df::Options &options)
{
    QStringList folders;
    for (const QString &folder : options.folders) {
        folders.append(folderPath(folder));
    }
    folders.removeDuplicates();
    m_folderList->setStringList(folders);
    m_pattern->setText(options.pattern.trimmed().isEmpty() ? QStringLiteral("*") : options.pattern);
    m_subfolders->setChecked(options.subfolders);
    m_minSize->setValue(int(qBound<qint64>(0, (options.minSize + 1023) / 1024, qint64(kMaxMinimumSize))));
    m_empty->setChecked(options.includeEmpty);
    m_scanned = false;
    updateStatus();
    updateControls();
}

void DuplicatesWindow::addFolder(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        return;
    }
    const QString folder = folderPath(info.isDir() ? path : info.absolutePath());
    if (!QFileInfo(folder).isDir()) {
        return;
    }
    QStringList folders = m_folderList->stringList();
    if (folders.contains(folder, kPathCase)) {
        return;
    }
    folders.append(folder);
    m_folderList->setStringList(folders);
    m_folders->selectRow(int(folders.size()) - 1);
    m_scanned = false;
    updateStatus();
    updateControls();
}

void DuplicatesWindow::removeFolder(const QString &path)
{
    const QString folder = folderPath(path);
    QStringList folders = m_folderList->stringList();
    for (int i = int(folders.size()) - 1; i >= 0; --i) {
        if (folders.at(i).compare(folder, kPathCase) == 0) {
            folders.removeAt(i);
        }
    }
    m_folderList->setStringList(folders);
    m_scanned = false;
    updateStatus();
    updateControls();
}

void DuplicatesWindow::removeSelectedFolder()
{
    if (isBusy()) {
        return;
    }
    // From the bottom, so that the rows above keep their numbers.
    const QModelIndexList rows = m_folders->selectionModel()->selectedRows();
    for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
        m_folderList->removeRow(it->row());
    }
    updateControls();
}

QTableView *DuplicatesWindow::list() const
{
    return m_list;
}

bool DuplicatesWindow::isScanning() const
{
    return m_scanning;
}

bool DuplicatesWindow::isRemoving() const
{
    return m_removing;
}

bool DuplicatesWindow::isBusy() const
{
    return m_scanning || m_removing;
}

QString DuplicatesWindow::statusText() const
{
    return m_status->text();
}

QList<df::Group> DuplicatesWindow::groups() const
{
    return m_model->groups();
}

QStringList DuplicatesWindow::checkedPaths() const
{
    return m_model->checkedPaths();
}

QString DuplicatesWindow::totalText() const
{
    return m_total->text();
}

int DuplicatesWindow::currentGroup() const
{
    return m_list->currentGroup();
}

df::Rule DuplicatesWindow::rule() const
{
    static constexpr df::Rule::Kind kinds[] = {df::Rule::Kind::First,        df::Rule::Kind::Newest,
                                               df::Rule::Kind::Oldest,     df::Rule::Kind::InFolder,
                                               df::Rule::Kind::NotInFolder};
    df::Rule result;
    const int index = m_rule->currentIndex();
    if (index >= 0 && index < int(std::size(kinds))) {
        result.kind = kinds[index];
    }
    if (result.kind == df::Rule::Kind::InFolder || result.kind == df::Rule::Kind::NotInFolder) {
        result.folder = m_ruleFolder->text().trimmed();
    }
    return result;
}

bool DuplicatesWindow::setChecked(int group, int file, bool checked)
{
    return group >= 0 && group < m_model->entries().size()
           && m_model->setChecked(group, file, checked);
}

void DuplicatesWindow::mark(const df::Rule &what, int group)
{
    if (isBusy() || m_model->entries().isEmpty()) {
        return;
    }
    m_notice.clear();
    m_model->mark(what, group);
    updateStatus();
    updateTotals();
    updateControls();
}

void DuplicatesWindow::pickFolder()
{
    const QString folder =
            QFileDialog::getExistingDirectory(this, tr("Add a folder to scan"), m_folderList->stringList().value(0));
    addFolder(folder);
}

void DuplicatesWindow::pickRuleFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Choose a folder"), m_ruleFolder->text());
    if (!folder.isEmpty()) {
        m_ruleFolder->setText(folderPath(folder));
    }
}

void DuplicatesWindow::ruleChanged()
{
    const bool byFolder = m_rule->currentIndex() >= 3;
    m_ruleFolderLabel->setVisible(byFolder);
    m_ruleFolder->setVisible(byFolder);
    m_ruleBrowse->setVisible(byFolder);
    if (byFolder && m_ruleFolder->text().isEmpty()) {
        // "This folder": the one of the file the cursor is on, or else the first to be scanned.
        const QPair<int, int> place = m_list->currentIndex().row() >= 0
                                              ? m_model->place(m_list->currentIndex().row())
                                              : qMakePair(-1, -1);
        m_ruleFolder->setText(place.second >= 0
                                      ? QFileInfo(m_model->entries().at(place.first).group.files.at(place.second).path).path()
                                      : m_folderList->stringList().value(0));
    }
}

void DuplicatesWindow::startScan()
{
    if (isBusy()) {
        return;
    }
    const df::Options scanOptions = options();
    m_foldersGiven = 0;
    for (const QString &folder : scanOptions.folders) {
        m_foldersGiven += QFileInfo(folder).isDir();
    }
    m_notice.clear();
    m_stopped = false;
    m_scanned = false;
    m_scanning = true;
    m_model->clear();

    m_stopFlag = std::make_shared<std::atomic<bool>>(false);
    auto *watcher = new QFutureWatcher<df::Result>(this);
    m_scanWatcher = watcher;
    connect(watcher, &QFutureWatcher<df::Result>::finished, this, [this, watcher] {
        const df::Result result = watcher->result();
        watcher->deleteLater();
        m_scanWatcher = nullptr;
        scanDone(result);
    });
    // The worker thread reports to the GUI thread, and only while this window
    // is alive: the destructor waits for the scan to end.
    const df::Report report = [this](const df::Progress &progress) {
        QMetaObject::invokeMethod(this, [this, progress] { showProgress(progress); }, Qt::QueuedConnection);
    };
    watcher->setFuture(QtConcurrent::run([scanOptions, stop = m_stopFlag, report]() -> df::Result {
        return df::find(scanOptions, *stop, report);
    }));

    m_progress->setRange(0, 0);
    m_status->setText(tr("Looking for files\u2026"));
    updateStatus();
    updateTotals();
    updateControls();
}

void DuplicatesWindow::stopScan()
{
    if (m_scanning) {
        *m_stopFlag = true;
    }
}

void DuplicatesWindow::scanDone(const df::Result &result)
{
    m_scanning = false;
    m_scanned = true;
    m_stopped = result.stopped;
    m_filesChecked = result.files;
    m_unreadable = result.unreadable;
    m_model->setGroups(result.groups);
    if (!m_model->entries().isEmpty()) {
        m_list->selectionModel()->setCurrentIndex(m_model->index(0, Model::NameColumn),
                                                  QItemSelectionModel::NoUpdate);
    }
    m_progress->setRange(0, 1000);
    m_progress->setValue(1000);
    updateStatus();
    updateTotals();
    updateControls();
    emit scanFinished();
}

void DuplicatesWindow::showProgress(const df::Progress &progress)
{
    if (!m_scanning) {
        // A report that was on its way when the scan ended.
        return;
    }
    switch (progress.stage) {
    case df::Progress::Stage::Walking:
        m_progress->setRange(0, 0);
        m_status->setText(tr("Looking for files\u2026 %1 so far").arg(filesText(progress.files)));
        break;
    case df::Progress::Stage::Comparing:
        m_progress->setRange(0, 1000);
        m_progress->setValue(progress.bytesTotal > 0
                                     ? int(progress.bytesRead * 1000 / progress.bytesTotal)
                                     : 1000);
        m_status->setText(tr("Comparing contents\u2026 %1 checked").arg(filesText(progress.files)));
        break;
    }
}

void DuplicatesWindow::updateStatus()
{
    if (m_scanning || m_removing) {
        // Said by showProgress(), and by removeChecked().
        updatePlaceholder();
        return;
    }

    QString text;
    if (!m_scanned) {
        text = m_folderList->stringList().isEmpty() ? tr("Add a folder to scan, then click Scan.")
                                                    : tr("Click Scan to look for duplicates.");
    } else if (m_filesChecked == 0 && m_unreadable > 0 && m_foldersGiven == 0) {
        text = tr("None of the folders could be read.");
    } else {
        const QString checked = tr("%1 checked").arg(filesText(m_filesChecked));
        const int count = int(m_model->entries().size());
        text = count == 0 ? tr("%1: no duplicates found.").arg(checked)
                          : tr("%1, %2, %3 can be freed")
                                .arg(checked, groupsText(count), humanSize(m_model->wasted()));
        if (m_stopped) {
            text.prepend(tr("Stopped early: "));
        }
        if (m_unreadable == 1) {
            text += QLatin1Char(' ') + tr("(1 folder could not be read)");
        } else if (m_unreadable > 1) {
            text += QLatin1Char(' ') + tr("(%1 folders could not be read)").arg(number(m_unreadable));
        }
    }
    if (!m_notice.isEmpty()) {
        text.prepend(m_notice + QLatin1Char(' '));
    }
    m_status->setText(text);
    updatePlaceholder();
}

void DuplicatesWindow::updatePlaceholder()
{
    QString text;
    if (m_scanning) {
        text = tr("Scanning\u2026");
    } else if (!m_scanned) {
        text = tr("The duplicates show up here.");
    } else if (m_model->entries().isEmpty()) {
        text = m_stopped ? tr("Nothing was found before the scan was stopped.") : tr("No duplicates found.");
    }
    m_list->setPlaceholder(text);
}

void DuplicatesWindow::updateTotals()
{
    const int count = m_model->checkedCount();
    m_total->setText(count == 0 ? tr("No files are marked.")
                                : tr("%1 marked, %2").arg(filesText(count), humanSize(m_model->checkedBytes())));
}

void DuplicatesWindow::updateControls()
{
    const bool busy = isBusy();
    const bool marked = m_model->checkedCount() > 0;
    const QList<QWidget *> settings = {m_folders,    m_addFolder, m_removeFolder,    m_pattern,
                                       m_subfolders, m_minSize,   m_empty,           m_rule,
                                       m_ruleBrowse, m_ruleFolder, m_ruleFolderLabel};
    for (QWidget *widget : settings) {
        widget->setEnabled(!busy);
    }
    if (!busy) {
        m_removeFolder->setEnabled(!m_folders->selectionModel()->selectedRows().isEmpty());
    }
    m_scanButton->setEnabled(!busy && !m_folderList->stringList().isEmpty());
    m_stopButton->setEnabled(m_scanning);
    m_list->setEnabled(!busy);
    m_markGroup->setEnabled(!busy && currentGroup() >= 0);
    m_markAll->setEnabled(!busy && !m_model->entries().isEmpty());
    m_trash->setEnabled(!busy && marked);
    m_delete->setEnabled(!busy && marked);
}

void DuplicatesWindow::rememberPlace()
{
    m_placeScroll = m_list->verticalScrollBar()->value();
    const QPair<int, int> place = m_list->currentIndex().row() >= 0
                                          ? m_model->place(m_list->currentIndex().row())
                                          : qMakePair(-1, -1);
    m_placeGroup = place.first;
    m_placeFile = place.second;
}

void DuplicatesWindow::rowsChanged()
{
    m_list->clearSpans();
    for (const Model::Entry &entry : m_model->entries()) {
        m_list->setSpan(entry.row, Model::NameColumn, 1, Model::ColumnCount - 1);
    }

    // Back where the user was: the same file, or its group's row, or nothing.
    int row = -1;
    const QList<Model::Entry> &entries = m_model->entries();
    if (m_placeGroup >= 0 && m_placeGroup < entries.size()) {
        const Model::Entry &entry = entries.at(m_placeGroup);
        row = entry.row;
        if (!entry.collapsed && m_placeFile >= 0 && m_placeFile < entry.group.files.size()) {
            row += 1 + m_placeFile;
        }
    }
    if (row >= 0 && m_list->currentIndex().row() < 0) {
        m_list->selectionModel()->setCurrentIndex(m_model->index(row, Model::NameColumn),
                                                  QItemSelectionModel::NoUpdate);
    }
    m_list->verticalScrollBar()->setValue(m_placeScroll);
}

void DuplicatesWindow::removeChecked(bool toTrash)
{
    if (isBusy()) {
        return;
    }
    const QList<Target> targets = m_model->targets();
    if (targets.isEmpty()) {
        return;
    }
    qint64 count = 0;
    qint64 bytes = 0;
    for (const Target &target : targets) {
        count += target.marked.size();
        bytes += target.marked.size() * target.size;
    }
    const QString what = count == 1 ? tr("this file (%1)").arg(humanSize(bytes))
                                    : tr("these %1 (%2)").arg(filesText(count), humanSize(bytes));
    const QString title = toTrash ? tr("To the Trash") : tr("Delete");
    const QString question = toTrash
                ? tr("Move %1 to the Trash? One copy of each group stays.").arg(what)
                : tr("Delete %1 for good? This can't be undone. One copy of each group stays.").arg(what);
    if (m_dialogs.ask(title, question) != Dialogs::Answer::Yes) {
        return;
    }

    m_removing = true;
    m_trashing = toTrash;
    m_status->setText(toTrash ? tr("Moving %1 to the Trash\u2026").arg(what)
                              : tr("Deleting %1\u2026").arg(what));
    updatePlaceholder();
    updateControls();

    auto *watcher = new QFutureWatcher<Removal>(this);
    m_removeWatcher = watcher;
    connect(watcher, &QFutureWatcher<Removal>::finished, this, [this, watcher] {
        const Removal removal = watcher->result();
        watcher->deleteLater();
        m_removeWatcher = nullptr;
        removalDone(removal);
    });
    watcher->setFuture(QtConcurrent::run([targets, toTrash]() -> Removal {
        return removeMarked(targets, toTrash);
    }));
}

void DuplicatesWindow::removalDone(const Removal &removal)
{
    m_removing = false;

    // How it went, in the order of what the user sees: what is gone, what
    // stayed although it was marked, and what was left alone whole.
    QStringList notes;
    qint64 bytes = 0;
    const QSet<QString> removed(removal.removed.cbegin(), removal.removed.cend());
    for (const Model::Entry &entry : m_model->entries()) {
        for (const df::File &file : entry.group.files) {
            if (removed.contains(file.path)) {
                bytes += entry.group.size;
            }
        }
    }
    if (!removal.removed.isEmpty()) {
        notes << (m_trashing ? tr("Moved %1 to the Trash (%2).") : tr("Deleted %1 (%2)."))
                     .arg(filesText(removal.removed.size()), humanSize(bytes));
    }
    if (removal.changed.size() == 1) {
        notes << tr("1 file stayed: it changed since the scan.");
    } else if (!removal.changed.isEmpty()) {
        notes << tr("%1 files stayed: they changed since the scan.").arg(number(removal.changed.size()));
    }
    if (removal.groupsLeft == 1) {
        notes << tr("1 group stayed as it was: none of its other copies is still there.");
    } else if (removal.groupsLeft > 1) {
        notes << tr("%1 groups stayed as they were: none of their other copies are still there.")
                     .arg(number(removal.groupsLeft));
    }
    if (notes.isEmpty()) {
        notes << tr("Nothing was removed.");
    }
    m_notice = notes.join(QLatin1Char(' '));

    QStringList gone = removal.removed;
    gone += removal.gone;
    m_model->forget(gone);
    m_model->uncheck(QSet<QString>(removal.changed.cbegin(), removal.changed.cend()));

    updateStatus();
    updateTotals();
    updateControls();

    // Why something didn't go is said apart from how it went: it is what the
    // user has to see.
    const int count = int(removal.removed.size()) + int(removal.changed.size()) + removal.failed
                      + int(removal.gone.size());
    if (removal.failed > 0) {
        m_dialogs.tell(m_trashing ? tr("To the Trash") : tr("Delete"),
                       tr("%1 of %2 files could not be removed: %3")
                               .arg(number(removal.failed), number(count), removal.error));
    }
    emit removalFinished();
}
