#pragma once

#include <QCollator>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>

struct FileEntry;
class FileModel;

// Sits between a FileModel and the view: decides what is shown and in which
// order. Folders always come first; names sort in natural order, so "file2"
// comes before "file10".
class FileProxyModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit FileProxyModel(QObject *parent = nullptr);

    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);

    // Words the names must contain; "*" and "?" act as wildcards.
    QString filterText() const { return m_filterText; }
    void setFilterText(const QString &text);

    // The entry behind a proxy index, or nullptr.
    const FileEntry *entry(const QModelIndex &index) const;

    // The folder whose contents are listed; empty before the first listing.
    QString folder() const;
    // Whether that folder is being read.
    bool isLoading() const;
    // Why the last listing failed; empty when it didn't.
    QString error() const;

    // The row of the item at `path`, in the Name column; invalid if there is none.
    QModelIndex indexForPath(const QString &path) const;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    FileModel *fileModel() const;
    // Re-runs the filter over the source rows.
    void refilter();

    bool m_showHidden = false;
    QString m_filterText;
    QList<QRegularExpression> m_terms;
    QCollator m_collator;
};
