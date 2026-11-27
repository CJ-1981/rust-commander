#include "AboutDialog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QIODevice>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

struct Component {
    QString name;
    QString license;  // the name of a file in resources/licenses/
};

QString tr(const char *text)
{
    return QCoreApplication::translate("AboutDialog", text);
}

// A license text that is compiled into the app, under resources/licenses/.
QString readLicense(const QString &name)
{
    QFile file(":/licenses/" + name);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

// What the app is made with, and under which license it comes. The LGPLv3
// refers to the GPLv3 for its terms, so that text is here as well.
QList<Component> components()
{
    const QString qt = tr("Qt %1").arg(QLatin1String(qVersion()));
    return {
        {tr("%1: GNU Lesser General Public License, version 3").arg(qt), "qt-lgpl-3.0.txt"},
        {tr("%1: GNU General Public License, version 3").arg(qt), "qt-gpl-3.0.txt"},
        {tr("Lucide icons: ISC license, and the MIT license for the icons from Feather"),
         "lucide.txt"},
    };
}

}  // namespace

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("About Rust Commander"));
    resize(600, 580);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 16);
    layout->setSpacing(10);

    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.35);
    auto *name = new QLabel(tr("<b>Rust Commander %1</b>").arg(QLatin1String(APP_VERSION)));
    name->setFont(titleFont);
    layout->addWidget(name);
    layout->addWidget(new QLabel(tr("A file manager with two panes, after Total Commander.")));
    layout->addWidget(new QLabel(tr("Made with Qt %1, which is free software under the GNU LGPL.")
                                      .arg(QLatin1String(qVersion()))));

    layout->addStretch(1);
    layout->addWidget(new QLabel(tr("The third-party software it is made with, and its licenses:")));
    m_chooser = new QComboBox;
    for (const Component &component : components()) {
        m_chooser->addItem(component.name, component.license);
    }
    layout->addWidget(m_chooser);

    // The texts are laid out in lines of their own, and read best as they are.
    m_text = new QPlainTextEdit;
    m_text->setObjectName("licenseText");
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_text->setReadOnly(true);
    m_text->setMinimumHeight(260);
    layout->addWidget(m_text, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_chooser, &QComboBox::currentIndexChanged, this, &AboutDialog::showLicense);
    showLicense(0);
}

void AboutDialog::showLicense(int index)
{
    m_text->setPlainText(readLicense(m_chooser->itemData(index).toString()));
}

QString AboutDialog::licenseText() const
{
    return m_text->toPlainText();
}
