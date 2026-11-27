#pragma once

#include <QDialog>

class QComboBox;
class QPlainTextEdit;

// "About Rust Commander": what it is, with its version, and the licenses of
// the third-party software it is made with.
class AboutDialog : public QDialog {
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = nullptr);

    // The third-party software, in the order it is offered.
    QComboBox *licenseChooser() const { return m_chooser; }
    // The license text that is shown.
    QString licenseText() const;

private:
    // Shows the license of the software picked in the list.
    void showLicense(int index);

    QComboBox *m_chooser;
    QPlainTextEdit *m_text;
};
