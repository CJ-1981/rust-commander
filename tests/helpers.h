#pragma once

// Shared by the tests: files in a temporary folder, and waiting for listings.

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QStringList>
#include <QTest>

#include "panels/FileModel.h"

inline bool writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

inline bool makeFolder(const QString &path)
{
    return QDir().mkpath(path);
}

inline void setModified(const QString &path, const QDateTime &when)
{
    QFile file(path);
    if (file.open(QIODevice::ReadWrite)) {
        file.setFileTime(when, QFileDevice::FileModificationTime);
    }
}

// Unix hides names that start with a dot; Windows needs the attribute.
inline void setHiddenAttribute(const QString &path)
{
#ifdef Q_OS_WIN
    const std::wstring native = path.toStdWString();
    SetFileAttributesW(native.c_str(), FILE_ATTRIBUTE_HIDDEN);
#else
    Q_UNUSED(path);
#endif
}

// Waits until the listing of `model` has arrived; false on timeout.
inline bool waitListed(FileModel &model)
{
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 15000;
    while (model.isLoading() && QDateTime::currentMSecsSinceEpoch() < deadline) {
        QTest::qWait(10);
    }
    return !model.isLoading();
}

// Names in row order. macOS may hand names back in a different Unicode form
// than they were created with, so they are normalised before comparing.
inline QStringList names(const QAbstractItemModel &model)
{
    QStringList result;
    for (int row = 0; row < model.rowCount(); ++row) {
        result.append(model.index(row, FileModel::NameColumn).data().toString()
                          .normalized(QString::NormalizationForm_C));
    }
    return result;
}

inline int rowOfName(const QAbstractItemModel &model, const QString &name)
{
    return names(model).indexOf(name);
}
