#include "FileProxyModel.h"

#include "FileModel.h"

FileProxyModel::FileProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    sort(FileModel::NameColumn, Qt::AscendingOrder);
}

FileModel *FileProxyModel::fileModel() const
{
    return qobject_cast<FileModel *>(sourceModel());
}

void FileProxyModel::setShowHidden(bool show)
{
    if (m_showHidden == show) {
        return;
    }
    m_showHidden = show;
    refilter();
}

void FileProxyModel::refilter()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    endFilterChange();
#else
    invalidateFilter();
#endif
}

void FileProxyModel::setFilterText(const QString &text)
{
    if (m_filterText == text) {
        return;
    }
    m_filterText = text;
    m_terms.clear();
    const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &word : words) {
        m_terms.append(QRegularExpression::fromWildcard(QLatin1Char('*') + word + QLatin1Char('*'),
                                                        Qt::CaseInsensitive));
    }
    refilter();
}

const FileEntry *FileProxyModel::entry(const QModelIndex &index) const
{
    const FileModel *model = fileModel();
    return model && index.isValid() ? model->entry(mapToSource(index).row()) : nullptr;
}

bool FileProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &) const
{
    const FileModel *model = fileModel();
    const FileEntry *item = model ? model->entry(sourceRow) : nullptr;
    if (!item) {
        return false;
    }
    if (!m_showHidden && item->isHidden) {
        return false;
    }
    for (const QRegularExpression &term : m_terms) {
        if (!term.match(item->name).hasMatch()) {
            return false;
        }
    }
    return true;
}

bool FileProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const FileModel *model = fileModel();
    const FileEntry *a = model ? model->entry(left.row()) : nullptr;
    const FileEntry *b = model ? model->entry(right.row()) : nullptr;
    if (!a || !b) {
        return false;
    }

    // Everything below answers "does `a` come before `b` when ascending". The
    // proxy swaps its two arguments for a descending sort, which would also
    // swap the folders to the bottom, so that one rule flips with the order.
    if (a->isDir != b->isDir) {
        return a->isDir == (sortOrder() == Qt::AscendingOrder);
    }

    int order = 0;
    switch (left.column()) {
    case FileModel::SizeColumn: {
        const qint64 sizeA = left.data(FileModel::SortRole).toLongLong();
        const qint64 sizeB = right.data(FileModel::SortRole).toLongLong();
        order = sizeA < sizeB ? -1 : sizeA > sizeB ? 1 : 0;
        break;
    }
    case FileModel::TypeColumn:
        order = QString::compare(a->type, b->type, Qt::CaseInsensitive);
        break;
    case FileModel::ModifiedColumn:
        if (a->modified.isValid() != b->modified.isValid()) {
            // Entries without a date go first.
            return !a->modified.isValid();
        }
        order = a->modified < b->modified ? -1 : a->modified > b->modified ? 1 : 0;
        break;
    default:
        break;
    }
    if (order == 0) {
        order = m_collator.compare(a->name, b->name);
    }
    return order < 0;
}
