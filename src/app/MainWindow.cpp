#include "MainWindow.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QLabel>
#include <QSplitter>

#include "Settings.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QCoreApplication::applicationName());
    resize(1180, 700);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->addWidget(new QLabel(tr("Left pane"), m_splitter));
    m_splitter->addWidget(new QLabel(tr("Right pane"), m_splitter));
    setCentralWidget(m_splitter);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    Settings::saveLayout(this, m_splitter->saveState());
    event->accept();
}
