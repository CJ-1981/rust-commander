#include "SearchWindow.h"

#include "app/Dialogs.h"
#include "ops/Shell.h"

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QClipboard>
#include <QCollator>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateEdit>
#include <QDir>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMutex>
#include <QProgressBar>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>
#include <utility>

namespace {

constexpr int kBatchEvery = 100;  // ms between taking what the search found into the table
constexpr int kNameColumn = 0;
constexpr int kFolderColumn = 1;
constexpr int kSizeColumn = 2;
constexpr int kModifiedColumn = 3;
constexpr int kColumnCount = 4;

QString tr(const char *text)
{
    return QCoreApplication::translate("SearchWindow", text);
}

// "1 file", "3 files".
QString counted(int count, const QString &singular, const QString &plural)
{
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? singular : plural);
}

// "1.5 MB" as the tables show it.
QString sizeText(qint64 bytes)
{
    return QLocale::system().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

// "12345" and "1.5 MB" as bytes; -1 when there is nothing there. Suffixes:
// k, m, g, alone or with a "b", whatever the case.
qint64 sizeIn(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return -1;
    }
    qsizetype end = trimmed.size();
    qint64 factor = 1;
    static const QRegularExpression suffix(QStringLiteral("([kmg])b?\\s*$"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = suffix.match(trimmed);
    if (match.hasMatch()) {
        const QChar unit = match.captured(1).at(0).toLower();
        factor = unit == QLatin1Char('k') ? 1024
                                          : unit == QLatin1Char('m') ? 1024 * 1024
                                                                     : qint64(1024) * 1024 * 1024;
        end = match.capturedStart();
    }
    bool ok = false;
    const double number = trimmed.left(end).toDouble(&ok);
    if (!ok || number < 0) {
        return -1;
    }
    return qint64(number * factor);
}

}  // namespace

// The results, as the table shows them. Filled only on the GUI thread, from
// what the worker thread put aside.
class ResultsModel : public QAbstractTableModel {
public:
    explicit ResultsModel(QObject *parent)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_matches.size());
    }
    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : kColumnCount;
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() >= m_matches.size()) {
            return {};
        }
        const FileSearch::Match &match = m_matches.at(index.row());
        switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
            case kNameColumn:
                return match.name;
            case kFolderColumn:
                return QDir::toNativeSeparators(match.folder);
            case kSizeColumn:
                return match.isDir ? QVariant() : QVariant(sizeText(match.size));
            case kModifiedColumn:
                return match.modified.isValid()
                               ? QVariant(QLocale::system().toString(match.modified,
                                                                     QStringLiteral("yyyy-MM-dd HH:mm")))
                               : QVariant();
            default:
                return {};
            }
        case Qt::TextAlignmentRole:
            return index.column() == kSizeColumn ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                                                 : QVariant();
        case Qt::ForegroundRole:
            if (match.hidden) {
                return QGuiApplication::palette().placeholderText().color();
            }
            return {};
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
        case kNameColumn:
            return tr("Name");
        case kFolderColumn:
            return tr("In folder");
        case kSizeColumn:
            return tr("Size");
        case kModifiedColumn:
            return tr("Modified");
        default:
            return {};
        }
    }

    void append(const QList<FileSearch::Match> &matches)
    {
        if (matches.isEmpty()) {
            return;
        }
        beginInsertRows({}, int(m_matches.size()), int(m_matches.size() + matches.size()) - 1);
        m_matches.append(matches);
        endInsertRows();
    }

    void clear()
    {
        beginResetModel();
        m_matches.clear();
        endResetModel();
    }

    const FileSearch::Match *matchAt(int row) const
    {
        return row >= 0 && row < m_matches.size() ? &m_matches.at(row) : nullptr;
    }

private:
    QList<FileSearch::Match> m_matches;
};

// Sorting for the table: names in natural order, whatever the case.
class ResultProxy : public QSortFilterProxyModel {
public:
    explicit ResultProxy(QObject *parent)
        : QSortFilterProxyModel(parent)
    {
        sort(kNameColumn, Qt::AscendingOrder);
    }

protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        // Numbers in names count: file2 before file10.
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        return collator.compare(left.data().toString(), right.data().toString()) < 0;
    }
};

// What the worker thread fills and the window takes every so often.
struct SearchWindow::Shared {
    QMutex mutex;
    QList<FileSearch::Match> pending;  // found since the window took them last
    FileSearch::Progress progress;
};

SearchWindow::SearchWindow(Dialogs &dialogs, QWidget *parent)
    : QDialog(parent)
    , m_dialogs(dialogs)
{
    setAttribute(Qt::WA_DeleteOnClose);
    buildUi();
    m_elapsed.start();
}

SearchWindow::~SearchWindow()
{
    // The worker stops at its next look, and is waited for, so that nothing
    // reports to a window that is gone.
    stopSearch();
    if (m_watcher) {
        m_watcher->waitForFinished();
    }
}

SearchWindow *SearchWindow::open(const QString &startFolder, Dialogs &dialogs, QWidget *parent)
{
    auto *window = new SearchWindow(dialogs, parent);
    window->setStartFolder(startFolder);
    window->show();
    return window;
}

void SearchWindow::buildUi()
{
    resize(980, 620);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    // Where to look, and what to look for.
    auto *first = new QGridLayout;
    first->setHorizontalSpacing(8);
    first->setVerticalSpacing(8);
    first->setColumnStretch(1, 1);
    int row = 0;
    first->addWidget(new QLabel(tr("Folder")), row, 0);
    m_folder = new QLineEdit;
    m_folder->setObjectName("folder");
    m_folder->setPlaceholderText(tr("Where to look"));
    first->addWidget(m_folder, row, 1);
    ++row;
    first->addWidget(new QLabel(tr("Name")), row, 0);
    m_name = new QLineEdit;
    m_name->setObjectName("name");
    m_name->setPlaceholderText(tr("Words in the name; * and ? as wildcards"));
    first->addWidget(m_name, row, 1);
    auto *nameOptions = new QHBoxLayout;
    nameOptions->setSpacing(6);
    m_regex = new QCheckBox(tr("Regular expression"));
    nameOptions->addWidget(m_regex);
    m_matchCase = new QCheckBox(tr("Match case"));
    nameOptions->addWidget(m_matchCase);
    first->addLayout(nameOptions, row, 2);
    ++row;
    first->addWidget(new QLabel(tr("Text")), row, 0);
    m_content = new QLineEdit;
    m_content->setObjectName("content");
    m_content->setPlaceholderText(tr("What files must have in them (optional)"));
    first->addWidget(m_content, row, 1);
    layout->addLayout(first);

    // More options, behind a button: they are not needed every day.
    m_more = new QToolButton;
    m_more->setText(tr("More options"));
    m_more->setCheckable(true);
    connect(m_more, &QToolButton::toggled, this, [this](bool on) { m_options->setVisible(on); });
    layout->addWidget(m_more);

    m_options = new QWidget;
    auto *options = new QGridLayout(m_options);
    options->setContentsMargins(0, 0, 0, 0);
    options->setHorizontalSpacing(8);
    options->setVerticalSpacing(8);
    options->setColumnStretch(1, 1);
    row = 0;
    options->addWidget(new QLabel(tr("Look for")), row, 0);
    m_lookFor = new QComboBox;
    m_lookFor->addItems({tr("Files and folders"), tr("Files only"), tr("Folders only")});
    options->addWidget(m_lookFor, row, 1);
    ++row;
    m_subfolders = new QCheckBox(tr("Look in subfolders"));
    m_subfolders->setChecked(true);
    options->addWidget(m_subfolders, row, 1);
    ++row;
    m_hidden = new QCheckBox(tr("Include hidden files and folders"));
    options->addWidget(m_hidden, row, 1);
    ++row;
    auto *sizes = new QHBoxLayout;
    sizes->setSpacing(6);
    sizes->addWidget(new QLabel(tr("Size from")));
    m_minSize = new QLineEdit;
    m_minSize->setObjectName("minSize");
    m_minSize->setPlaceholderText(tr("none"));
    m_minSize->setMaximumWidth(120);
    sizes->addWidget(m_minSize);
    sizes->addWidget(new QLabel(tr("to")));
    m_maxSize = new QLineEdit;
    m_maxSize->setObjectName("maxSize");
    m_maxSize->setPlaceholderText(tr("none"));
    m_maxSize->setMaximumWidth(120);
    sizes->addWidget(m_maxSize);
    sizes->addWidget(new QLabel(tr("bytes; k, M, G understood")));
    sizes->addStretch(1);
    options->addLayout(sizes, row, 1);
    ++row;
    m_modified = new QCheckBox(tr("Changed between"));
    options->addWidget(m_modified, row, 0);
    auto *dates = new QHBoxLayout;
    dates->setSpacing(6);
    m_modifiedFrom = new QDateEdit;
    m_modifiedFrom->setCalendarPopup(true);
    m_modifiedFrom->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    dates->addWidget(m_modifiedFrom);
    dates->addWidget(new QLabel(tr("and")));
    m_modifiedTo = new QDateEdit;
    m_modifiedTo->setCalendarPopup(true);
    m_modifiedTo->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    dates->addWidget(m_modifiedTo);
    dates->addStretch(1);
    options->addLayout(dates, row, 1);
    for (QDateEdit *field : {m_modifiedFrom, m_modifiedTo}) {
        field->setEnabled(false);
    }
    connect(m_modified, &QCheckBox::toggled, this, [this](bool on) {
        m_modifiedFrom->setEnabled(on);
        m_modifiedTo->setEnabled(on);
    });
    ++row;
    options->addWidget(new QLabel(tr("Look into files up to")), row, 0);
    m_contentLimit = new QComboBox;
    m_contentLimit->addItem(tr("1 MB"), 1024 * 1024);
    m_contentLimit->addItem(tr("10 MB"), 10 * 1024 * 1024);
    m_contentLimit->addItem(tr("100 MB"), 100 * 1024 * 1024);
    m_contentLimit->addItem(tr("no limit"), -1);
    m_contentLimit->setCurrentIndex(2);
    options->addWidget(m_contentLimit, row, 1);
    ++row;
    auto *limitRow = new QHBoxLayout;
    limitRow->setSpacing(6);
    limitRow->addWidget(new QLabel(tr("Stop after")));
    m_limit = new QSpinBox;
    m_limit->setRange(1, 1000000);
    m_limit->setValue(FileSearch::kDefaultMaxResults);
    m_limit->setSuffix(tr(" results"));
    limitRow->addWidget(m_limit);
    limitRow->addStretch(1);
    options->addLayout(limitRow, row, 1);
    m_options->setVisible(false);
    layout->addWidget(m_options);

    // Search and Stop in one button, the progress and the folder being looked
    // into next to it.
    auto *searchRow = new QHBoxLayout;
    searchRow->setSpacing(8);
    m_search = new QToolButton;
    m_search->setText(tr("Search"));
    m_search->setObjectName("search");
    connect(m_search, &QToolButton::clicked, this, &SearchWindow::searchButtonClicked);
    searchRow->addWidget(m_search);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 0);  // how much there is isn't known before it is looked at
    m_progress->setTextVisible(false);
    m_progress->setMaximumWidth(160);
    searchRow->addWidget(m_progress);
    m_current = new QLabel;
    searchRow->addWidget(m_current, 1);
    layout->addLayout(searchRow);

    // The results.
    m_model = new ResultsModel(this);
    m_proxy = new ResultProxy(this);
    m_proxy->setSourceModel(m_model);
    m_results = new QTableView;
    m_results->setModel(m_proxy);
    m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->setShowGrid(false);
    m_results->setWordWrap(false);
    m_results->verticalHeader()->hide();
    m_results->horizontalHeader()->setHighlightSections(false);
    m_results->horizontalHeader()->setSectionResizeMode(kNameColumn, QHeaderView::Stretch);
    m_results->horizontalHeader()->setSectionResizeMode(kFolderColumn, QHeaderView::Stretch);
    m_results->horizontalHeader()->resizeSection(kSizeColumn, 90);
    m_results->horizontalHeader()->resizeSection(kModifiedColumn, 140);
    m_results->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_results, &QTableView::doubleClicked, this, &SearchWindow::openIndex);
    connect(m_results, &QTableView::customContextMenuRequested, this, &SearchWindow::showContextMenu);
    layout->addWidget(m_results, 1);

    m_status = new QLabel;
    m_status->setObjectName("searchStatus");
    m_status->setText(tr("Type what to look for, then click Search."));
    layout->addWidget(m_status);

    // What the search finds arrives in batches.
    m_timer = new QTimer(this);
    m_timer->setInterval(kBatchEvery);
    connect(m_timer, &QTimer::timeout, this, &SearchWindow::takeResults);

    for (QLineEdit *field : {m_folder, m_name, m_content}) {
        connect(field, &QLineEdit::returnPressed, this, &SearchWindow::startSearch);
    }
}

void SearchWindow::setStartFolder(const QString &startFolder)
{
    const QFileInfo info(startFolder);
    // A file: the search starts in the folder it is in.
    m_folder->setText(info.isDir() ? info.absoluteFilePath() : info.absolutePath());
}

void SearchWindow::searchButtonClicked()
{
    if (isSearching()) {
        stopSearch();
    } else {
        startSearch();
    }
}

bool SearchWindow::readQuery(FileSearch::Query *query, QString *error)
{
    const QString folder = m_folder->text().trimmed();
    if (folder.isEmpty()) {
        *error = tr("Say where to look.");
        return false;
    }
    const QFileInfo info(folder);
    if (!info.isDir()) {
        *error = tr("%1 is not a folder.").arg(folder);
        return false;
    }
    query->folders = {info.absoluteFilePath()};
    query->name = m_name->text().trimmed();
    query->nameIsRegex = m_regex->isChecked();
    query->caseSensitive = m_matchCase->isChecked();
    query->what = FileSearch::Query::What(m_lookFor->currentIndex());
    query->includeHidden = m_hidden->isChecked();
    query->recursive = m_subfolders->isChecked();
    query->maxResults = m_limit->value();

    if (!m_minSize->text().trimmed().isEmpty()) {
        query->minSize = sizeIn(m_minSize->text());
    }
    if (!m_maxSize->text().trimmed().isEmpty()) {
        query->maxSize = sizeIn(m_maxSize->text());
    }
    if (query->minSize >= 0 && query->maxSize >= 0 && query->minSize > query->maxSize) {
        *error = tr("The smallest size is bigger than the biggest.");
        return false;
    }
    if (m_modified->isChecked()) {
        query->modifiedAfter = QDateTime(m_modifiedFrom->date(), QTime(0, 0));
        query->modifiedBefore = QDateTime(m_modifiedTo->date(), QTime(23, 59, 59));
        if (query->modifiedAfter > query->modifiedBefore) {
            *error = tr("The dates are the wrong way round.");
            return false;
        }
    }

    const QString text = m_content->text();
    if (!text.isEmpty()) {
        FileSearch::Query::Content content;
        content.text = text;
        content.maxSize = m_contentLimit->currentData().toInt();
        query->content = content;
    }

    // Said before anything is looked at.
    *error = FileSearch::validate(*query);
    return error->isEmpty();
}

void SearchWindow::startSearch()
{
    QString error;
    FileSearch::Query query;
    if (!readQuery(&query, &error)) {
        m_status->setText(error);
        return;
    }
    if (isSearching()) {
        stopSearch();
        if (m_watcher) {
            m_watcher->waitForFinished();
        }
    }

    m_model->clear();
    m_summary.reset();
    m_shared = std::make_shared<Shared>();
    m_stop = std::make_shared<std::atomic<bool>>(false);
    m_elapsed.restart();

    auto *watcher = new QFutureWatcher<FileSearch::Summary>(this);
    m_watcher = watcher;
    const std::shared_ptr<Shared> shared = m_shared;
    const std::shared_ptr<std::atomic<bool>> stop = m_stop;
    connect(watcher, &QFutureWatcher<FileSearch::Summary>::finished, this, [this, watcher] {
        const FileSearch::Summary summary = watcher->result();
        watcher->deleteLater();
        m_watcher = nullptr;
        searchEnded(summary);
    });
    watcher->setFuture(QtConcurrent::run([this, shared, stop, query]() -> FileSearch::Summary {
        return FileSearch::search(
                query,
                [shared](const FileSearch::Match *match, const FileSearch::Progress &progress) {
                    const QMutexLocker lock(&shared->mutex);
                    if (match) {
                        shared->pending.append(*match);
                    }
                    shared->progress = progress;
                },
                *stop);
    }));
    m_progress->show();
    m_current->clear();
    m_search->setText(tr("Stop"));
    m_timer->start();
    updateStatus();
}

void SearchWindow::stopSearch()
{
    if (m_stop) {
        // The search looks at this between folders, and stops there.
        *m_stop = true;
    }
}

QStringList SearchWindow::resultPaths() const
{
    QStringList paths;
    for (int row = 0; row < m_proxy->rowCount(); ++row) {
        const FileSearch::Match *match = matchAt(m_proxy->index(row, kNameColumn));
        if (match) {
            paths.append(match->path);
        }
    }
    return paths;
}

QAbstractItemModel *SearchWindow::resultsModel() const
{
    return m_proxy;
}

const FileSearch::Match *SearchWindow::matchAt(const QModelIndex &index) const
{
    return m_model->matchAt(m_proxy->mapToSource(index).row());
}

void SearchWindow::takeResults()
{
    if (!m_shared) {
        return;
    }
    QList<FileSearch::Match> taken;
    FileSearch::Progress progress;
    {
        const QMutexLocker lock(&m_shared->mutex);
        taken = std::exchange(m_shared->pending, {});
        progress = m_shared->progress;
    }
    if (!taken.isEmpty()) {
        m_model->append(taken);
    }
    m_current->setText(tr("Looking in %1").arg(QDir::toNativeSeparators(progress.current)));
    updateStatus();
}

void SearchWindow::searchEnded(const FileSearch::Summary &summary)
{
    m_summary = summary;
    takeResults();       // what the worker found goes on screen first
    m_shared.reset();    // and then the channel to it is closed
    m_timer->stop();
    m_progress->hide();
    m_current->clear();
    m_search->setText(tr("Search"));
    m_seconds = double(m_elapsed.elapsed()) / 1000.0;
    updateStatus();
    emit searchFinished();
}

void SearchWindow::updateStatus()
{
    const int found = m_proxy->rowCount();
    if (isSearching()) {
        const qint64 entries = m_shared ? m_shared->progress.entries : 0;
        m_status->setText(tr("Searching… %1 found, %2 looked at.")
                              .arg(counted(found, tr("file"), tr("files")))
                              .arg(entries));
        return;
    }
    if (m_summary && !m_summary->error.isEmpty()) {
        m_status->setText(m_summary->error);
        return;
    }
    // How long it took, and how it ended.
    const QString took = tr("%1 s").arg(m_seconds, 0, 'f', 1);
    QString how;
    if (m_summary && m_summary->stopped) {
        how = tr("Stopped after %1: %2 found.").arg(took, counted(found, tr("file"), tr("files")));
    } else {
        how = m_summary && m_summary->capped
                      ? tr("Stopped at the limit of %1 after %2: there may be more.")
                            .arg(m_limit->value())
                            .arg(took)
                      : tr("%1 found in %2.").arg(counted(found, tr("file"), tr("files")), took);
    }
    if (found == 0) {
        m_status->setText(tr("Nothing found. %1").arg(how));
        return;
    }
    m_status->setText(how);
}

QList<FileSearch::Match> SearchWindow::selectedMatches() const
{
    QList<FileSearch::Match> matches;
    const QModelIndexList selected =
            m_results->selectionModel()->selectedRows(kNameColumn);
    for (const QModelIndex &index : selected) {
        if (const FileSearch::Match *match = matchAt(index)) {
            matches.append(*match);
        }
    }
    return matches;
}

void SearchWindow::openIndex(const QModelIndex &index)
{
    const FileSearch::Match *match = matchAt(index);
    if (!match) {
        return;
    }
    // A folder is shown in the system's own file manager; a file opens with
    // its application.
    if (!Shell::open(match->path) && match->isDir) {
        m_dialogs.tell(tr("Open"), tr("%1 could not be opened.").arg(match->path));
    }
}

void SearchWindow::showContextMenu(const QPoint &position)
{
    const QList<FileSearch::Match> matches = selectedMatches();
    if (matches.isEmpty()) {
        return;
    }
    QMenu menu(this);
    QAction *open = menu.addAction(tr("Open"));
    QAction *reveal = menu.addAction(tr("Open containing folder"));
    menu.addSeparator();
    QAction *copyPaths = menu.addAction(tr("Copy path"));
    QAction *copyNames = menu.addAction(tr("Copy name"));
    QAction *picked = menu.exec(m_results->viewport()->mapToGlobal(position));
    if (picked == open) {
        for (const FileSearch::Match &match : matches) {
            Shell::open(match.path);
        }
    } else if (picked == reveal) {
        // The main window can go there itself; when nobody listens, the
        // folder opens in the system's own file manager.
        const bool shown = isSignalConnected(
                QMetaMethod::fromSignal(&SearchWindow::revealRequested));
        for (const FileSearch::Match &match : matches) {
            if (shown) {
                emit revealRequested(match.path);
            } else if (!Shell::open(match.folder)) {
                m_dialogs.tell(tr("Open containing folder"),
                               tr("%1 could not be opened.").arg(match.folder));
            }
        }
    } else if (picked == copyPaths || picked == copyNames) {
        copyToClipboard(picked == copyNames);
    }
}

void SearchWindow::copyToClipboard(bool namesOnly)
{
    QStringList texts;
    for (const FileSearch::Match &match : selectedMatches()) {
        texts.append(namesOnly ? match.name : match.path);
    }
    QGuiApplication::clipboard()->setText(texts.join(QLatin1Char('\n')));
}
