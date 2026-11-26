#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QGuiApplication>
#include <QSettings>
#include <QStyleHints>
#include <QThread>

#include "app/MainWindow.h"
#include "app/Settings.h"
#include "app/Theme.h"

namespace {

// Draws the window to `png` without showing it on screen and exits. Used to
// check the look of the app from scripts and tests.
int renderTo(const QString &renderTarget, Theme::Mode mode)
{
    Theme::apply(mode);
    MainWindow window;
    window.resize(1180, 660);
    window.show();
    // Let layouts settle and first listings load.
    for (int i = 0; i < 25; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const bool saved = window.grab().save(renderTarget);
    window.close();
    return saved ? 0 : 1;
}

}  // namespace

int main(int argc, char *argv[])
{
    // "--render <png> [--theme light|dark] [--path <folder>]" renders the
    // window off screen instead of starting the app.
    QString renderTarget;
    QString renderTheme;
    QString startPath;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--render" && i + 1 < argc) {
            renderTarget = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--theme" && i + 1 < argc) {
            renderTheme = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--path" && i + 1 < argc) {
            startPath = QString::fromLocal8Bit(argv[++i]);
        }
    }
    if (!renderTarget.isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("CJ-1981");
    QCoreApplication::setOrganizationDomain("io.github.CJ-1981");
    QCoreApplication::setApplicationName("Rust Commander");
    QCoreApplication::setApplicationVersion(APP_VERSION);

    if (!renderTarget.isEmpty()) {
        // Rendering must not touch the user's real settings.
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           QDir::temp().filePath("rust-commander-render"));
        return renderTo(renderTarget,
                        Theme::modeFromName(renderTheme, Theme::Mode::System));
    }

    Theme::apply(Settings::themeMode());
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                     &app, [](Qt::ColorScheme) { Theme::refreshSystem(); });

    MainWindow window;
    window.show();
    return app.exec();
}
