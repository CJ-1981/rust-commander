#pragma once

#include <QColor>
#include <QString>

class QPalette;

// The app's look: one flat design with a light and a dark theme.
namespace Theme {

enum class Mode {
    System,  // follow the operating system
    Light,
    Dark,
};

QString modeName(Mode mode);
Mode modeFromName(const QString &name, Mode fallback);

// Whether the operating system currently asks for a dark color scheme.
bool systemIsDark();

// Installs the style, palette and stylesheet for `mode` ("System" resolves to
// the operating system's current color scheme).
void apply(Mode mode);

// Re-apply after the system's color scheme changed. Returns true if something
// changed.
bool refreshSystem();

// Colors of the current theme, for code that paints by hand.
QColor accent();
QColor muted();
// A faint wash for what the pointer is over, or was just clicked.
QColor hover();

}  // namespace Theme
