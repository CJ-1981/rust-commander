#include "ChecksumWindow.h"

#include "app/Dialogs.h"
#include "panels/FileModel.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProgressBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <chrono>
#include <optional>
#include <utility>

namespace {

using namespace std::chrono_literals;

constexpr auto kReportEvery = 60ms;  // how often the worker thread reports to the window at most
constexpr int kNoticeMs = 4000;      // how long the status line says what was done last
constexpr int kAtOnce = 8;           // the expected hash grew by this many digits at once: it was pasted, not typed

enum Column {
    FileColumn = 0,
    StatusColumn,
    HashColumn,
    ColumnCount,
};

QString tr(const char *text)
{
    return QCoreApplication::translate("ChecksumWindow", text);
}

// Whether the theme in use is dark: the colour of a window says so.
bool darkTheme()
{
    return QApplication::palette().window().color().lightness() < 128;
}

QColor mutedColor()
{
    return QApplication::palette().placeholderText().color();
}

// The colours that say how a check came out; each of them reads on a window of either theme.
QColor goodColor()
{
    return darkTheme() ? QColor(0x4ade80) : QColor(0x15803d);
}

QColor badColor()
{
    return darkTheme() ? QColor(0xf87171) : QColor(0xb91c1c);
}

QColor warningColor()
{
    return darkTheme() ? QColor(0xfbbf24) : QColor(0x92400e);
}

QColor statusColor(Checksums::Status status)
{
    switch (status) {
    case Checksums::Status::Ok:
        return goodColor();
    case Checksums::Status::Different:
        return badColor();
    case Checksums::Status::Missing:
    case Checksums::Status::Unreadable:
        return warningColor();
    case Checksums::Status::NotChecked:
        return mutedColor();
    }
    return QColor();
}

// A label's text in `color`, whatever else it says.
void colorize(QLabel *label, const QColor &color)
{
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, color);
    palette.setColor(QPalette::Text, color);
    label->setPalette(palette);
}

// A round badge with a tick, a cross or an exclamation mark. It is drawn here,
// as a font need not have such a sign.
QPixmap badge(ChecksumWindow::Verdict verdict, const QColor &color, qreal ratio)
{
    QPixmap pixmap(qRound(22 * ratio), qRound(22 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(0.5, 0.5, 21, 21));

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::white, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (verdict == ChecksumWindow::Verdict::Match) {
        QPainterPath tick;
        tick.moveTo(6.4, 11.6);
        tick.lineTo(9.7, 14.8);
        tick.lineTo(15.6, 7.6);
        painter.drawPath(tick);
    } else if (verdict == ChecksumWindow::Verdict::Different) {
        painter.drawLine(QPointF(7.4, 7.4), QPointF(14.6, 14.6));
        painter.drawLine(QPointF(14.6, 7.4), QPointF(7.4, 14.6));
    } else {
        painter.drawLine(QPointF(11, 5.8), QPointF(11, 12.4));
        painter.setBrush(Qt::white);
        painter.drawEllipse(QPointF(11, 16.2), 1.3, 1.3);
    }
    return pixmap;
}

// A button as the app's windows have them: flat, with its name on it.
QToolButton *button(const QString &text)
{
    auto *result = new QToolButton;
    result->setText(text);
    result->setFocusPolicy(Qt::TabFocus);
    return result;
}

// A field. It takes no drops of its own: a file dropped anywhere lands on the window.
QLineEdit *field()
{
    auto *result = new QLineEdit;
    result->setAcceptDrops(false);
    return result;
}

// A row of a form: the field, and the buttons after it.
QHBoxLayout *formRow(QWidget *first, std::initializer_list<QWidget *> buttons)
{
    auto *row = new QHBoxLayout;
    row->setSpacing(6);
    row->addWidget(first, 1);
    for (QWidget *after : buttons) {
        row->addWidget(after);
    }
    return row;
}

// A form as the pages have them.
QFormLayout *form()
{
    auto *result = new QFormLayout;
    result->setContentsMargins(0, 0, 0, 0);
    result->setHorizontalSpacing(12);
    result->setVerticalSpacing(8);
    result->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    return result;
}

// A page: its content at the top, whatever room there is below it.
QVBoxLayout *pageLayout(QWidget *page)
{
    auto *result = new QVBoxLayout(page);
    result->setContentsMargins(0, 14, 0, 0);
    result->setSpacing(16);
    return result;
}

// The digits of a hash as they are typed, without the spaces.
QString digitsOf(const QString &text)
{
    QString digits;
    for (const QChar c : text) {
        if (!c.isSpace()) {
            digits += c;
        }
    }
    return digits.toLower();
}

// Where a file dialog starts: the folder of what a field shows, else home.
QString startFolder(const QString &text)
{
    return text.isEmpty() ? QDir::homePath() : QFileInfo(text).absolutePath();
}

// The files of a drop. Folders are left out: a folder has no hash of its own.
QStringList localFiles(const QMimeData &mime)
{
    QStringList paths;
    for (const QUrl &url : mime.urls()) {
        // A folder has no hash of its own.
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
            paths.append(url.toLocalFile());
        }
    }
    return paths;
}

}  // namespace

// The worker thread's report while a job runs. The window is told at most so
// often; what was found for the files so far goes with it.
class ChecksumWindow::Bridge : public Checksums::Progress {
public:
    Bridge(ChecksumWindow *window, const std::shared_ptr<std::atomic<bool>> &cancel, quint64 id)
        : m_window(window)
        , m_cancel(cancel)
        , m_id(id)
    {
    }

    bool bytes(qint64 done, qint64 total) override
    {
        if (m_cancel->load()) {
            return false;
        }
        m_advance.done = done;
        m_advance.total = total;
        report();
        return true;
    }

    bool checked(const Checksums::Check &check, int done, int count) override
    {
        m_batch.append(check);
        m_advance.files = done;
        m_advance.count = count;
        m_advance.current = check.item.name;
        report();
        return !m_cancel->load();
    }

private:
    void report()
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_last && now - *m_last < kReportEvery) {
            return;
        }
        m_last = now;

        const QList<Checksums::Check> batch = std::exchange(m_batch, {});
        QMetaObject::invokeMethod(
                m_window,
                [window = m_window, id = m_id, from = m_posted, batch, advance = m_advance] {
                    window->showAdvance(id, advance);
                    for (int i = 0; i < batch.size(); ++i) {
                        window->showCheck(id, from + i, batch.at(i));
                    }
                },
                Qt::QueuedConnection);
        m_posted += int(batch.size());
    }

    ChecksumWindow *m_window;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    quint64 m_id;
    QList<Checksums::Check> m_batch;  // what was found since the last report
    int m_posted = 0;                 // how many files were reported so far
    Advance m_advance;
    std::optional<std::chrono::steady_clock::time_point> m_last;
};

ChecksumWindow *ChecksumWindow::open(const QString &file, Dialogs &dialogs, QWidget *parent)
{
    auto *window = new ChecksumWindow(dialogs, parent);
    window->takeFile(file);
    window->show();
    return window;
}

ChecksumWindow::ChecksumWindow(Dialogs &dialogs, QWidget *parent)
    : QDialog(parent)
    , m_dialogs(dialogs)
    , m_monospace(QFontDatabase::systemFont(QFontDatabase::FixedFont))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true);
    setWindowTitle(tr("Checksum"));
    resize(880, 560);
    setMinimumSize(600, 440);
    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(1);
    buildUi();
}

ChecksumWindow::~ChecksumWindow()
{
    // The worker stops at its next report. It has to be done before anything
    // it reports to goes away.
    if (m_cancel) {
        *m_cancel = true;
    }
    m_pool->waitForDone();
}

void ChecksumWindow::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(10);

    // The two things the window does; what belongs to them is below.
    m_tabs = new QTabBar;
    m_tabs->setDocumentMode(true);
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->setFocusPolicy(Qt::TabFocus);
    m_tabs->addTab(tr("Compare"));
    m_tabs->addTab(tr("Verify a checksum file"));
    layout->addWidget(m_tabs);

    m_pages = new QStackedWidget;
    m_pages->addWidget(buildComparePage());
    m_pages->addWidget(buildVerifyPage());
    layout->addWidget(m_pages);

    // Takes the room that the table doesn't need, so that the last row stays at the bottom.
    layout->addStretch();

    // How far the work is; it keeps its room while nothing goes on.
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progressText = new QLabel;
    m_stop = button(tr("Stop"));
    auto *progressRow = new QHBoxLayout;
    progressRow->setSpacing(10);
    progressRow->addWidget(m_progress, 1);
    progressRow->addWidget(m_progressText);
    progressRow->addWidget(m_stop);
    layout->addLayout(progressRow);
    for (QWidget *widget : {static_cast<QWidget *>(m_progress), static_cast<QWidget *>(m_progressText),
                            static_cast<QWidget *>(m_stop)}) {
        QSizePolicy policy = widget->sizePolicy();
        policy.setVerticalPolicy(QSizePolicy::Fixed);
        policy.setRetainSizeWhenHidden(true);
        widget->setSizePolicy(policy);
        widget->hide();
    }

    // The files of the checksum file, or the files that were dropped.
    m_table = new QTableWidget(0, ColumnCount);
    m_table->setObjectName("checks");
    m_table->setHorizontalHeaderLabels({tr("File"), tr("Status"), tr("Checksum")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAcceptDrops(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(FileColumn, 260);
    m_table->setColumnWidth(StatusColumn, 110);
    layout->addWidget(m_table, 1);

    // The line along the bottom.
    m_status = new QLabel;
    m_status->setObjectName("status");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_verify = button(tr("Verify"));
    m_save = button(tr("Save checksum file\u2026"));
    auto *statusRow = new QHBoxLayout;
    statusRow->setSpacing(6);
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_verify);
    statusRow->addWidget(m_save);
    layout->addLayout(statusRow);

    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) { showPage(Page(index)); });
    connect(m_stop, &QToolButton::clicked, this, &ChecksumWindow::stop);
    connect(m_verify, &QToolButton::clicked, this, &ChecksumWindow::verify);
    connect(m_save, &QToolButton::clicked, this, &ChecksumWindow::browseForTarget);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex &current) {
                if (current.isValid()) {
                    m_current = current.row();
                    updateCompare();
                }
            });
    showPage(Page::Compare);
}

QWidget *ChecksumWindow::buildComparePage()
{
    auto *page = new QWidget;
    auto *layout = pageLayout(page);
    QFormLayout *rows = form();
    layout->addLayout(rows);

    m_fileField = field();
    m_fileField->setPlaceholderText(tr("Choose a file, or drop one on this window"));
    m_browseFile = button(tr("Browse\u2026"));
    rows->addRow(tr("File"), formRow(m_fileField, {m_browseFile}));

    m_algorithmBox = new QComboBox;
    for (Checksums::Algorithm algorithm : {Checksums::Algorithm::Md5, Checksums::Algorithm::Sha1,
                                           Checksums::Algorithm::Sha256, Checksums::Algorithm::Sha512,
                                           Checksums::Algorithm::Crc32}) {
        m_algorithmBox->addItem(Checksums::algorithmName(algorithm), int(algorithm));
    }
    m_algorithmBox->setCurrentIndex(m_algorithmBox->findData(int(m_algorithm)));
    m_algorithmBox->setToolTip(tr("SHA-256 is what downloads are published with. MD5, SHA-1 and CRC32 "
                                  "tell a damaged file from a whole one, but not whether someone changed "
                                  "the file on purpose."));
    m_sizeLabel = new QLabel;
    auto *algorithmRow = new QHBoxLayout;
    algorithmRow->setSpacing(12);
    algorithmRow->addWidget(m_algorithmBox);
    algorithmRow->addWidget(m_sizeLabel, 1);
    rows->addRow(tr("Algorithm"), algorithmRow);

    m_digestField = field();
    m_digestField->setReadOnly(true);
    m_digestField->setFont(m_monospace);
    m_digestField->setPlaceholderText(tr("The hash of the file"));
    m_copy = button(tr("Copy"));
    rows->addRow(tr("Checksum"), formRow(m_digestField, {m_copy}));

    m_expected = field();
    m_expected->setFont(m_monospace);
    m_expected->setPlaceholderText(tr("Paste the hash the file should have"));
    m_paste = button(tr("Paste"));
    m_clear = button(tr("Clear"));
    rows->addRow(tr("Expected"), formRow(m_expected, {m_paste, m_clear}));

    // What it comes to, in words and colour and a sign.
    m_verdictIcon = new QLabel;
    m_verdictIcon->setFixedSize(22, 22);
    m_verdict = new QLabel;
    QFont verdictFont = m_verdict->font();
    verdictFont.setPointSizeF(verdictFont.pointSizeF() + 3);
    verdictFont.setBold(true);
    m_verdict->setFont(verdictFont);
    m_verdict->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *verdictRow = new QHBoxLayout;
    verdictRow->setSpacing(8);
    verdictRow->addWidget(m_verdictIcon, 0, Qt::AlignVCenter);
    verdictRow->addWidget(m_verdict, 1);
    layout->addLayout(verdictRow);

    layout->addStretch(1);

    connect(m_browseFile, &QToolButton::clicked, this, &ChecksumWindow::browseForFile);
    connect(m_fileField, &QLineEdit::editingFinished, this, [this] {
        const QString text = m_fileField->text().trimmed();
        if (!text.isEmpty() && (m_compare.size() != 1 || text != m_compare.first().item.path)) {
            setFiles({text});
        }
    });
    connect(m_algorithmBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        setAlgorithm(Checksums::Algorithm(m_algorithmBox->itemData(index).toInt()));
    });
    connect(m_copy, &QToolButton::clicked, this, &ChecksumWindow::copyHash);
    connect(m_expected, &QLineEdit::textChanged, this, &ChecksumWindow::expectedChanged);
    connect(m_expected, &QLineEdit::editingFinished, this, &ChecksumWindow::expectedSettled);
    connect(m_paste, &QToolButton::clicked, this, &ChecksumWindow::paste);
    connect(m_clear, &QToolButton::clicked, this, [this] {
        m_expected->clear();
        m_expected->setFocus();
    });
    return page;
}

QWidget *ChecksumWindow::buildVerifyPage()
{
    auto *page = new QWidget;
    auto *layout = pageLayout(page);
    QFormLayout *rows = form();
    layout->addLayout(rows);
    layout->addStretch(1);

    m_listField = field();
    m_listField->setPlaceholderText(tr("A .sha256, .md5 or .sfv file, or any file that lists checksums"));
    m_browseList = button(tr("Browse\u2026"));
    rows->addRow(tr("Checksum file"), formRow(m_listField, {m_browseList}));

    m_folderField = field();
    m_folderField->setPlaceholderText(tr("The folder with the files; that of the checksum file by default"));
    m_browseFolder = button(tr("Browse\u2026"));
    rows->addRow(tr("Folder"), formRow(m_folderField, {m_browseFolder}));

    connect(m_browseList, &QToolButton::clicked, this, &ChecksumWindow::browseForList);
    connect(m_listField, &QLineEdit::editingFinished, this, [this] {
        const QString text = m_listField->text().trimmed();
        if (!text.isEmpty() && text != m_list.path) {
            setChecksumFile(text);
        }
    });
    connect(m_browseFolder, &QToolButton::clicked, this, &ChecksumWindow::browseForFolder);
    connect(m_folderField, &QLineEdit::editingFinished, this, [this] {
        if (m_folderField->text().trimmed() != m_folder) {
            setFolder(m_folderField->text());
        }
    });
    return page;
}

void ChecksumWindow::browseForFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a file"),
                                                      startFolder(m_fileField->text()));
    if (!path.isEmpty()) {
        setFiles({path});
    }
}

void ChecksumWindow::browseForList()
{
    const QString path = QFileDialog::getOpenFileName(
            this, tr("Choose a checksum file"), startFolder(m_listField->text()),
            tr("Checksum files (*.md5 *.sha1 *.sha256 *.sha512 *.sfv);;All files (*)"));
    if (!path.isEmpty()) {
        setChecksumFile(path);
    }
}

void ChecksumWindow::browseForFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(
            this, tr("Choose the folder with the files"),
            m_folder.isEmpty() ? startFolder(m_listField->text()) : m_folder);
    if (!folder.isEmpty()) {
        setFolder(folder);
    }
}

void ChecksumWindow::takeFile(const QString &path)
{
    if (Checksums::isChecksumFile(path)) {
        showPage(Page::Verify);
        if (setChecksumFile(path)) {
            return;
        }
    }
    setFiles({path});
}

void ChecksumWindow::showPage(Page page)
{
    const int index = int(page);
    if (m_tabs->currentIndex() != index) {
        m_tabs->setCurrentIndex(index);  // which comes back here
        return;
    }
    m_pages->setCurrentIndex(index);
    m_verify->setVisible(page == Page::Verify);
    m_current = 0;
    refresh();
}

ChecksumWindow::Page ChecksumWindow::page() const
{
    return Page(m_tabs->currentIndex());
}

QList<Checksums::Check> *ChecksumWindow::rowsFor(Page page)
{
    return page == Page::Compare ? &m_compare : &m_checks;
}

const QList<Checksums::Check> *ChecksumWindow::rowsFor(Page page) const
{
    return page == Page::Compare ? &m_compare : &m_checks;
}

bool ChecksumWindow::waiting(Page page) const
{
    if (m_watcher && m_running.page == page) {
        return true;
    }
    for (const Request &request : m_queue) {
        if (request.page == page) {
            return true;
        }
    }
    return false;
}

// Whether the job `id` is the one running, and still the one its page wants to see.
bool ChecksumWindow::isCurrent(quint64 id) const
{
    return m_watcher && m_running.id == id && m_newest[size_t(m_running.page)] == id;
}

void ChecksumWindow::setFiles(const QStringList &paths)
{
    showPage(Page::Compare);

    Request order;
    order.page = Page::Compare;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        order.items.append(Checksums::Item{info.fileName(), info.absoluteFilePath(), m_algorithm, QString()});
    }
    // The field shows the one file; several are in the table.
    m_fileField->setText(paths.size() == 1 ? order.items.first().path
                                           : paths.size() > 1 ? tr("%1 files").arg(paths.size()) : QString());
    m_fileField->setToolTip(paths.join(QLatin1Char('\n')));
    m_compareBytes = 0;
    request(order);
}

QStringList ChecksumWindow::files() const
{
    QStringList paths;
    for (const Checksums::Check &check : m_compare) {
        paths.append(check.item.path);
    }
    return paths;
}

void ChecksumWindow::setAlgorithm(Checksums::Algorithm algorithm)
{
    if (algorithm == m_algorithm) {
        return;
    }
    m_algorithm = algorithm;
    m_algorithmBox->setCurrentIndex(m_algorithmBox->findData(int(algorithm)));  // which comes back here

    // The hashes there are of another kind now.
    Request order;
    order.page = Page::Compare;
    for (const Checksums::Check &check : m_compare) {
        order.items.append(Checksums::Item{check.item.name, check.item.path, m_algorithm, QString()});
    }
    m_compareBytes = 0;
    request(order);
}

QString ChecksumWindow::hash() const
{
    return m_current < m_compare.size() ? m_compare.at(m_current).actual : QString();
}

bool ChecksumWindow::isHashing() const
{
    return m_watcher != nullptr || !m_queue.isEmpty();
}

void ChecksumWindow::stop()
{
    // What waits for its turn is not started.
    m_queue.clear();
    if (m_watcher) {
        m_halted = m_running.id;
        *m_cancel = true;
    }
    refresh();
}

bool ChecksumWindow::setChecksumFile(const QString &path)
{
    Checksums::ChecksumFile file;
    QString error;
    if (!Checksums::read(path, &file, &error)) {
        // What was listed before is gone, and nothing of it is checked any more.
        m_list = Checksums::ChecksumFile();
        m_folder.clear();
        m_checks.clear();
        m_problem = error;
        m_listField->setText(path);
        m_folderField->clear();
        withdraw(Page::Verify);
        refresh();
        return false;
    }

    m_list = file;
    m_problem.clear();
    m_listField->setText(file.path);
    const QString kind = file.algorithm ? Checksums::algorithmName(*file.algorithm) : tr("Mixed algorithms");
    m_listField->setToolTip(tr("%1, %2 lines").arg(kind).arg(file.entries.size()));
    // The files are where the list is, unless the user says otherwise.
    setFolder(QFileInfo(file.path).absolutePath());
    return true;
}

void ChecksumWindow::setFolder(const QString &folder)
{
    const QString typed = folder.trimmed();
    m_folder = typed.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(typed).absoluteFilePath());
    m_folderField->setText(m_folder);
    if (m_list.entries.isEmpty()) {
        return;
    }

    Request order;
    order.page = Page::Verify;
    order.items = Checksums::itemsIn(m_folder, m_list.entries);
    request(order);
}

QList<Checksums::Check> ChecksumWindow::checks() const
{
    return m_checks;
}

void ChecksumWindow::verify()
{
    // What the field shows, for when it was typed and Enter was not pressed.
    setFolder(m_folderField->text().trimmed() == m_folder ? m_folder : m_folderField->text());
}

void ChecksumWindow::request(Request asked)
{
    asked.id = m_nextId++;
    m_newest[size_t(asked.page)] = asked.id;

    // Rows for what is to come, from the start: what was found before is gone.
    QList<Checksums::Check> &rows = *rowsFor(asked.page);
    rows.clear();
    for (const Checksums::Item &item : asked.items) {
        rows.append(Checksums::Check{item});
    }

    // An older request of the same page that still waits is of no use now.
    m_queue.removeIf([page = asked.page](const Request &waiting) { return waiting.page == page; });
    m_queue.append(asked);

    // The job that runs stops at its next report; finishedJob() takes it from there.
    if (m_watcher) {
        *m_cancel = true;
        refresh();
    } else {
        startNext();
    }
}

void ChecksumWindow::withdraw(Page page)
{
    m_newest[size_t(page)] = m_nextId++;
    m_queue.removeIf([page](const Request &waiting) { return waiting.page == page; });
    if (m_watcher && m_running.page == page) {
        *m_cancel = true;
    }
}

void ChecksumWindow::startNext()
{
    if (m_queue.isEmpty()) {
        refresh();
        return;
    }

    m_running = m_queue.takeFirst();
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_advance = Advance();

    auto *watcher = new QFutureWatcher<QList<Checksums::Check>>(this);
    m_watcher = watcher;
    const Request request = m_running;
    connect(watcher, &QFutureWatcher<QList<Checksums::Check>>::finished, this, [this, watcher, request] {
        const QList<Checksums::Check> checks = watcher->result();
        watcher->deleteLater();
        m_watcher = nullptr;
        finishedJob(request, checks);
    });

    const QList<Checksums::Item> items = request.items;
    const auto cancel = m_cancel;
    watcher->setFuture(QtConcurrent::run(m_pool, [this, items, id = request.id, cancel] {
        Bridge bridge(this, cancel, id);
        QList<Checksums::Check> checks;
        Checksums::check(items, &checks, &bridge);
        return checks;
    }));
    refresh();
}

void ChecksumWindow::finishedJob(const Request &request, const QList<Checksums::Check> &checks)
{
    const bool halted = m_halted == request.id;                 // the user stopped this one
    const bool madeRoom = m_cancel->load() && !halted;          // it stopped for a job that had to go first
    m_halted = 0;
    m_running = Request{};

    // Unless its page asked for something else in the meantime, this is what the page shows.
    const bool kept = m_newest[size_t(request.page)] == request.id;
    if (kept) {
        *rowsFor(request.page) = checks;
        if (madeRoom) {
            // It goes on once the job that went first is over.
            m_queue.append(request);
        }
    }
    refresh();

    // Of a check that ran to its end, what came out worst is what to see first.
    if (kept && !halted && !madeRoom && request.page == page()) {
        for (int row = 0; row < checks.size(); ++row) {
            if (checks.at(row).status != Checksums::Status::Ok) {
                m_table->setCurrentCell(row, FileColumn);
                break;
            }
        }
    }
    startNext();
}

void ChecksumWindow::showAdvance(quint64 id, const Advance &advance)
{
    if (!isCurrent(id)) {
        return;
    }
    m_advance = advance;
    if (m_running.page == Page::Compare) {
        m_compareBytes = advance.total;
    }
    updateProgress();
    updateStatus();
}

void ChecksumWindow::showCheck(quint64 id, int row, const Checksums::Check &check)
{
    if (!isCurrent(id)) {
        return;
    }
    QList<Checksums::Check> &rows = *rowsFor(m_running.page);
    if (row < 0 || row >= rows.size()) {
        return;
    }
    rows[row] = check;
    if (m_running.page == page()) {
        setRow(row, check);
    }
    if (m_running.page == Page::Compare) {
        updateCompare();
    }
    updateStatus();
}

void ChecksumWindow::fillTable()
{
    const Page shown = page();
    const QList<Checksums::Check> &rows = *rowsFor(shown);
    // One file needs no table: the Compare page says all there is about it.
    const bool needed = shown == Page::Verify || rows.size() > 1;
    m_table->setVisible(needed);
    if (!needed) {
        return;
    }

    // The whole table at once: with many files, row by row would take a while.
    m_table->setUpdatesEnabled(false);
    m_table->setRowCount(int(rows.size()));
    for (int row = 0; row < rows.size(); ++row) {
        for (int column = 0; column < 3; ++column) {
            m_table->setItem(row, column, new QTableWidgetItem);
        }
        setRow(row, rows.at(row));
    }
    m_table->setUpdatesEnabled(true);
}

void ChecksumWindow::setRow(int row, const Checksums::Check &check)
{
    const bool listed = page() == Page::Verify;
    const Checksums::Status status = shownStatus(check);
    const bool hashed = !check.actual.isEmpty();

    auto *name = m_table->item(row, FileColumn);
    name->setText(check.item.name);
    name->setToolTip(check.item.path);

    // On the Compare page a file is only compared once there is a hash to compare it with.
    const bool nothingToCompare = !listed && m_wanted.size() != Checksums::hashLength(check.item.algorithm);
    auto *state = m_table->item(row, StatusColumn);
    state->setText(nothingToCompare ? QStringLiteral("\u2014") : Checksums::statusName(status));
    state->setForeground(statusColor(status));

    // What the file has; until it was read, what is expected of it.
    auto *sum = m_table->item(row, HashColumn);
    sum->setText(hashed ? check.actual : check.item.expected);
    sum->setFont(m_monospace);
    sum->setForeground(hashed ? QApplication::palette().text().color() : mutedColor());

    QStringList lines{check.item.path};
    if (!check.item.expected.isEmpty()) {
        lines << tr("Expected: %1").arg(check.item.expected);
    }
    if (hashed) {
        lines << tr("The file has: %1").arg(check.actual);
    }
    if (!check.error.isEmpty()) {
        lines << check.error;
    }
    sum->setToolTip(lines.join(QLatin1Char('\n')));
}

Checksums::Status ChecksumWindow::shownStatus(const Checksums::Check &check) const
{
    // On the Verify page the status is what the check found. On the Compare
    // page it says whether the file has the hash in the field, which may have
    // been pasted after the file was hashed.
    if (page() == Page::Verify || check.actual.isEmpty()
        || m_wanted.size() != Checksums::hashLength(check.item.algorithm)) {
        return check.status;
    }
    return QString::compare(check.actual, m_wanted, Qt::CaseInsensitive) == 0
                   ? Checksums::Status::Ok
                   : Checksums::Status::Different;
}

void ChecksumWindow::updateCompare()
{
    if (m_current < 0 || m_current >= m_compare.size()) {
        m_current = 0;
    }
    const Checksums::Check *current = m_compare.isEmpty() ? nullptr : &m_compare.at(m_current);
    const QString hash = current ? current->actual : QString();
    // The whole hash, as a SHA-512 doesn't fit in the field.
    m_digestField->setText(hash);
    m_digestField->setToolTip(hash);
    m_copy->setEnabled(!hash.isEmpty());

    // How much there is to hash, next to the algorithm.
    if (m_compare.size() == 1) {
        const bool read = !hash.isEmpty() || waiting(Page::Compare);
        m_sizeLabel->setText(read ? humanSize(m_compareBytes) : QString());
    } else {
        m_sizeLabel->setText(m_compare.isEmpty() ? QString()
                                                 : tr("%1 files, %2").arg(m_compare.size())
                                                       .arg(humanSize(m_compareBytes)));
    }

    Verdict verdict = Verdict::Nothing;
    QColor color = mutedColor();
    QString text;
    const int needed = Checksums::hashLength(m_algorithm);
    if (!current) {
        text = tr("Choose a file, or drop one on this window.");
    } else if (waiting(Page::Compare)) {
        text = tr("Hashing\u2026");
    } else if (current->status == Checksums::Status::NotChecked) {
        text = tr("Stopped.");
    } else if (hash.isEmpty()) {
        color = warningColor();
        text = tr("The file could not be read: %1").arg(current->error);
    } else if (m_wanted.isEmpty() || (!m_settled && m_wanted.size() < needed
                                      && m_wanted == digitsOf(m_expected->text()))) {
        // Nothing to compare with yet: the field is empty, or a hash is being typed.
        text = tr("Paste the hash the file should have, to compare.");
    } else if (m_wanted.size() != needed) {
        verdict = Verdict::NotHash;
        color = warningColor();
        text = tr("That is not a %1 hash.").arg(Checksums::algorithmName(m_algorithm));
    } else {
        // With several files, the hash says which one it is.
        QStringList matching;
        for (const Checksums::Check &check : m_compare) {
            if (!check.actual.isEmpty()
                && QString::compare(check.actual, m_wanted, Qt::CaseInsensitive) == 0) {
                matching.append(check.item.name);
            }
        }
        verdict = matching.isEmpty() ? Verdict::Different : Verdict::Match;
        color = matching.isEmpty() ? badColor() : goodColor();
        if (m_compare.size() == 1) {
            text = matching.isEmpty() ? tr("Does not match") : tr("Match");
        } else {
            text = matching.isEmpty() ? tr("Does not match: none of the files has this hash.")
                                      : tr("Match: %1").arg(matching.join(QStringLiteral(", ")));
        }
    }

    m_verdictNow = verdict;
    m_verdict->setText(text);
    colorize(m_verdict, color);
    if (verdict != m_badgeFor || color != m_badgeColor) {
        // The badge is drawn anew only when what it says changes.
        m_badgeFor = verdict;
        m_badgeColor = color;
        m_verdictIcon->setPixmap(verdict == Verdict::Nothing
                                         ? QPixmap()
                                         : badge(verdict, color, m_verdictIcon->devicePixelRatioF()));
    }
}

QString ChecksumWindow::workText(Page page) const
{
    const QString verb = page == Page::Compare ? tr("Hashing") : tr("Verifying");
    if (isCurrent(m_running.id) && m_running.page == page && m_advance.count > 0 && !m_advance.current.isEmpty()) {
        return m_advance.count > 1
                       ? tr("%1 %2 of %3: %4").arg(verb).arg(m_advance.files + 1).arg(m_advance.count)
                             .arg(m_advance.current)
                       : tr("%1 %2").arg(verb).arg(m_advance.current);
    }
    return verb + QStringLiteral("\u2026");
}

void ChecksumWindow::updateStatus()
{
    if (!m_notice.isEmpty()) {
        m_status->setText(m_notice);
        colorize(m_status, mutedColor());
        return;
    }

    QString text;
    QColor color = mutedColor();
    if (page() == Page::Compare) {
        if (waiting(Page::Compare)) {
            text = workText(Page::Compare);
        }
    } else if (!m_problem.isEmpty()) {
        text = m_problem;
        color = warningColor();
    } else if (m_list.entries.isEmpty()) {
        text = tr("Choose a checksum file, or drop one on this window.");
    } else if (waiting(Page::Verify)) {
        text = workText(Page::Verify);
    } else {
        text = Checksums::summary(m_checks);
        bool unchecked = false;
        for (const Checksums::Check &check : m_checks) {
            unchecked = unchecked || check.status == Checksums::Status::NotChecked;
        }
        if (unchecked) {
            // Rows that were not looked at: the check was stopped.
            text.prepend(tr("Stopped") + QStringLiteral(". "));
        }
        if (m_list.unread > 0) {
            text += QStringLiteral(" \u00b7 ")
                    + (m_list.unread == 1 ? tr("1 line was not understood")
                                          : tr("%1 lines were not understood").arg(m_list.unread));
        }
    }
    m_status->setText(text);
    colorize(m_status, color);
}

bool ChecksumWindow::canSave() const
{
    if (isHashing()) {
        return false;
    }
    bool hashed = false;
    for (const Checksums::Check &check : *rowsFor(page())) {
        // What was not looked at has no hash yet.
        if (check.status == Checksums::Status::NotChecked) {
            return false;
        }
        hashed = hashed || !check.actual.isEmpty();
    }
    return hashed;
}

void ChecksumWindow::updateProgress()
{
    const bool busy = m_watcher != nullptr;
    m_progress->setVisible(busy);
    m_progressText->setVisible(busy);
    m_stop->setVisible(busy);
    m_verify->setEnabled(!busy && !m_list.entries.isEmpty());
    m_save->setEnabled(canSave());
    if (!busy) {
        return;
    }

    const qint64 total = m_advance.total;
    const qint64 done = qMin<qint64>(m_advance.done, total);
    const int value = total > 0 ? int(done * 1000 / total) : 1000;
    m_progress->setValue(value);
    m_progressText->setText(total > 0 ? tr("%1% \u00b7 %2 of %3").arg(value / 10).arg(humanSize(done))
                                             .arg(humanSize(total))
                                      : QString());
}

void ChecksumWindow::updateTitle()
{
    QString what;
    if (page() == Page::Verify) {
        what = m_list.path.isEmpty() ? QString() : QFileInfo(m_list.path).fileName();
    } else if (m_compare.size() == 1) {
        what = m_compare.first().item.name;
    } else if (!m_compare.isEmpty()) {
        what = tr("%1 files").arg(m_compare.size());
    }
    setWindowTitle(what.isEmpty() ? tr("Checksum") : tr("Checksum: %1").arg(what));
}

void ChecksumWindow::refresh()
{
    fillTable();
    updateCompare();
    updateStatus();
    updateProgress();
    updateTitle();
}

void ChecksumWindow::expectedChanged()
{
    const QString text = m_expected->text();
    // Pasted, or dropped in, all at once: then it is a whole hash. Typed one
    // digit at a time, it is whole only once Enter is pressed or the field is left.
    const qsizetype change = text.size() - m_expectedSize;
    m_expectedSize = text.size();
    if (qAbs(change) >= kAtOnce) {
        m_settled = true;
    } else if (change > 0) {
        m_settled = false;
    }
    m_wanted = Checksums::hashIn(text);
    if (m_settled) {
        followExpectedAlgorithm();
    }
    refresh();
}

void ChecksumWindow::expectedSettled()
{
    if (m_settled) {
        return;
    }
    m_settled = true;
    followExpectedAlgorithm();
    refresh();
}

void ChecksumWindow::followExpectedAlgorithm()
{
    // The expected hash is of another kind than the one computed: compute that
    // kind, or a pasted MD5 next to a SHA-256 could only ever be told it doesn't match.
    const std::optional<Checksums::Algorithm> kind = Checksums::algorithmForLength(int(m_wanted.size()));
    if (kind && *kind != m_algorithm) {
        setAlgorithm(*kind);
    }
}

void ChecksumWindow::notice(const QString &text)
{
    m_notice = text;
    updateStatus();
    QTimer::singleShot(kNoticeMs, this, [this] {
        m_notice.clear();
        updateStatus();
    });
}

void ChecksumWindow::copyHash()
{
    const QString hash = this->hash();
    if (hash.isEmpty()) {
        return;
    }
    QGuiApplication::clipboard()->setText(hash);
    notice(tr("Copied the %1 to the clipboard.").arg(Checksums::algorithmName(m_algorithm)));
}

void ChecksumWindow::paste()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (text.trimmed().isEmpty()) {
        // What is in the field stays.
        notice(tr("The clipboard has no hash in it. Copy one first."));
        return;
    }
    m_expected->setText(text);
    m_expected->setFocus();
}

bool ChecksumWindow::saveChecksumFile(const QString &path)
{
    // The files of the page that is shown, with the hashes that were found for
    // them, named as seen from the checksum file. One that could not be read
    // has no hash, and is left out.
    const QDir folder = QDir(QFileInfo(path).absolutePath());
    QList<Checksums::Entry> entries;
    int leftOut = 0;
    for (const Checksums::Check &check : *rowsFor(page())) {
        if (check.actual.isEmpty()) {
            ++leftOut;
            continue;
        }
        entries.append(Checksums::Entry{folder.relativeFilePath(check.item.path), check.actual,
                                        check.item.algorithm});
    }
    if (entries.isEmpty()) {
        notice(tr("Nothing to save: no file was hashed."));
        return false;
    }

    if (QFileInfo::exists(path)) {
        const FileOps::Existing answer = m_dialogs.askOverwrite(path);
        if (answer != FileOps::Existing::Overwrite && answer != FileOps::Existing::OverwriteAll) {
            return false;
        }
    }

    QString error;
    if (!Checksums::write(path, entries, &error)) {
        m_dialogs.tell(tr("Save checksum file"), tr("%1 could not be written: %2").arg(path, error));
        return false;
    }
    QString said = tr("Wrote %1 checksums to %2").arg(entries.size()).arg(QFileInfo(path).fileName());
    if (leftOut > 0) {
        said += tr("; %1 that could not be read %2 left out.")
                        .arg(leftOut)
                        .arg(leftOut == 1 ? tr("was") : tr("were"));
    }
    notice(said);
    return true;
}

void ChecksumWindow::browseForTarget()
{
    const QList<Checksums::Check> &rows = *rowsFor(page());
    if (rows.isEmpty()) {
        return;
    }
    const QString suffix = Checksums::fileSuffix(rows.first().item.algorithm);
    QString folder = QFileInfo(rows.first().item.path).absolutePath();
    QString name = rows.first().item.name + QLatin1Char('.') + suffix;
    if (page() == Page::Verify || rows.size() > 1) {
        // Named after the folder, not after the checksum file that was checked:
        // that one is the reference, and is not replaced by accident.
        folder = page() == Page::Verify && !m_folder.isEmpty() ? m_folder : folder;
        name = QDir(folder).dirName() + QLatin1Char('.') + suffix;
        if (QDir(folder).dirName().isEmpty()) {
            name = QStringLiteral("checksums.") + suffix;
        }
    }
    const QString path = QFileDialog::getSaveFileName(
            this, tr("Save checksum file"), folder + QLatin1Char('/') + name,
            tr("%1 files (*.%2);;All files (*)").arg(Checksums::algorithmName(rows.first().item.algorithm), suffix));
    if (!path.isEmpty()) {
        saveChecksumFile(path);
    }
}

void ChecksumWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QDialog::dragEnterEvent(event);
}

void ChecksumWindow::dropEvent(QDropEvent *event)
{
    const QStringList paths = localFiles(*event->mimeData());
    if (paths.isEmpty()) {
        event->ignore();
        notice(tr("These are not files. Drop a file to hash it, or a checksum file to check it."));
        return;
    }
    event->acceptProposedAction();
    // A checksum file among them is checked against the files around it.
    for (const QString &path : paths) {
        if (Checksums::isChecksumFile(path)) {
            takeFile(path);
            return;
        }
    }
    setFiles(paths);
}

void ChecksumWindow::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    // The colours are chosen for the theme in use, which may just have changed.
    if ((event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange)
        && m_table) {
        refresh();
    }
}
