#pragma once

// Shared by the tests: files in a temporary folder, and waiting for listings.

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QStringList>
#include <QTest>

#include <optional>

#include "app/Dialogs.h"
#include "panels/FileModel.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

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

// Reads a whole file as text; empty when it can't be read.
inline QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

// The questions the app asks, answered from a script; remembers what was asked.
class ScriptedDialogs : public Dialogs {
public:
    std::optional<QString> askText(const QString &, const QString &, const QString &value) override
    {
        ++textsAsked;
        offered = value;
        if (echoOffered) {
            return value;
        }
        return text;
    }
    Answer ask(const QString &, const QString &question) override
    {
        questions.append(question);
        return answer;
    }
    void tell(const QString &title, const QString &message) override
    {
        told.append(title + ": " + message);
    }
    FileOps::Existing askOverwrite(const QString &target) override
    {
        targets.append(target);
        return overwrite.isEmpty() ? FileOps::Existing::Cancel : overwrite.takeFirst();
    }

    std::optional<QString> text = std::nullopt;  // the answer to a text question
    bool echoOffered = false;                    // ... or the name that was offered
    Answer answer = Answer::Yes;
    QList<FileOps::Existing> overwrite;
    QString offered;
    QStringList questions;
    QStringList told;
    QStringList targets;
    int textsAsked = 0;
};
