#include "PropertiesWindow.h"

#include "panels/FileModel.h"

#include <QDateTime>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

namespace {

// How big a folder is: every file below it, links not followed, as the
// folder sizes of the listing are measured.
qint64 folderBytes(const QString &path, int *items)
{
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    qint64 bytes = 0;
    while (it.hasNext()) {
        it.next();
        if (!it.fileInfo().isSymLink()) {
            bytes += it.fileInfo().size();
        }
        ++*items;
    }
    return bytes;
}

// What a file is called in the Type column.
QString kindOf(const QFileInfo &info)
{
    if (info.isDir()) {
        return PropertiesWindow::tr("Folder");
    }
    const QString suffix = info.suffix();
    return suffix.isEmpty() ? PropertiesWindow::tr("File")
                            : PropertiesWindow::tr("%1 file").arg(suffix.toUpper());
}

QString when(const QDateTime &moment)
{
    return QLocale().toString(moment, QLocale::ShortFormat);
}

}  // namespace

PropertiesWindow::PropertiesWindow(const QStringList &paths, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Properties"));
    setModal(false);
    buildFor(paths);
}

void PropertiesWindow::buildFor(const QStringList &paths)
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto *head = new QHBoxLayout;
    m_icon = new QLabel;
    m_icon->setAlignment(Qt::AlignCenter);
    head->addWidget(m_icon);

    auto *headText = new QVBoxLayout;
    m_title = new QLabel;
    m_title->setFont(QFontDatabase::systemFont(QFontDatabase::TitleFont));
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headText->addWidget(m_title);
    m_details = new QLabel;
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headText->addWidget(m_details);
    head->addLayout(headText, 1);
    layout->addLayout(head);

    m_size = new QLabel;
    m_size->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_size);

    auto *close = new QPushButton(tr("&Close"));
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    if (paths.isEmpty()) {
        m_title->setText(tr("Nothing selected"));
        return;
    }

    if (paths.size() == 1) {
        const QFileInfo info(paths.first());
        QFileIconProvider icons;
        m_icon->setPixmap(icons.icon(info).pixmap(48, 48));
        m_title->setText(info.fileName());

        QStringList lines;
        lines << kindOf(info);
        lines << QDir::toNativeSeparators(info.absoluteFilePath());
        if (info.isSymLink()) {
            lines << tr("A link to %1").arg(QDir::toNativeSeparators(info.symLinkTarget()));
        }
        lines << tr("Created %1, changed %2, read %3")
                     .arg(when(info.birthTime()), when(info.lastModified()),
                          when(info.lastRead()));
        if (info.isHidden()) {
            lines << tr("Hidden");
        }
        m_details->setText(lines.join(QLatin1Char('\n')));

        if (info.isDir()) {
            m_size->setText(tr("Finding out how big it is\u2026"));
            measureFolder(info.absoluteFilePath());
        } else {
            m_size->setText(tr("%1 (%2 bytes)").arg(humanSize(info.size())).arg(info.size()));
        }
        return;
    }

    // Several at once: counted here, and measured together on a worker.
    QFileIconProvider icons;
    int folders = 0;
    qint64 bytes = 0;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            ++folders;
            m_folders.append(info.absoluteFilePath());
        } else {
            bytes += info.size();
        }
    }
    m_icon->setPixmap(icons.icon(QFileIconProvider::Folder).pixmap(48, 48));
    m_title->setText(tr("%1 items").arg(paths.size()));
    m_details->setText(tr("%1 folders and %2 files").arg(folders).arg(paths.size() - folders));
    m_size->setText(tr("Finding out how big it is\u2026"));

    auto *watcher = new QFutureWatcher<qint64>(this);
    connect(watcher, &QFutureWatcher<qint64>::finished, this, [this, watcher, bytes] {
        watcher->deleteLater();
        const qint64 total = bytes + watcher->result();
        m_size->setText(tr("%1 (%2 bytes)").arg(humanSize(total)).arg(total));
    });
    watcher->setFuture(QtConcurrent::run([folders = m_folders]() -> qint64 {
        qint64 total = 0;
        for (const QString &folder : folders) {
            int items = 0;
            total += folderBytes(folder, &items);
        }
        return total;
    }));
}

void PropertiesWindow::measureFolder(const QString &path)
{
    auto *watcher = new QFutureWatcher<QPair<qint64, int>>(this);
    connect(watcher, &QFutureWatcher<QPair<qint64, int>>::finished, this, [this, watcher] {
        watcher->deleteLater();
        const auto total = watcher->result();
        m_size->setText(tr("%1 (%2 bytes, %3 items)")
                            .arg(humanSize(total.first))
                            .arg(total.first)
                            .arg(total.second));
    });
    watcher->setFuture(QtConcurrent::run([path]() -> QPair<qint64, int> {
        int items = 0;
        const qint64 bytes = folderBytes(path, &items);
        return {bytes, items};
    }));
}
