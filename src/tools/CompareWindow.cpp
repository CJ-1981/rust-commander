#include "CompareWindow.h"

#include "app/Dialogs.h"
#include "app/Theme.h"

#include <QBrush>
#include <QCheckBox>
#include <QTextLayout>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kLeftNumberColumn = 0;
constexpr int kLeftColumn = 1;
constexpr int kRightNumberColumn = 2;
constexpr int kRightColumn = 3;
constexpr int kColumnCount = 4;
constexpr qsizetype kBinarySniffBytes = 8192;  // this much is looked at for a zero byte

QString tr(const char *text)
{
    return QCoreApplication::translate("CompareWindow", text);
}

// A tint of a colour, for the background of a row that is a difference.
QColor tinted(const QColor &color, int alpha)
{
    QColor tint = color;
    tint.setAlpha(alpha);
    return tint;
}

// Whether the bytes of a file are text: a zero byte near the start says they
// are not.
bool looksBinary(const QByteArray &bytes)
{
    return bytes.left(int(kBinarySniffBytes)).contains('\0');
}

}  // namespace

// Draws a line of text with the parts that differ marked in the colour of
// the changed row. A span is drawn over its place in the line, which is
// found by measuring what comes before it in the font the line is drawn
// in, so the marking follows the text whatever font it uses.
class SpanDelegate : public QStyledItemDelegate {
public:
    explicit SpanDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const auto model = static_cast<const QAbstractTableModel *>(index.model());
        painter->save();
        painter->fillRect(option.rect, option.palette.base());

        const QString text = index.data(Qt::DisplayRole).toString();
        if (!text.isEmpty()) {
            painter->setFont(option.font);
            const QFontMetricsF metrics(option.font);
            const QColor textColor = option.palette.color(QPalette::Text);
            painter->setPen(textColor);
            painter->drawText(QRectF(option.rect.adjusted(6, 0, -6, 0)),
                              Qt::AlignVCenter | Qt::AlignLeft, text);

            const auto spans = index.column() == 1
                                   ? model->data(index.siblingAtColumn(0), Qt::UserRole + 1)
                                         .value<QVector<LineDiff::Span>>()
                               : index.column() == 3
                                   ? model->data(index.siblingAtColumn(2), Qt::UserRole + 1)
                                         .value<QVector<LineDiff::Span>>()
                                   : QVector<LineDiff::Span>();
            QColor mark = Theme::accent();
            mark.setAlpha(190);
            painter->setPen(mark);
            for (const LineDiff::Span &span : spans) {
                const qreal from = option.rect.left() + 6
                                   + metrics.horizontalAdvance(text.left(span.start));
                painter->drawText(QRectF(from, option.rect.top(), option.rect.width() - 12,
                                         option.rect.height()),
                                  Qt::AlignVCenter | Qt::AlignLeft, text.mid(span.start, span.length));
            }
        }
        painter->restore();
    }
};

// The rows of the table, from the comparison.
class DiffModel : public QAbstractTableModel {
public:
    explicit DiffModel(QObject *parent)
        : QAbstractTableModel(parent)
    {
    }

    void setRows(const QList<LineDiff::Row> &rows)
    {
        beginResetModel();
        m_rows = rows;
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_rows.size());
    }
    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : kColumnCount;
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size()) {
            return {};
        }
        const LineDiff::Row &row = m_rows.at(index.row());
        const bool onLeft = index.column() == kLeftNumberColumn || index.column() == kLeftColumn;
        const bool number = index.column() == kLeftNumberColumn
                            || index.column() == kRightNumberColumn;
        if (role == Qt::UserRole && number) {
            return int(row.kind);
        }
        if (role == Qt::UserRole + 1) {
            const QVector<LineDiff::Span> &spans = onLeft ? row.leftSpans : row.rightSpans;
            QVariant list;
            list.setValue(spans);
            return list;
        }
        switch (role) {
        case Qt::DisplayRole:
            if (number) {
                const int value = onLeft ? row.leftNumber : row.rightNumber;
                return value > 0 ? QString::number(value) : QString();
            }
            if (onLeft) {
                return row.leftNumber > 0 ? row.left : QString();
            }
            return row.rightNumber > 0 ? row.right : QString();
        case Qt::TextAlignmentRole:
            return number ? QVariant(Qt::AlignRight | Qt::AlignVCenter) : QVariant();
        case Qt::ForegroundRole:
            if (row.kind == LineDiff::Kind::Ignored) {
                return QGuiApplication::palette().placeholderText().color();
            }
            return {};
        case Qt::BackgroundRole: {
            const QPalette &palette = QGuiApplication::palette();
            switch (row.kind) {
            case LineDiff::Kind::Removed:
                if (onLeft) {
                    return tinted(QColor(0xd9534f), 70);
                }
                break;
            case LineDiff::Kind::Added:
                if (!onLeft) {
                    return tinted(QColor(0x5cb85c), 70);
                }
                break;
            case LineDiff::Kind::Changed:
                return tinted(QColor(0xf0ad4e), 70);
            case LineDiff::Kind::Ignored:
                return tinted(palette.base().color(), 40);
            case LineDiff::Kind::Equal:
                break;
            }
            return {};
        }
        default:
            return {};
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        switch (section) {
        case kLeftNumberColumn:
        case kRightNumberColumn:
            return QStringLiteral("#");
        case kLeftColumn:
            return tr("Left file");
        case kRightColumn:
            return tr("Right file");
        default:
            return {};
        }
    }

    // For the delegate: the kind of a row (in the number columns), and the
    // parts that differ in a changed row.
    QVariant data(int row, int column, int role) const
    {
        if (role == Qt::UserRole && column == kLeftNumberColumn) {
            return int(m_rows.at(row).kind);
        }
        if (role == Qt::UserRole + 1) {
            const auto &spans = column == kLeftColumn ? m_rows.at(row).leftSpans
                                                      : m_rows.at(row).rightSpans;
            QVariant list;
            list.setValue(spans);
            return list;
        }
        return {};
    }

    // The kinds of the rows, for finding the differences, and a row itself.
    LineDiff::Kind kind(int row) const
    {
        return m_rows.at(row).kind;
    }

    const LineDiff::Row &row(int index) const
    {
        return m_rows.at(index);
    }

private:
    QList<LineDiff::Row> m_rows;
};

CompareWindow *CompareWindow::open(const QString &left, const QString &right, Dialogs &dialogs,
                                   QWidget *parent)
{
    auto *window = new CompareWindow(dialogs, parent);
    window->m_left->setText(left);
    window->m_right->setText(right);
    // With a side left empty, there is nothing to compare yet; the fields
    // hold what was given and the sides are completed in the window.
    if (!left.isEmpty() && !right.isEmpty()) {
        window->compare(left, right);
    }
    window->show();
    return window;
}

CompareWindow::CompareWindow(Dialogs &dialogs, QWidget *parent)
    : QDialog(parent)
    , m_dialogs(dialogs)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1080, 640);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    // The two files, and what to pass over.
    auto *fields = new QGridLayout;
    fields->setHorizontalSpacing(8);
    fields->setVerticalSpacing(6);
    fields->setColumnStretch(1, 1);
    int row = 0;
    fields->addWidget(new QLabel(tr("Left file")), row, 0);
    m_left = new QLineEdit;
    m_left->setObjectName("left");
    m_left->setPlaceholderText(tr("The file on the left"));
    fields->addWidget(m_left, row, 1);
    auto *leftBrowse = new QPushButton(tr("Browse…"));
    connect(leftBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Left file"), m_left->text());
        if (!path.isEmpty()) {
            m_left->setText(path);
        }
    });
    fields->addWidget(leftBrowse, row, 2);
    ++row;
    fields->addWidget(new QLabel(tr("Right file")), row, 0);
    m_right = new QLineEdit;
    m_right->setObjectName("right");
    m_right->setPlaceholderText(tr("The file on the right"));
    fields->addWidget(m_right, row, 1);
    auto *rightBrowse = new QPushButton(tr("Browse…"));
    connect(rightBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Right file"), m_right->text());
        if (!path.isEmpty()) {
            m_right->setText(path);
        }
    });
    fields->addWidget(rightBrowse, row, 2);
    layout->addLayout(fields);

    auto *optionsRow = new QHBoxLayout;
    optionsRow->setSpacing(10);
    m_whitespace = new QCheckBox(tr("Ignore spaces at the end of a line"));
    m_whitespace->setChecked(true);
    optionsRow->addWidget(m_whitespace);
    m_lineEndings = new QCheckBox(tr("Ignore line endings"));
    m_lineEndings->setChecked(true);
    optionsRow->addWidget(m_lineEndings);
    m_blank = new QCheckBox(tr("Ignore blank lines"));
    optionsRow->addWidget(m_blank);
    connect(m_whitespace, &QCheckBox::toggled, this, [this] { compare(m_left->text(), m_right->text()); });
    connect(m_lineEndings, &QCheckBox::toggled, this, [this] { compare(m_left->text(), m_right->text()); });
    connect(m_blank, &QCheckBox::toggled, this, [this] { compare(m_left->text(), m_right->text()); });
    layout->addLayout(optionsRow);

    // Compare, swap, and through the differences.
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_compare = new QPushButton(tr("Compare"));
    m_compare->setObjectName("compare");
    m_compare->setDefault(true);
    connect(m_compare, &QPushButton::clicked, this,
            [this] { compare(m_left->text(), m_right->text()); });
    buttons->addWidget(m_compare);
    m_swap = new QPushButton(tr("Swap sides"));
    m_swap->setObjectName("swap");
    connect(m_swap, &QPushButton::clicked, this, [this] {
        const QString left = m_left->text();
        m_left->setText(m_right->text());
        m_right->setText(left);
        compare(m_left->text(), m_right->text());
        emit swapped();
    });
    buttons->addWidget(m_swap);
    m_previous = new QPushButton(tr("Previous difference"));
    m_previous->setEnabled(false);
    connect(m_previous, &QPushButton::clicked, this, [this] { gotoDifference(-1); });
    buttons->addWidget(m_previous);
    m_next = new QPushButton(tr("Next difference"));
    m_next->setEnabled(false);
    connect(m_next, &QPushButton::clicked, this, [this] { gotoDifference(1); });
    buttons->addWidget(m_next);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    // The two texts, side by side in one table: the scrolling is the same on
    // both sides by itself.
    m_model = new DiffModel(this);
    m_table = new QTableView;
    m_table->setObjectName("table");
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->horizontalHeader()->resizeSection(kLeftNumberColumn, 46);
    m_table->horizontalHeader()->resizeSection(kRightNumberColumn, 46);
    m_table->horizontalHeader()->setSectionResizeMode(kLeftColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(kRightColumn, QHeaderView::Stretch);
    m_table->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto *spans = new SpanDelegate(m_table);
    m_table->setItemDelegateForColumn(kLeftColumn, spans);
    m_table->setItemDelegateForColumn(kRightColumn, new SpanDelegate(m_table));
    layout->addWidget(m_table, 1);

    m_status = new QLabel;
    m_status->setObjectName("compareStatus");
    layout->addWidget(m_status);
}

// The lines the two files have, read here. A file that is not text is
// refused, and said so.
bool readTextFile(const QString &path, QString *content, QString *error)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        *error = tr("%1 is not a file.").arg(path.isEmpty() ? tr("(nothing)") : path);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (looksBinary(bytes)) {
        *error = tr("%1 is not a text file.").arg(path);
        return false;
    }
    *content = QString::fromUtf8(bytes);
    return true;
}

bool CompareWindow::compare(const QString &left, const QString &right)
{
    QString leftText;
    QString rightText;
    QString error;
    if (!left.isEmpty() && !readTextFile(left, &leftText, &error)) {
        m_status->setText(error);
        return false;
    }
    if (!right.isEmpty() && !readTextFile(right, &rightText, &error)) {
        m_status->setText(error);
        return false;
    }

    LineDiff::Options options;
    options.ignoreTrailingWhitespace = m_whitespace->isChecked();
    options.ignoreLineEndings = m_lineEndings->isChecked();
    options.ignoreBlankLines = m_blank->isChecked();

    const LineDiff::Result result = LineDiff::compare(LineDiff::split(leftText, options),
                                                      LineDiff::split(rightText, options), options);
    m_model->setRows(result.rows);
    m_table->resizeColumnsToContents();

    const int differences = result.removed + result.added;
    m_next->setEnabled(differences > 0);
    m_previous->setEnabled(differences > 0);
    if (differences == 0) {
        m_status->setText(tr("The files are the same."));
    } else {
        QStringList what;
        if (result.removed > 0) {
            what << tr("%1 removed").arg(result.removed);
        }
        if (result.added > 0) {
            what << tr("%1 added").arg(result.added);
        }
        m_status->setText(tr("%1 differences: %2.").arg(differences).arg(what.join(QStringLiteral(", "))));
    }
    return true;
}

QList<LineDiff::Row> CompareWindow::rows() const
{
    QList<LineDiff::Row> rows;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        rows.append(m_model->row(row));
    }
    return rows;
}

int CompareWindow::differences() const
{
    int count = 0;
    for (const LineDiff::Row &row : rows()) {
        if (row.kind == LineDiff::Kind::Removed || row.kind == LineDiff::Kind::Added
            || row.kind == LineDiff::Kind::Changed) {
            ++count;
        }
    }
    return count;
}

// Scrolls to the difference `step` rows after (or before) the cursor, and
// puts the cursor on it.
void CompareWindow::gotoDifference(int step)
{
    const int count = m_model->rowCount();
    if (count == 0) {
        return;
    }
    int row = m_table->selectionModel()->currentIndex().row();
    if (row < 0) {
        row = step > 0 ? -1 : count;
    }
    // The rows between the cursor and the next difference are passed over.
    row += step;
    while (row >= 0 && row < count
           && (m_model->kind(row) == LineDiff::Kind::Equal
               || m_model->kind(row) == LineDiff::Kind::Ignored)) {
        row += step;
    }
    if (row < 0 || row >= count) {
        return;
    }
    const QModelIndex index = m_model->index(row, kLeftColumn);
    m_table->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
    m_table->scrollTo(index, QAbstractItemView::PositionAtCenter);
}

void CompareWindow::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // The table needs to know how wide it is before the text can be laid out.
    m_table->resizeColumnsToContents();
}
