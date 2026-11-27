#pragma once

#include <QAbstractTableModel>
#include <QDateTime>
#include <QFileIconProvider>
#include <QHash>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <memory>

// One file or folder shown in a pane.
struct FileEntry {
    QString name;
    QString path;      // absolute
    QString type;      // what the Type column shows, e.g. "PNG image"
    QString suffix;    // lower-case, files only
    qint64 size = 0;   // files only; a folder's size is computed on request
    QDateTime modified;
    bool isDir = false;
    bool isSymLink = false;
    bool isHidden = false;
};

// Human-readable size, e.g. "1.4 MB".
QString humanSize(qint64 bytes);

// Extra mime format on drags that start in this app. A drop can tell them
// from files dragged in from another app; the data is the folder the items
// were dragged from.
inline constexpr auto dragSourceFormat = "application/x-rust-commander-source";

// The contents of one folder, as a table (name, size, type, modified).
// The folder is read on a worker thread; the model is empty until the listing
// arrives.
class FileModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        NameColumn = 0,
        SizeColumn,
        TypeColumn,
        ModifiedColumn,
        ColumnCount,
    };
    enum Role {
        PathRole = Qt::UserRole + 1,
        IsDirRole,
        IsSymLinkRole,
        FolderSizeRole,  // bytes, for folders whose size has been computed
        SortRole,
    };

    explicit FileModel(QObject *parent = nullptr);
    ~FileModel() override;

    QString path() const { return m_path; }
    // The folder whose entries are in the model. Not `path()` while the
    // listing of another folder is on its way: the entries are then still
    // those of the folder before.
    QString listedPath() const { return m_listedPath; }
    bool isLoading() const { return m_loading; }
    // Why the last listing failed; empty when it didn't.
    QString error() const { return m_error; }

    const FileEntry *entry(int row) const;
    // Row of the entry at `path`, or -1.
    int rowOf(const QString &path) const;

    void setPath(const QString &path);
    void reload();

    // Folder sizes, as "size · N items", for every folder in the listing that
    // has none yet. Computed on a worker thread.
    void requestFolderSizes();

    Qt::ItemFlags flags(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    QStringList mimeTypes() const override;
    Qt::DropActions supportedDropActions() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;

signals:
    void listingStarted(const QString &path);
    void listingFinished(const QString &path);
    void listingFailed(const QString &path, const QString &error);

private:
    struct Listing {
        QString path;
        QList<FileEntry> entries;
        QString error;
    };
    struct FolderSize {
        QString path;
        qint64 bytes = 0;
        qint64 items = 0;
    };

    void load(const QString &path);
    void setFolderSize(const QString &path, qint64 bytes, qint64 items);
    QIcon iconFor(const FileEntry &entry) const;
    qint64 folderSizeOf(const FileEntry &entry) const;

    QThreadPool m_pool;
    // Apart from the listings': measuring folder sizes can take minutes on a
    // slow disk, and a listing must not wait its turn behind that.
    QThreadPool m_sizePool;
    // Set while the model is destroyed, so that the worker threads stop
    // measuring instead of holding up the destructor.
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
    // Identifies the newest listing request; results of older ones are dropped.
    quint64 m_generation = 0;

    QString m_path;
    QString m_listedPath;
    QList<FileEntry> m_entries;
    QString m_error;
    bool m_loading = false;

    QHash<QString, QPair<qint64, qint64>> m_folderSizes;  // path -> (bytes, items)
    QSet<QString> m_sizeRequests;                          // folders being measured
    mutable QFileIconProvider m_icons;
    mutable QHash<QString, QIcon> m_fileIcons;             // by suffix
};
