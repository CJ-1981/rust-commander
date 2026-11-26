#pragma once

#include <QByteArray>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

#include "Theme.h"

class QMainWindow;

// Everything the app remembers between launches.
class Settings {
public:
    struct Bookmark {
        QString name;
        QString path;
    };

    // The store itself. Tests point it at a scratch location.
    static QSettings &values();

    static Theme::Mode themeMode();
    static void setThemeMode(Theme::Mode mode);

    static bool showHidden();
    static void setShowHidden(bool show);

    // The folders open in each pane's tabs, left to right.
    static QStringList tabPaths(int pane);
    static void setTabPaths(int pane, const QStringList &paths);

    static QVector<Bookmark> bookmarks();
    static void setBookmarks(const QVector<Bookmark> &bookmarks);

    // Window size, position and the position of the divider between the panes.
    static void saveLayout(QMainWindow *window, const QByteArray &splitterState);
    // Restores the window; returns the saved divider state, or an empty array.
    static QByteArray restoreLayout(QMainWindow *window);
};
