#include "Theme.h"

#include "Icons.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>

namespace {

struct Colors {
    QColor window;
    QColor base;
    QColor alternate;
    QColor text;
    QColor muted;
    QColor border;
    QColor accent;
    QColor hover;
    QColor pressed;
    QColor menu;
};

Colors colorsFor(bool dark)
{
    // Colors are given as text so that style sheets and palettes agree.
    if (dark) {
        return {
            QColor(0x1b1d21),  // window
            QColor(0x141619),  // base
            QColor(0x191b1f),  // alternate rows
            QColor(0xe5e7ea),  // text
            QColor(0x9aa1ab),  // muted text
            QColor(0x2c3037),  // borders
            QColor(0x3b82f6),  // accent
            QColor(255, 255, 255, 18),  // hover
            QColor(255, 255, 255, 31),  // pressed
            QColor(0x22252a),  // menus, tooltips
        };
    }
    return {
        QColor(0xf2f3f5),
        QColor(0xffffff),
        QColor(0xf5f6f8),
        QColor(0x1f2329),
        QColor(0x6b7280),
        QColor(0xd9dce1),
        QColor(0x2563eb),
        QColor(0, 0, 0, 16),
        QColor(0, 0, 0, 26),
        QColor(0xffffff),
    };
}

QPalette makePalette(const Colors &c)
{
    QPalette p;
    p.setColor(QPalette::Window, c.window);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.base);
    p.setColor(QPalette::AlternateBase, c.alternate);
    p.setColor(QPalette::ToolTipBase, c.menu);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.base);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Light, c.alternate);
    p.setColor(QPalette::Midlight, c.window);
    p.setColor(QPalette::Mid, c.border);
    p.setColor(QPalette::Dark, c.muted);
    p.setColor(QPalette::Shadow, c.muted);
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::PlaceholderText, c.muted);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::Highlight}) {
        p.setColor(QPalette::Disabled, role, c.border);
    }
    return p;
}

QString fillIn(const QString &spec, const Colors &c)
{
    QString s = spec;
    s.replace("@window@", c.window.name());
    s.replace("@base@", c.base.name());
    s.replace("@alternate@", c.alternate.name());
    s.replace("@text@", c.text.name());
    s.replace("@muted@", c.muted.name());
    s.replace("@border@", c.border.name());
    s.replace("@accent@", c.accent.name());
    s.replace("@hover@", c.hover.name(QColor::HexArgb));
    s.replace("@pressed@", c.pressed.name(QColor::HexArgb));
    s.replace("@menu@", c.menu.name());
    // A fainter selection for the pane that is not the active one.
    QColor dim = c.accent;
    dim.setAlpha(c.window.lightness() > 128 ? 60 : 84);
    s.replace("@dimSelection@", dim.name(QColor::HexArgb));
    return s;
}

// The stylesheet restyles the chrome around the content (toolbars, tab bars,
// table headers); text and selection colors come from the palette.
const char *kStyleSheet = R"(
QToolBar { border: none; padding: 4px 6px; spacing: 2px; background: transparent; }
QToolButton { border: 1px solid transparent; border-radius: 6px; padding: 5px; background: transparent; }
QToolButton:hover { background: @hover@; }
QToolButton:pressed, QToolButton:checked { background: @pressed@; }
QToolButton::menu-indicator { image: none; }
QToolBar#paneToolBar { padding: 2px 0; }
QToolBar#paneToolBar QToolButton { padding: 2px; }
QToolBar#paneToolBar::separator { width: 1px; background: @border@; margin: 4px; }
QToolButton#newTabButton { padding: 2px; }

QTabBar::tab { background: transparent; color: @muted@; padding: 5px 10px; margin-right: 2px;
               border: 1px solid transparent; border-bottom: none;
               border-top-left-radius: 6px; border-top-right-radius: 6px; }
QTabBar::tab:hover { background: @hover@; }
QTabBar::tab:selected { background: @base@; border-color: @border@; color: @text@; }
#paneMark { background: transparent; }
#paneMark[active="true"] { background: @accent@; }
QToolButton#tabClose { padding: 0; border-radius: 3px; color: @muted@; }

QTableView { background: @base@; alternate-background-color: @alternate@; border: none;
             gridline-color: transparent; selection-background-color: @dimSelection@;
             selection-color: @text@; outline: none; }
QTableView[active="true"] { selection-background-color: @accent@; selection-color: #ffffff; }
QTableView::item { padding: 0 6px; border: none; }
QHeaderView::section { background: @base@; color: @muted@; border: none;
                       border-bottom: 1px solid @border@; padding: 6px; }
QTableCornerButton::section { background: @base@; border: none; }

QSplitter::handle:horizontal { width: 7px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 @window@, stop:0.5 @border@, stop:0.64 @border@, stop:0.64 @window@); }
QSplitter::handle:horizontal:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 @window@, stop:0.4 @accent@, stop:0.6 @accent@, stop:0.6 @window@); }
QSplitter::handle:vertical { height: 7px; background: @border@; }

QLineEdit { border: 1px solid @border@; border-radius: 6px; padding: 4px 8px;
            background: @base@; selection-background-color: @accent@; selection-color: #ffffff; }
QLineEdit:focus { border-color: @accent@; }
QComboBox { border: 1px solid @border@; border-radius: 6px; padding: 4px 8px; background: @base@; }
QComboBox:focus { border-color: @accent@; }
QPlainTextEdit#licenseText { border: 1px solid @border@; border-radius: 6px; padding: 4px; background: @base@; }

QMenuBar { background: transparent; }
QMenuBar::item { padding: 4px 8px; border-radius: 4px; }
QMenuBar::item:selected { background: @hover@; }
QMenu { background: @menu@; border: 1px solid @border@; border-radius: 6px; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 12px; border-radius: 4px; }
QMenu::item:selected { background: @accent@; color: #ffffff; }
QMenu::separator { height: 1px; background: @border@; margin: 4px 8px; }

#keyRow { border-top: 1px solid @border@; }
#keyRow QToolButton { padding: 4px 8px; color: @text@; }
#paneStatus { color: @muted@; border-top: 1px solid @border@; }
QStatusBar::item { border: none; }

QProgressBar { border: none; border-radius: 4px; background: @alternate@; height: 8px; }
QProgressBar::chunk { border-radius: 4px; background: @accent@; }

QToolTip { background: @menu@; color: @text@; border: 1px solid @border@; padding: 4px 6px; }
QCheckBox, QLabel, QRadioButton { background: transparent; }
QDialog { background: @window@; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @border@; border-radius: 3px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: @muted@; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: @border@; border-radius: 3px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: @muted@; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
)";

Theme::Mode g_mode = Theme::Mode::System;
bool g_dark = false;
Colors g_colors = colorsFor(false);

}  // namespace

namespace Theme {

QString modeName(Mode mode)
{
    switch (mode) {
    case Mode::Light:
        return "light";
    case Mode::Dark:
        return "dark";
    default:
        return "system";
    }
}

Mode modeFromName(const QString &name, Mode fallback)
{
    if (name == "light") {
        return Mode::Light;
    }
    if (name == "dark") {
        return Mode::Dark;
    }
    if (name == "system") {
        return Mode::System;
    }
    return fallback;
}

bool systemIsDark()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void apply(Mode mode)
{
    g_mode = mode;
    g_dark = mode == Mode::Dark || (mode == Mode::System && systemIsDark());
    g_colors = colorsFor(g_dark);

    // Fusion takes the palette everywhere the same way, on every platform.
    if (auto *style = QStyleFactory::create("Fusion")) {
        QApplication::setStyle(style);
    }
    QApplication::setPalette(makePalette(g_colors));
    qApp->setStyleSheet(fillIn(QString::fromUtf8(kStyleSheet), g_colors));
    // The parts of the window the system draws, above all the title bar, keep
    // following the system appearance unless the app says which one it wants.
    // An explicit choice pins them to it; "like the system" hands them back.
    if (mode == Mode::System) {
        QGuiApplication::styleHints()->unsetColorScheme();
    } else {
        QGuiApplication::styleHints()->setColorScheme(g_dark ? Qt::ColorScheme::Dark
                                                             : Qt::ColorScheme::Light);
    }
    // The icons have the theme's colors in their pixmaps, so they are drawn again.
    Icons::reload();
}

bool refreshSystem()
{
    if (g_mode != Mode::System) {
        return false;
    }
    const bool dark = systemIsDark();
    if (dark == g_dark) {
        return false;
    }
    apply(Mode::System);
    return true;
}

QColor accent()
{
    return g_colors.accent;
}

QColor muted()
{
    return g_colors.muted;
}

QColor hover()
{
    return g_colors.hover;
}

}  // namespace Theme
