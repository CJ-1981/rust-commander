#pragma once

#include <QMainWindow>

class QSplitter;

// The application window: two file panes side by side.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QSplitter *m_splitter = nullptr;
};
