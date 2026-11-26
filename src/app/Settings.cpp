#include "Settings.h"

#include <QDir>
#include <QMainWindow>

void Settings::storeIn(const QString &folder)
{
    QDir().mkpath(folder);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, folder);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, folder);
}

QSettings &Settings::values()
{
    static QSettings settings;
    return settings;
}

Theme::Mode Settings::themeMode()
{
    return Theme::modeFromName(values().value("appearance/theme").toString(),
                               Theme::Mode::System);
}

void Settings::setThemeMode(Theme::Mode mode)
{
    values().setValue("appearance/theme", Theme::modeName(mode));
}

bool Settings::showHidden()
{
    return values().value("files/showHidden", false).toBool();
}

void Settings::setShowHidden(bool show)
{
    values().setValue("files/showHidden", show);
}

QStringList Settings::tabPaths(int pane)
{
    return values().value(QString("panes/%1/tabs").arg(pane)).toStringList();
}

void Settings::setTabPaths(int pane, const QStringList &paths)
{
    values().setValue(QString("panes/%1/tabs").arg(pane), paths);
}

QVector<Settings::Bookmark> Settings::bookmarks()
{
    QSettings &s = values();
    s.beginGroup("bookmarks");
    QVector<Bookmark> result;
    const int count = s.beginReadArray("items");
    for (int i = 0; i < count; ++i) {
        s.setArrayIndex(i);
        result.append(Bookmark {s.value("name").toString(), s.value("path").toString()});
    }
    s.endArray();
    s.endGroup();
    return result;
}

void Settings::setBookmarks(const QVector<Bookmark> &bookmarks)
{
    QSettings &s = values();
    s.beginGroup("bookmarks");
    s.remove("items");
    s.beginWriteArray("items", bookmarks.size());
    for (int i = 0; i < bookmarks.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("name", bookmarks[i].name);
        s.setValue("path", bookmarks[i].path);
    }
    s.endArray();
    s.endGroup();
}

void Settings::saveLayout(QMainWindow *window, const QByteArray &splitterState)
{
    QSettings &s = values();
    s.setValue("layout/window", window->saveGeometry());
    s.setValue("layout/divider", splitterState);
}

QByteArray Settings::restoreLayout(QMainWindow *window)
{
    QSettings &s = values();
    window->restoreGeometry(s.value("layout/window").toByteArray());
    return s.value("layout/divider").toByteArray();
}
