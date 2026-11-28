#include "MultiRenameWindow.h"

#include "app/Dialogs.h"
#include "app/Theme.h"
#include "ops/FileOps.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace {

// How long after the last change of the rules the table follows them, so
// that a long list doesn't make typing slow.
constexpr int kFollowDelay = 120;

QString tr(const char *text)
{
    return QCoreApplication::translate("MultiRenameWindow", text);
}

QString countFiles(int count)
{
    return count == 1 ? tr("1 file") : tr("%1 files").arg(count);
}

QString countProblems(int count)
{
    return count == 1 ? tr("1 problem") : tr("%1 problems").arg(count);
}

QString countUnchanged(int count)
{
    return count == 1 ? tr("1 unchanged") : tr("%1 unchanged").arg(count);
}

// What a name is to the file system. On macOS and on Windows two names lead
// to the same file when they differ only in case, or in whether a letter with
// its accent is one character or two.
QString nameKey(const QString &name)
{
    return name.normalized(QString::NormalizationForm_C).toCaseFolded();
}

// The names a folder holds, found by nameKey().
class FolderNames {
public:
    explicit FolderNames(const QString &folder)
    {
        const QStringList names =
                QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                                       QDir::NoSort);
        for (const QString &name : names) {
            m_byKey[nameKey(name)].append(name);
        }
    }

    // Whether a file could be called `newName` in the folder, when the file
    // to be renamed is called `ownName` now: nothing else may have that name.
    bool isFree(const QString &newName, const QString &ownName) const
    {
        for (const QString &entry : m_byKey.value(nameKey(newName))) {
            if (nameKey(entry) != nameKey(ownName)) {
                return false;
            }
        }
        return true;
    }

private:
    QHash<QString, QStringList> m_byKey;
};

bool onDarkBase()
{
    return QApplication::palette().color(QPalette::Base).lightness() < 128;
}

// Red for what went wrong and green for what was done, to be read on the
// base colour of either theme.
QColor problemColor()
{
    return onDarkBase() ? QColor(0xf8, 0x71, 0x71) : QColor(0xb4, 0x23, 0x18);
}

QColor doneColor()
{
    return onDarkBase() ? QColor(0x4a, 0xde, 0x80) : QColor(0x15, 0x80, 0x3d);
}

// The folder of a file, as the table shows it: the home folder is ~.
QString shownFolder(const QString &folder)
{
    const QString home = QDir::homePath();
    QString shown = folder;
    if (folder == home) {
        shown = QStringLiteral("~");
    } else if (folder.startsWith(home + QLatin1Char('/'))) {
        shown = QStringLiteral("~") + folder.mid(home.size());
    }
    return QDir::toNativeSeparators(shown);
}

using State = MultiRenameWindow::State;

// What the Status column says of a row.
QString statusOf(const MultiRenameWindow::Row &row)
{
    switch (row.state) {
    case State::Rename:
        return QString();
    case State::Unchanged:
        return tr("Unchanged");
    case State::Problem:
        return row.message;
    case State::Renamed:
        return tr("Renamed");
    case State::Failed:
        return tr("Could not be renamed: %1").arg(row.message);
    case State::Restored:
        return tr("Renamed back");
    case State::RestoreFailed:
        return tr("Could not be renamed back: %1").arg(row.message);
    }
    return QString();
}

}  // namespace

// The table: one row per file. It reads the window's rows, which it may as a friend.
class PreviewModel : public QAbstractTableModel {
public:
    enum Column {
        NameColumn = 0,
        NewNameColumn,
        FolderColumn,
        StatusColumn,
        ColumnCount,
    };

    explicit PreviewModel(MultiRenameWindow *window)
        : QAbstractTableModel(window)
        , m_window(window)
    {
    }

    // The rows are worked out again. When there are as many as before, the
    // selection and the scroll position stay where they are.
    void changed()
    {
        const int count = rowCount();
        if (count == m_count) {
            if (count > 0) {
                emit dataChanged(index(0, 0), index(count - 1, ColumnCount - 1));
            }
        } else {
            beginResetModel();
            m_count = count;
            endResetModel();
        }
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_window->m_rows.size());
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : ColumnCount;
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() >= m_window->m_rows.size()) {
            return QVariant();
        }
        const int row = index.row();
        const int column = index.column();
        const MultiRenameWindow::Row &entry = m_window->m_rows.at(row);

        if (role == Qt::DisplayRole) {
            switch (column) {
            case NameColumn:
                return entry.name;
            case NewNameColumn:
                return entry.newName;
            case FolderColumn:
                return shownFolder(m_window->m_files.at(row).folder);
            case StatusColumn:
                return statusOf(entry);
            default:
                return QVariant();
            }
        }
        // The whole of what a cell shows, as a long name or path is cut short.
        if (role == Qt::ToolTipRole) {
            switch (column) {
            case NameColumn:
                return entry.name;
            case NewNameColumn:
                return entry.newName;
            case FolderColumn:
                return QDir::toNativeSeparators(m_window->m_files.at(row).folder);
            case StatusColumn:
                return statusOf(entry);
            default:
                return QVariant();
            }
        }
        if (role == Qt::ForegroundRole) {
            switch (entry.state) {
            case State::Unchanged:
                return QVariant(Theme::muted());  // dimmed: nothing happens to it
            case State::Problem:
            case State::Failed:
            case State::RestoreFailed:
                return column == StatusColumn || column == NewNameColumn ? QVariant(problemColor()) : QVariant();
            case State::Renamed:
            case State::Restored:
                return column == StatusColumn ? QVariant(doneColor()) : QVariant();
            default:
                return QVariant();
            }
        }
        return QVariant();
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return QAbstractTableModel::headerData(section, orientation, role);
        }
        switch (section) {
        case NameColumn:
            return tr("Name");
        case NewNameColumn:
            return tr("New name");
        case FolderColumn:
            return tr("Folder");
        case StatusColumn:
            return tr("Status");
        default:
            return QVariant();
        }
    }

private:
    MultiRenameWindow *m_window;
    int m_count = 0;  // what the views were told of last
};

namespace {

class PreviewDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        // The end of a name is its extension, and the end of a path its folder: those stay in view.
        option->textElideMode = index.column() == PreviewModel::StatusColumn ? Qt::ElideRight : Qt::ElideMiddle;
    }
};

// A placeholder on the row of buttons under the mask.
struct Placeholder {
    QString button;  // on the button
    QString code;    // what goes into the mask
    QString tip;
};

QList<Placeholder> placeholders()
{
    const QString date = tr("of the file's modification date, as two digits");
    return {
            {tr("[N] Name"), QStringLiteral("[N]"),
             tr("The name without its extension. [N2-5] is characters 2 to 5 of it, [N-3] the third from the end.")},
            {tr("[E] Extension"), QStringLiteral("[E]"),
             tr("The extension, without the dot. [E1-2] is the first two characters of it.")},
            {tr("[C] Counter"), QStringLiteral("[C]"),
             tr("The counter: 1, 2, 3 from the first file on. [C10] starts at 10, [C+5] five above the start, "
                "[C:3] three digits wide.")},
            {tr("[Y] Year"), QStringLiteral("[Y]"), tr("The year %1, as four digits.").arg(date)},
            {tr("[M] Month"), QStringLiteral("[M]"), tr("The month %1.").arg(date)},
            {tr("[D] Day"), QStringLiteral("[D]"), tr("The day %1.").arg(date)},
            {tr("[h] Hour"), QStringLiteral("[h]"), tr("The hour %1, on the 24-hour clock.").arg(date)},
            {tr("[m] Minute"), QStringLiteral("[m]"), tr("The minute %1.").arg(date)},
            {tr("[s] Second"), QStringLiteral("[s]"), tr("The second %1.").arg(date)},
    };
}

}  // namespace

MultiRenameWindow *MultiRenameWindow::open(const QStringList &files, Dialogs &dialogs, QWidget *parent)
{
    auto *window = new MultiRenameWindow(dialogs, parent);
    window->setFiles(files);
    window->show();
    return window;
}

MultiRenameWindow::MultiRenameWindow(Dialogs &dialogs, QWidget *parent)
    : QDialog(parent)
    , m_timer(new QTimer(this))
{
    Q_UNUSED(dialogs);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Multi-rename"));
    resize(960, 600);

    m_timer->setSingleShot(true);
    m_timer->setInterval(kFollowDelay);
    buildUi();
    updateStatus();
    // A new mask is what most come for: the old one is selected, ready to be typed over.
    m_mask->setFocus();
    m_mask->selectAll();
}

void MultiRenameWindow::buildUi()
{
    const RenameRules::Settings defaults;

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(10);

    // The rules.
    auto *form = new QGridLayout;
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    form->setColumnStretch(1, 1);
    form->setColumnStretch(3, 1);
    layout->addLayout(form);

    m_mask = new QLineEdit;
    m_mask->setObjectName("mask");
    m_mask->setText(defaults.mask);
    m_mask->setToolTip(
            tr("The new name, out of text and placeholders in brackets. [N] is the name, [E] the extension, [C] "
               "the counter, [Y] [M] [D] the date and [h] [m] [s] the time.\n"
               "From [U], [L] or [F] on, the mask is in upper case, in lower case, or has the first letter of "
               "each word in upper case; [n] goes back to as it was. "
               "A bracket that belongs in the name is [[] or []]."));
    auto *maskLabel = new QLabel(tr("&Mask:"));
    maskLabel->setBuddy(m_mask);
    form->addWidget(maskLabel, 0, 0);
    form->addWidget(m_mask, 0, 1, 1, 4);

    // The placeholders, as buttons that put them where the mask's cursor is.
    auto *chips = new QHBoxLayout;
    chips->setSpacing(2);
    auto *insert = new QLabel(tr("Insert:"));
    insert->setForegroundRole(QPalette::PlaceholderText);
    chips->addWidget(insert);
    for (const Placeholder &placeholder : placeholders()) {
        auto *button = new QToolButton;
        button->setText(placeholder.button);
        button->setToolTip(placeholder.tip);
        // The mask keeps the focus, so that typing goes on there.
        button->setFocusPolicy(Qt::NoFocus);
        const QString code = placeholder.code;
        connect(button, &QToolButton::clicked, this, [this, code] { m_mask->insert(code); });
        chips->addWidget(button);
    }
    chips->addStretch(1);
    form->addLayout(chips, 1, 1, 1, 4);

    m_find = new QLineEdit;
    m_find->setToolTip(tr("Where this is in the new name, \"Replace with\" comes instead. Text matches in any "
                          "case; a regular expression as it says."));
    auto *findLabel = new QLabel(tr("&Find:"));
    findLabel->setBuddy(m_find);
    form->addWidget(findLabel, 2, 0);
    form->addWidget(m_find, 2, 1);

    m_replace = new QLineEdit;
    m_replace->setToolTip(tr("Empty leaves what was found out."));
    auto *replaceLabel = new QLabel(tr("Replace &with:"));
    replaceLabel->setBuddy(m_replace);
    form->addWidget(replaceLabel, 2, 2);
    form->addWidget(m_replace, 2, 3);

    m_regularExpression = new QCheckBox(tr("Re&gular expression"));
    m_regularExpression->setToolTip(
            tr("\"Find\" is a regular expression. In \"Replace with\", $1, $2 and so on stand for what the "
               "pattern's groups matched, ${name} for a group with a name, and $$ for a $."));
    form->addWidget(m_regularExpression, 2, 4);

    m_case = new QComboBox;
    m_case->addItem(tr("No change"));
    m_case->addItem(tr("lowercase"));
    m_case->addItem(tr("UPPERCASE"));
    m_case->addItem(tr("First Letter Of Each Word"));
    auto *caseLabel = new QLabel(tr("&Case:"));
    caseLabel->setBuddy(m_case);
    form->addWidget(caseLabel, 3, 0);
    form->addWidget(m_case, 3, 1);

    auto *start = new QHBoxLayout;
    auto *startLabel = new QLabel(tr("Counter &start:"));
    m_counterStart = new QLineEdit;
    m_counterStart->setText(QString::number(defaults.counterStart));
    m_counterStart->setValidator(new QIntValidator(-999999999, 999999999, m_counterStart));
    m_counterStart->setToolTip(
            tr("The counter of the first file; it goes up by one for each file down the table."));
    startLabel->setBuddy(m_counterStart);
    start->addWidget(startLabel);
    start->addWidget(m_counterStart);
    form->addLayout(start, 3, 2);

    auto *width = new QHBoxLayout;
    auto *widthLabel = new QLabel(tr("&Digits:"));
    m_counterWidth = new QLineEdit;
    m_counterWidth->setText(QString::number(defaults.counterWidth));
    m_counterWidth->setValidator(new QIntValidator(1, 18, m_counterWidth));
    m_counterWidth->setFixedWidth(56);
    m_counterWidth->setToolTip(tr("How many digits the counter has at least: 007 rather than 7."));
    widthLabel->setBuddy(m_counterWidth);
    width->addWidget(widthLabel);
    width->addWidget(m_counterWidth);
    form->addLayout(width, 3, 3);

    m_underscores = new QCheckBox(tr("Replace underscores with spaces"));
    form->addWidget(m_underscores, 3, 4);

    // A counter field shows the number it stands for once it is left.
    for (QLineEdit *field : {m_counterStart, m_counterWidth}) {
        const bool digits = field == m_counterWidth;
        connect(field, &QLineEdit::editingFinished, this, [field, digits] {
            field->setText(QString::number(number(field, digits)));
        });
    }

    // The preview.
    m_model = new PreviewModel(this);
    m_table = new QTableView;
    m_table->setModel(m_model);
    m_table->setItemDelegate(new PreviewDelegate(m_table));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    QHeaderView *header = m_table->horizontalHeader();
    header->setHighlightSections(false);
    // The order of the rows is the order of the counter: the table isn't for sorting.
    header->setSectionsClickable(false);
    const int widths[] = {250, 250, 200, 230};
    for (int column = 0; column < PreviewModel::ColumnCount; ++column) {
        header->resizeSection(column, widths[column]);
        header->setSectionResizeMode(column, QHeaderView::Stretch);
    }
    layout->addWidget(m_table, 1);

    // What will happen, and the way to make it happen.
    auto *bottom = new QHBoxLayout;
    m_status = new QLabel;
    bottom->addWidget(m_status, 1);
    m_undoButton = new QPushButton(tr("&Undo"));
    m_undoButton->setToolTip(tr("Give the files of the last rename the names they had."));
    auto *close = new QPushButton(tr("Close"));
    m_renameButton = new QPushButton(tr("&Rename"));
    for (QPushButton *button : {m_undoButton, close, m_renameButton}) {
        // Enter in a field is not "rename": it is pressed too easily halfway through a mask.
        button->setAutoDefault(false);
        bottom->addWidget(button);
    }
    layout->addLayout(bottom);

    const auto follow = [this] { scheduleUpdate(); };
    for (QLineEdit *field : {m_mask, m_find, m_replace, m_counterStart, m_counterWidth}) {
        connect(field, &QLineEdit::textChanged, this, follow);
    }
    connect(m_regularExpression, &QCheckBox::toggled, this, follow);
    connect(m_underscores, &QCheckBox::toggled, this, follow);
    connect(m_case, &QComboBox::currentIndexChanged, this, follow);
    connect(m_timer, &QTimer::timeout, this, [this] { rebuild(); });
    connect(m_renameButton, &QPushButton::clicked, this, [this] { renameAll(); });
    connect(m_undoButton, &QPushButton::clicked, this, [this] { undo(); });
    connect(close, &QPushButton::clicked, this, &QWidget::close);
}

int MultiRenameWindow::number(QLineEdit *field, bool atLeastOne)
{
    bool ok = false;
    const int value = field->text().toInt(&ok);
    // What isn't a number yet, as while it is being typed, stands for the default.
    return ok ? (atLeastOne ? qMax(1, value) : value) : 1;
}

void MultiRenameWindow::setFiles(const QStringList &files)
{
    // A new set of files, a fresh plan: the fields back to what they were.
    resetSettings();
    m_timer->stop();
    m_updatePending = false;
    m_files.clear();
    m_rowOfPath.clear();
    for (const QString &path : files) {
        const QFileInfo info(path);
        File file;
        file.path = QDir::cleanPath(info.absoluteFilePath());
        file.folder = QFileInfo(file.path).absolutePath();
        file.name = QFileInfo(file.path).fileName();
        file.modified = info.lastModified();
        file.exists = info.exists();
        // A file that is in the list twice is one file.
        if (!m_rowOfPath.contains(file.path)) {
            m_rowOfPath.insert(file.path, int(m_files.size()));
            m_files.append(file);
        }
    }
    rebuild();
}

QStringList MultiRenameWindow::files() const
{
    QStringList paths;
    for (const File &file : m_files) {
        paths.append(file.path);
    }
    return paths;
}

void MultiRenameWindow::setMask(const QString &mask)
{
    m_mask->setText(mask);
    updatePreview();
}

void MultiRenameWindow::setFind(const QString &find)
{
    m_find->setText(find);
    updatePreview();
}

void MultiRenameWindow::setReplace(const QString &replace)
{
    m_replace->setText(replace);
    updatePreview();
}

void MultiRenameWindow::setRegularExpression(bool on)
{
    m_regularExpression->setChecked(on);
    updatePreview();
}

void MultiRenameWindow::setCase(RenameRules::Case mode)
{
    m_case->setCurrentIndex(int(mode));
    updatePreview();
}

void MultiRenameWindow::setCounterStart(int start)
{
    m_counterStart->setText(QString::number(start));
    updatePreview();
}

void MultiRenameWindow::setCounterWidth(int width)
{
    m_counterWidth->setText(QString::number(width));
    updatePreview();
}

void MultiRenameWindow::setUnderscoresToSpaces(bool on)
{
    m_underscores->setChecked(on);
    updatePreview();
}

// The fields as they were when the window opened.
void MultiRenameWindow::resetSettings()
{
    const RenameRules::Settings defaults;
    m_mask->setText(defaults.mask);
    m_find->clear();
    m_replace->clear();
    m_regularExpression->setChecked(false);
    m_case->setCurrentIndex(0);
    m_counterStart->setText(QString::number(defaults.counterStart));
    m_counterWidth->setText(QString::number(defaults.counterWidth));
    m_underscores->setChecked(false);
    rebuild();
}

RenameRules::Settings MultiRenameWindow::settings() const
{
    RenameRules::Settings settings;
    settings.mask = m_mask->text();
    settings.find = m_find->text();
    settings.replace = m_replace->text();
    settings.regularExpression = m_regularExpression->isChecked();
    // The choices of the box are in the order of the enum.
    settings.caseMode = RenameRules::Case(qBound(0, m_case->currentIndex(), int(RenameRules::Case::Title)));
    settings.counterStart = number(m_counterStart, false);
    settings.counterWidth = number(m_counterWidth, true);
    settings.underscoresToSpaces = m_underscores->isChecked();
    return settings;
}

QList<MultiRenameWindow::Row> MultiRenameWindow::rows() const
{
    return m_rows;
}

int MultiRenameWindow::renameCount() const
{
    int count = 0;
    for (const Row &row : m_rows) {
        count += row.state == State::Rename;
    }
    return count;
}

int MultiRenameWindow::problemCount() const
{
    int count = 0;
    for (const Row &row : m_rows) {
        count += row.state == State::Problem;
    }
    return count;
}

bool MultiRenameWindow::canUndo() const
{
    return !m_batches.isEmpty();
}

void MultiRenameWindow::scheduleUpdate()
{
    m_updatePending = true;
    m_timer->start();
}

void MultiRenameWindow::updatePreview()
{
    if (m_updatePending) {
        rebuild();
    }
}

void MultiRenameWindow::rebuild(const QHash<int, Outcome> &outcomes)
{
    m_timer->stop();
    m_updatePending = false;
    m_settings = settings();
    // What is wrong with the rules goes for every file.
    const QString problem = RenameRules::problem(m_settings);

    QHash<QString, FolderNames> folders;
    const auto namesIn = [&folders](const QString &folder) -> const FolderNames & {
        auto it = folders.find(folder);
        if (it == folders.end()) {
            it = folders.insert(folder, FolderNames(folder));
        }
        return it.value();
    };

    // What is there now, not what was there when the files were listed.
    for (File &file : m_files) {
        file.exists = QFileInfo::exists(file.path);
    }

    m_rows.clear();
    // The folder and new name a row wants, and the rows that want it.
    QHash<QString, QList<int>> wanted;
    for (int row = 0; row < m_files.size(); ++row) {
        const File &file = m_files.at(row);
        Row entry;
        entry.name = file.name;
        entry.path = file.path;
        entry.state = State::Problem;
        if (!file.exists) {
            entry.message = tr("The file isn't there anymore.");
        } else if (!problem.isEmpty()) {
            entry.message = problem;
        } else {
            const RenameRules::Result result =
                    RenameRules::newName({file.name, file.modified, row}, m_settings);
            entry.newName = result.name;
            if (!result.ok()) {
                entry.message = result.error;
            } else if (result.name == file.name) {
                entry.state = State::Unchanged;
            } else if (!namesIn(file.folder).isFree(result.name, file.name)) {
                entry.message = tr("Something with this name is already in the folder.");
            } else {
                entry.state = State::Rename;
                wanted[file.folder + QLatin1Char('\n') + nameKey(result.name)].append(row);
            }
        }
        m_rows.append(entry);
    }

    // Two files that would end up with the same name: neither is renamed, so that neither goes lost.
    for (const QList<int> &wanters : std::as_const(wanted)) {
        if (wanters.size() < 2) {
            continue;
        }
        for (const int row : wanters) {
            // The other file that wants the name, whichever row this is.
            const int other = row == wanters.first() ? wanters.at(1) : wanters.first();
            m_rows[row].state = State::Problem;
            m_rows[row].message = tr("The same new name as %1.").arg(m_files.at(other).name);
        }
    }

    for (auto it = outcomes.cbegin(); it != outcomes.cend(); ++it) {
        Row &row = m_rows[it.key()];
        row.name = it.value().before;
        row.newName = it.value().after;
        row.state = it.value().state;
        row.message = it.value().message;
    }
    m_model->changed();
    updateStatus();
}

void MultiRenameWindow::updateStatus()
{
    m_renameButton->setEnabled(renameCount() > 0);
    m_undoButton->setEnabled(canUndo());
    // What is wrong with the rules is said in red: it is the one thing to fix.
    const QString problem = RenameRules::problem(m_settings);
    m_status->setStyleSheet(problem.isEmpty() ? QString()
                                              : QStringLiteral("color: %1;").arg(problemColor().name()));
    m_status->setText(statusText());
}

QString MultiRenameWindow::statusText() const
{
    const QString problem = RenameRules::problem(m_settings);
    if (!problem.isEmpty()) {
        return problem;  // every row says it too
    }
    const int total = int(m_rows.size());
    if (total == 0) {
        return tr("No files to rename.");
    }

    int rename = 0;
    int unchanged = 0;
    int withAProblem = 0;
    int renamed = 0;
    int failed = 0;
    int restored = 0;
    int notRestored = 0;
    for (const Row &row : m_rows) {
        switch (row.state) {
        case State::Rename:
            ++rename;
            break;
        case State::Unchanged:
            ++unchanged;
            break;
        case State::Problem:
            ++withAProblem;
            break;
        case State::Renamed:
            ++renamed;
            break;
        case State::Failed:
            ++failed;
            break;
        case State::Restored:
            ++restored;
            break;
        case State::RestoreFailed:
            ++notRestored;
            break;
        }
    }

    QStringList sentences;
    // How the last rename or Undo went.
    if (renamed + failed > 0) {
        sentences << (failed > 0 ? tr("%1 renamed, %2 could not be").arg(countFiles(renamed), countFiles(failed))
                                 : tr("%1 renamed").arg(countFiles(renamed)));
    }
    if (restored + notRestored > 0) {
        sentences << (notRestored > 0 ? tr("%1 given back their names, %2 could not be")
                                            .arg(countFiles(restored), countFiles(notRestored))
                                      : tr("%1 given back their names").arg(countFiles(restored)));
    }
    // What the rules would do now; after a rename or an Undo only when there is something left to do.
    if (rename > 0 || sentences.isEmpty()) {
        QString preview;
        if (rename == 0) {
            preview = tr("No file will be renamed");
        } else if (rename < total) {
            preview = tr("%1 of %2 will be renamed").arg(rename).arg(countFiles(total));
        } else {
            preview = total == 1 ? tr("The file will be renamed")
                                 : tr("All %1 will be renamed").arg(countFiles(total));
        }
        QStringList tails;
        if (withAProblem > 0) {
            tails << countProblems(withAProblem);
        }
        if (unchanged > 0) {
            tails << countUnchanged(unchanged);
        }
        if (!tails.isEmpty()) {
            preview += QStringLiteral(", ") + tails.join(QStringLiteral(", "));
        }
        sentences << preview;
    }
    return sentences.join(QStringLiteral(". "));
}

int MultiRenameWindow::renameAll()
{
    // What the table shows is the plan; this is the plan that is carried out.
    updatePreview();
    QList<Renaming> batch;
    QHash<int, Outcome> outcomes;
    for (int row = 0; row < m_rows.size(); ++row) {
        if (m_rows.at(row).state != State::Rename) {
            continue;
        }
        File &file = m_files[row];
        Outcome outcome;
        outcome.state = State::Renamed;
        outcome.before = file.name;
        outcome.after = m_rows.at(row).newName;
        if (!FileOps::rename(file.path, outcome.after, &outcome.message)) {
            // The next file is not held up by this one.
            outcome.state = State::Failed;
            // Gone since the files were read? Then the row is to say so from now on.
            file.exists = QFileInfo::exists(file.path);
        } else {
            Renaming renaming;
            renaming.path = file.folder + QLatin1Char('/') + outcome.after;
            renaming.oldName = file.name;
            batch.append(renaming);
            m_rowOfPath.remove(file.path);
            file.path = renaming.path;
            file.name = outcome.after;
            m_rowOfPath.insert(file.path, row);
        }
        outcomes.insert(row, outcome);
    }
    if (!batch.isEmpty()) {
        m_batches.append(batch);
    }
    rebuild(outcomes);
    if (!batch.isEmpty()) {
        emit filesChanged();
    }
    return int(batch.size());
}

int MultiRenameWindow::undo()
{
    if (m_batches.isEmpty()) {
        return 0;
    }
    const QList<Renaming> batch = m_batches.takeLast();
    QList<Renaming> stillToDo;
    QHash<int, Outcome> outcomes;
    int restored = 0;
    // The file that was renamed last goes back first.
    for (int i = int(batch.size()) - 1; i >= 0; --i) {
        const Renaming &renaming = batch.at(i);
        Outcome outcome;
        outcome.state = State::Restored;
        outcome.before = QFileInfo(renaming.path).fileName();
        outcome.after = renaming.oldName;

        int row = rowOf(renaming.path);
        if (row < 0) {
            // Not in the table any more, because other files were set since: it comes back with its report.
            const QFileInfo info(renaming.path);
            row = int(m_files.size());
            File file;
            file.path = renaming.path;
            file.folder = info.absolutePath();
            file.name = info.fileName();
            file.modified = info.lastModified();
            file.exists = info.exists();
            m_rowOfPath.insert(file.path, row);
            m_files.append(file);
        }
        File &file = m_files[row];
        if (FileOps::rename(renaming.path, renaming.oldName, &outcome.message)) {
            ++restored;
            m_rowOfPath.remove(file.path);
            file.path = file.folder + QLatin1Char('/') + renaming.oldName;
            file.name = renaming.oldName;
            m_rowOfPath.insert(file.path, row);
        } else {
            outcome.state = State::RestoreFailed;
            stillToDo.append(renaming);
            file.exists = QFileInfo::exists(file.path);
        }
        outcomes.insert(row, outcome);
    }
    // What couldn't be given back stays in, to be tried again.
    if (!stillToDo.isEmpty()) {
        m_batches.append(stillToDo);
    }
    rebuild(outcomes);
    if (restored > 0) {
        emit filesChanged();
    }
    return restored;
}

int MultiRenameWindow::rowOf(const QString &path) const
{
    return m_rowOfPath.value(path, -1);
}
