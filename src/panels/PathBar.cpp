#include "PathBar.h"

#include "app/Theme.h"
#include "ops/Shell.h"

#include <QAction>
#include <QCompleter>
#include <QCoreApplication>
#include <QDir>
#include <QFileSystemModel>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStackedLayout>

namespace {

constexpr int kPadding = 6;    // on each side of a folder's name
constexpr int kRadius = 4;
constexpr int kChevron = 16;   // room for the "›" between two folders
constexpr int kMore = -2;      // the index of the "…" that stands for folders left out

QString tr(const char *text)
{
    return QCoreApplication::translate("PathBar", text);
}

// What to call the root of a volume: its name, if it has one.
QString volumeName(const QString &root)
{
    for (const Shell::Volume &volume : Shell::volumes()) {
        if (QDir::cleanPath(QDir::fromNativeSeparators(volume.root)) == root) {
            return volume.name;
        }
    }
    return root;
}

}  // namespace

// The row of folder buttons.
class PathBar::Crumbs : public QWidget {
    Q_OBJECT

public:
    explicit Crumbs(QWidget *parent)
        : QWidget(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
    }

    void setPath(const QString &path)
    {
        m_path = QDir::cleanPath(path);
        m_items.clear();
        QDir dir(m_path);
        while (true) {
            const QString here = QDir::cleanPath(dir.absolutePath());
            const QString name = dir.dirName();
            // The root of a volume has no name of its own.
            m_items.prepend(Item{name.isEmpty() ? volumeName(here) : name, here});
            if (!dir.cdUp()) {
                break;
            }
        }
        relayout();
        updateGeometry();
        update();
    }

    QStringList folders() const
    {
        QStringList paths;
        for (const Item &item : m_items) {
            paths.append(item.path);
        }
        return paths;
    }

    QRect folderRect(int index) const
    {
        return index >= 0 && index < m_items.size() ? m_items.at(index).rect : QRect();
    }

    QSize sizeHint() const override
    {
        return QSize(240, fontMetrics().height() + 12);
    }

signals:
    void chosen(const QString &path);
    void typed(const QString &text);
    void clickedEmpty();

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QColor text = palette().color(QPalette::Text);
        const QColor muted = Theme::muted();

        const auto button = [&](const QRect &rect, const QString &label, bool current, int index) {
            if (index == m_hover || index == m_pressed) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(Theme::hover());
                painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), kRadius, kRadius);
            }
            painter.setPen(current ? text : muted);
            painter.drawText(rect.adjusted(kPadding, 0, -kPadding, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             label);
        };

        // "…" stands for the folders there was no room for.
        if (!m_moreRect.isEmpty()) {
            button(m_moreRect, QStringLiteral("\u2026"), false, kMore);
        }
        for (int i = m_first; i < m_items.size(); ++i) {
            const bool last = i == m_items.size() - 1;
            button(m_items[i].rect, m_items[i].text, last, i);
            if (!last) {
                painter.setPen(muted);
                painter.drawText(QRect(m_items[i].rect.right() + 1, 0, kChevron, height()),
                                 Qt::AlignCenter, QStringLiteral("\u203a"));
            }
        }

        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(Theme::accent(), 1.0));
            painter.setRenderHint(QPainter::Antialiasing);
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
        }
    }

    void resizeEvent(QResizeEvent *) override { relayout(); }

    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange
            || event->type() == QEvent::PaletteChange) {
            relayout();
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        m_pressed = indexAt(event->position().toPoint());
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        const int index = indexAt(event->position().toPoint());
        const int pressed = m_pressed;
        m_pressed = -1;
        update();
        // A click is a press and a release on the same thing.
        if (index != pressed) {
            return;
        }
        if (index == kMore) {
            chooseLeftOut(event->position().toPoint());
        } else if (index >= 0) {
            emit chosen(m_items.at(index).path);
        } else {
            emit clickedEmpty();
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int hover = indexAt(event->position().toPoint());
        if (hover == m_hover) {
            return;
        }
        m_hover = hover;
        setCursor(hover == -1 ? Qt::ArrowCursor : Qt::PointingHandCursor);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QWidget::leaveEvent(event);
        m_hover = -1;
        update();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            emit clickedEmpty();
            return;
        }
        // Typing starts a path: the first character is kept.
        const QString text = event->text();
        if (!text.isEmpty()
            && (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::ShiftModifier)) {
            emit typed(text);
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    struct Item {
        QString name;
        QString path;
        QString text;  // the name, shortened if there was no room
        QRect rect;    // empty when the folder isn't shown
    };

    // Works out what is shown where. When the row is too narrow, folders go
    // from the outside in; the current folder stays.
    void relayout()
    {
        const QFontMetrics metrics = fontMetrics();
        const int count = m_items.size();
        const int room = width();
        QList<int> widths;
        for (const Item &item : m_items) {
            widths.append(metrics.horizontalAdvance(item.name) + 2 * kPadding);
        }
        const int more = metrics.horizontalAdvance(QStringLiteral("\u2026")) + 2 * kPadding;

        const auto rowWidth = [&](int first) {
            int result = 0;
            for (int i = first; i < count; ++i) {
                result += widths.at(i) + (i < count - 1 ? kChevron : 0);
            }
            return result + (first > 0 ? more + kChevron : 0);
        };

        int first = 0;
        while (first < count - 1 && rowWidth(first) > room) {
            ++first;
        }

        int x = 0;
        m_moreRect = QRect();
        if (first > 0) {
            m_moreRect = QRect(0, 0, more, height());
            x = more + kChevron;
        }
        m_first = first;
        for (int i = 0; i < count; ++i) {
            m_items[i].rect = QRect();
        }
        for (int i = first; i < count; ++i) {
            const bool last = i == count - 1;
            const int available = qMax(0, room - x - (last ? 0 : kChevron));
            const int width = qMin(widths.at(i), available);
            m_items[i].text =
                    metrics.elidedText(m_items[i].name, Qt::ElideMiddle, qMax(0, width - 2 * kPadding));
            m_items[i].rect = QRect(x, 0, width, height());
            x += width + kChevron;
        }
    }

    // What is at `position`: a folder's index, kMore, or -1 for empty space.
    int indexAt(const QPoint &position) const
    {
        if (m_moreRect.contains(position)) {
            return kMore;
        }
        for (int i = m_first; i < m_items.size(); ++i) {
            if (m_items.at(i).rect.contains(position)) {
                return i;
            }
        }
        return -1;
    }

    // Offers the folders that were left out.
    void chooseLeftOut(const QPoint &at)
    {
        QMenu menu(this);
        QList<QAction *> actions;
        for (int i = 0; i < m_first; ++i) {
            actions.append(menu.addAction(m_items.at(i).name));
        }
        QAction *picked = menu.exec(mapToGlobal(at));
        for (int i = 0; i < actions.size(); ++i) {
            if (actions.at(i) == picked) {
                emit chosen(m_items.at(i).path);
            }
        }
    }

    QString m_path;
    QList<Item> m_items;
    int m_first = 0;         // the first folder shown
    QRect m_moreRect;        // where "…" is, when some folders were left out
    int m_hover = -1;
    int m_pressed = -1;
};

PathBar::PathBar(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_crumbs = new Crumbs(this);

    m_edit = new QLineEdit(this);
    m_edit->setClearButtonEnabled(true);
    m_edit->setPlaceholderText(tr("Type a path"));
    // Completes folder names while the path is typed.
    auto *completer = new QCompleter(m_edit);
    auto *model = new QFileSystemModel(completer);
    model->setRootPath(QDir::rootPath());
    model->setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    completer->setModel(model);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    m_edit->setCompleter(completer);

    m_layout = new QStackedLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->addWidget(m_crumbs);
    m_layout->addWidget(m_edit);

    connect(m_crumbs, &Crumbs::chosen, this, &PathBar::pathChosen);
    connect(m_crumbs, &Crumbs::typed, this, &PathBar::typed);
    connect(m_crumbs, &Crumbs::clickedEmpty, this, &PathBar::edit);
    connect(m_edit, &QLineEdit::returnPressed, this, &PathBar::accept);
    // The field lost focus, by a click elsewhere or by Tab: what was typed is dropped.
    connect(m_edit, &QLineEdit::editingFinished, this, [this] { finish(false); });
}

PathBar::~PathBar()
{
    // The field loses the focus while the bar is taken apart, which it
    // reports as editing finished; there is nothing left to switch to then.
    m_editing = false;
    disconnect(m_edit, nullptr, this, nullptr);
}

void PathBar::setPath(const QString &path)
{
    m_path = QDir::cleanPath(path);
    finish(false);
    m_crumbs->setPath(m_path);
}

QStringList PathBar::folders() const
{
    return m_crumbs->folders();
}

QRect PathBar::folderRect(int index) const
{
    return m_crumbs->folderRect(index);
}

bool PathBar::isEditing() const
{
    return m_editing;
}

QSize PathBar::sizeHint() const
{
    return QSize(240, qMax(m_edit->sizeHint().height(), m_crumbs->sizeHint().height()));
}

void PathBar::edit()
{
    if (isEditing()) {
        return;
    }
    m_editing = true;
    m_edit->setText(m_path);
    m_layout->setCurrentWidget(m_edit);
    m_edit->setFocus(Qt::OtherFocusReason);
    m_edit->selectAll();
    emit editingStarted();
}

void PathBar::stopEditing()
{
    finish(false);
}

// Typing started on the buttons themselves: the field begins with what was typed.
void PathBar::typed(const QString &text)
{
    edit();
    m_edit->setText(text);
    m_edit->end(false);
}

void PathBar::accept()
{
    QString typed = m_edit->text().trimmed();
    if (typed.startsWith(QLatin1Char('~'))) {
        typed = QDir::homePath() + typed.mid(1);
    }
    // A relative path is taken from the folder shown.
    const QString path =
            QDir::cleanPath(QDir::isAbsolutePath(typed) ? typed : QDir(m_path).absoluteFilePath(typed));
    finish(true);
    emit pathChosen(path);
}

void PathBar::finish(bool chosen)
{
    if (!isEditing()) {
        return;
    }
    // Said first: hiding the field takes the focus from it, which is reported
    // as editing finished too.
    m_editing = false;
    m_layout->setCurrentWidget(m_crumbs);
    emit editingEnded(chosen);
}

void PathBar::keyPressEvent(QKeyEvent *event)
{
    // The field doesn't use Escape itself; it reaches the bar.
    if (isEditing() && event->key() == Qt::Key_Escape) {
        stopEditing();
        return;
    }
    QWidget::keyPressEvent(event);
}

#include "PathBar.moc"
