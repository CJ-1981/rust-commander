#include "FileModel.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPalette>
#include <QThreadPool>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("FileModel", text);
}

}  // namespace

QString humanSize(qint64 bytes)
{
    return QLocale::system().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

FileModel::FileModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    m_pool.setMaxThreadCount(2);
}

FileModel::~FileModel()
{
    // The thread pool is destroyed after this and waits for its workers.
    *m_cancel = true;
}

const FileEntry *FileModel::entry(int row) const
{
    return row >= 0 && row < m_entries.size() ? &m_entries.at(row) : nullptr;
}

int FileModel::rowOf(const QString &path) const
{
    for (int row = 0; row < m_entries.size(); ++row) {
        if (m_entries.at(row).path == path) {
            return row;
        }
    }
    return -1;
}

void FileModel::setPath(const QString &path)
{
    load(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

void FileModel::reload()
{
    if (!m_path.isEmpty()) {
        load(m_path);
    }
}

void FileModel::load(const QString &path)
{
    m_path = path;
    m_error.clear();
    m_loading = true;
    const quint64 generation = ++m_generation;

    beginResetModel();
    m_entries.clear();
    m_folderSizes.clear();
    m_sizeRequests.clear();
    endResetModel();

    auto *watcher = new QFutureWatcher<Listing>(this);
    connect(watcher, &QFutureWatcher<Listing>::finished, this,
            [this, watcher, generation] {
                watcher->deleteLater();
                // A newer request superseded this one: drop the stale result.
                if (generation != m_generation) {
                    return;
                }
                const Listing listing = watcher->result();
                m_loading = false;
                m_error = listing.error;
                beginResetModel();
                m_entries = listing.entries;
                endResetModel();
                if (m_error.isEmpty()) {
                    emit listingFinished(m_path);
                } else {
                    emit listingFailed(m_path, m_error);
                }
            });
    watcher->setFuture(QtConcurrent::run(&m_pool, [path, cancel = m_cancel]() -> FileModel::Listing {
        FileModel::Listing result;
        result.path = path;

        const QFileInfo folder(path);
        if (!folder.exists() || !folder.isDir()) {
            result.error = tr("This is not a folder, or it no longer exists.");
            return result;
        }
        if (!folder.isReadable()) {
            result.error = tr("You don't have permission to open this folder.");
            return result;
        }

        QDir dir(path);
        dir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        dir.setSorting(QDir::NoSort);

        QMimeDatabase mimeDb;
        for (const QFileInfo &info : dir.entryInfoList()) {
            FileEntry entry;
            // Ask first: on macOS, isSymLink() makes an entry that came from a
            // directory listing report that it is not hidden (Qt 6.10).
            entry.isHidden = info.isHidden();
            entry.name = info.fileName();
            entry.path = info.absoluteFilePath();
            entry.isDir = info.isDir();
            entry.size = entry.isDir ? 0 : info.size();
            entry.modified = info.lastModified();
            entry.suffix = info.suffix().toLower();
            entry.isSymLink = info.isSymLink();
            if (entry.isDir) {
                entry.type = tr("Folder");
            } else {
                entry.type = mimeDb.mimeTypeForFile(info).comment();
                if (entry.type.isEmpty()) {
                    entry.type = entry.suffix.isEmpty() ? tr("File")
                                                        : entry.suffix.toUpper() + ' ' + tr("file");
                }
            }
            result.entries.append(entry);
            if (result.entries.size() % 256 == 0 && cancel->load(std::memory_order_relaxed)) {
                return result;
            }
        }

        // A stable order, so that the listing doesn't jump before it is sorted
        // by the view's criteria.
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(result.entries.begin(), result.entries.end(),
                  [&collator](const FileEntry &a, const FileEntry &b) {
                      if (a.isDir != b.isDir) {
                          return a.isDir;
                      }
                      return collator.compare(a.name, b.name) < 0;
                  });
        return result;
    }));
    emit listingStarted(path);
}

void FileModel::requestFolderSizes()
{
    QStringList pending;
    for (const FileEntry &entry : m_entries) {
        if (entry.isDir && !m_folderSizes.contains(entry.path)
            && !m_sizeRequests.contains(entry.path)) {
            pending.append(entry.path);
        }
    }
    for (const QString &path : pending) {
        m_sizeRequests.insert(path);
        auto *watcher = new QFutureWatcher<FolderSize>(this);
        connect(watcher, &QFutureWatcher<FolderSize>::finished, this,
                [this, watcher] {
                    watcher->deleteLater();
                    const FolderSize result = watcher->result();
                    m_sizeRequests.remove(result.path);
                    // A cancelled measurement is incomplete: keep it out of the cache.
                    if (!m_cancel->load(std::memory_order_relaxed)) {
                        setFolderSize(result.path, result.bytes, result.items);
                    }
                });
        watcher->setFuture(QtConcurrent::run(&m_pool, [path, cancel = m_cancel]() -> FileModel::FolderSize {
            // Symlinked folders are not followed, so a link loop can't run forever.
            QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                            QDirIterator::Subdirectories);
            FileModel::FolderSize total;
            total.path = path;
            while (it.hasNext()) {
                if (cancel->load(std::memory_order_relaxed)) {
                    return total;
                }
                it.next();
                const QFileInfo info = it.fileInfo();
                if (!info.isSymLink()) {
                    total.bytes += info.size();
                }
                ++total.items;
            }
            return total;
        }));
    }
}

void FileModel::setFolderSize(const QString &path, qint64 bytes, qint64 items)
{
    m_folderSizes.insert(path, qMakePair(bytes, items));
    const int row = rowOf(path);
    if (row >= 0) {
        emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
    }
}

qint64 FileModel::folderSizeOf(const FileEntry &entry) const
{
    const auto it = m_folderSizes.constFind(entry.path);
    return it == m_folderSizes.end() ? -1 : it->first;
}

QIcon FileModel::iconFor(const FileEntry &entry) const
{
    if (entry.isDir) {
        return m_icons.icon(QFileIconProvider::Folder);
    }
    // One icon per file type, however many files there are.
    const QString key = entry.suffix.isEmpty() ? QStringLiteral("<none>") : entry.suffix;
    const auto it = m_fileIcons.constFind(key);
    if (it != m_fileIcons.constEnd()) {
        return it.value();
    }
    const QIcon icon = m_icons.icon(QFileInfo(entry.path));
    m_fileIcons.insert(key, icon);
    return icon;
}

Qt::ItemFlags FileModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    const FileEntry *entry = this->entry(index.row());
    if (!entry) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (entry->isDir) {
        result |= Qt::ItemIsDropEnabled;
    }
    return result;
}

int FileModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

int FileModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant FileModel::data(const QModelIndex &index, int role) const
{
    const FileEntry *entry = index.isValid() ? this->entry(index.row()) : nullptr;
    if (!entry || index.column() >= ColumnCount) {
        return {};
    }

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case NameColumn:
            return entry->name;
        case SizeColumn:
            if (entry->isDir) {
                const qint64 bytes = folderSizeOf(*entry);
                return bytes < 0 ? QString() : humanSize(bytes);
            }
            return humanSize(entry->size);
        case TypeColumn:
            return entry->type;
        case ModifiedColumn:
            return entry->modified;
        default:
            return {};
        }
    case Qt::ToolTipRole: {
        QString tip = entry->path;
        if (entry->isSymLink) {
            const QString target = QFileInfo(entry->path).symLinkTarget();
            if (!target.isEmpty()) {
                tip += QStringLiteral("\n") + tr("Symbolic link to %1").arg(target);
            }
        }
        if (entry->isDir) {
            const auto it = m_folderSizes.constFind(entry->path);
            if (it != m_folderSizes.constEnd()) {
                tip += QStringLiteral("\n") + tr("%1 in %2 items").arg(humanSize(it->first)).arg(it->second);
            }
        }
        return tip;
    }
    case Qt::DecorationRole:
        return index.column() == NameColumn ? QVariant(iconFor(*entry)) : QVariant();
    case Qt::TextAlignmentRole:
        return index.column() == SizeColumn ? QVariant(Qt::AlignRight | Qt::AlignVCenter) : QVariant();
    case Qt::ForegroundRole:
        if (entry->isHidden) {
            return QGuiApplication::palette().placeholderText().color();
        }
        return {};
    case PathRole:
        return entry->path;
    case IsDirRole:
        return entry->isDir;
    case IsSymLinkRole:
        return entry->isSymLink;
    case FolderSizeRole:
        return entry->isDir ? QVariant(folderSizeOf(*entry)) : QVariant();
    case SortRole:
        switch (index.column()) {
        case NameColumn:
            return entry->name;
        case SizeColumn:
            return entry->isDir ? QVariant(qMax<qint64>(0, folderSizeOf(*entry))) : QVariant(entry->size);
        case TypeColumn:
            return entry->type;
        case ModifiedColumn:
            return entry->modified;
        default:
            return {};
        }
    default:
        return {};
    }
}

QVariant FileModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section >= ColumnCount) {
        return {};
    }
    switch (section) {
    case NameColumn:
        return tr("Name");
    case SizeColumn:
        return tr("Size");
    case TypeColumn:
        return tr("Type");
    case ModifiedColumn:
        return tr("Modified");
    default:
        return {};
    }
}

QStringList FileModel::mimeTypes() const
{
    return {QStringLiteral("text/uri-list")};
}

QMimeData *FileModel::mimeData(const QModelIndexList &indexes) const
{
    // The selection has an index per column; take each row once.
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    QSet<int> rows;
    for (const QModelIndex &index : indexes) {
        if (index.isValid() && !rows.contains(index.row())) {
            rows.insert(index.row());
            if (const FileEntry *entry = this->entry(index.row())) {
                urls.append(QUrl::fromLocalFile(entry->path));
            }
        }
    }
    mime->setUrls(urls);
    return mime;
}
