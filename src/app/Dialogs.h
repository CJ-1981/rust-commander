#pragma once

#include <QString>

#include "ops/FileOps.h"

class QWidget;

// The questions the app asks the user. The app shows window dialogs
// (GuiDialogs); tests answer from a script.
class Dialogs {
public:
    virtual ~Dialogs() = default;

    enum class Answer {
        Yes,
        No,
        Cancel,
    };

    // A text the user enters; empty when cancelled.
    virtual QString askText(const QString &title, const QString &label, const QString &value = {}) = 0;
    // A question with Yes / No / Cancel.
    virtual Answer ask(const QString &title, const QString &text) = 0;
    // Something to read, with an OK.
    virtual void tell(const QString &title, const QString &text) = 0;
    // What to do with a target that exists already.
    virtual FileOps::Existing askOverwrite(const QString &target) = 0;
};

// Window dialogs over `parent`.
class GuiDialogs : public Dialogs {
public:
    explicit GuiDialogs(QWidget *parent)
        : m_parent(parent)
    {
    }

    QString askText(const QString &title, const QString &label, const QString &value = {}) override;
    Answer ask(const QString &title, const QString &text) override;
    void tell(const QString &title, const QString &text) override;
    FileOps::Existing askOverwrite(const QString &target) override;

private:
    QWidget *m_parent;
};
