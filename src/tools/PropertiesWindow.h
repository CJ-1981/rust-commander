#pragma once

#include <QDialog>
#include <QLabel>
#include <QList>
#include <QString>


// What is there to know about the selection: its icon, its name, how big it
// is, when it was made and last touched. One item is described by itself;
// several are counted and measured together. Folder sizes are computed on a
// worker thread, so that a big folder cannot hold the dialog up.
class PropertiesWindow : public QDialog {
    Q_OBJECT

public:
    // One or more paths, as the selection gives them.
    explicit PropertiesWindow(const QStringList &paths, QWidget *parent = nullptr);

    // What the three lines say, to read in tests.
    QString titleText() const { return m_title->text(); }
    QString detailsText() const { return m_details->text(); }
    QString sizeText() const { return m_size->text(); }

private:
    void buildFor(const QStringList &paths);
    // The size of one folder, looked for off the interface thread.
    void measureFolder(const QString &path);

    QLabel *m_icon;
    QLabel *m_title;
    QLabel *m_details;
    QLabel *m_size;
    QList<QString> m_folders;  // folders of a several-item selection, to measure
};
