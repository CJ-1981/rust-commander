#pragma once

#include <QString>

class QIcon;
class QObject;

// The app's icons: Lucide's stroke drawings in resources/icons, in the colors
// of the current theme. They are drawn in "currentColor", which Qt's SVG
// renderer doesn't resolve, so each one is read as text, recolored and
// rendered here, once per theme.
namespace Icons {

// The icon `name`: the name of a file in resources/icons, without ".svg".
QIcon icon(const QString &name);
// Gives `target`, an action or a button, the icon `name`. The name stays with
// the target: that is how reload() finds all there is to recolor.
void setIcon(QObject *target, const QString &name);
// The name of the icon `target` was given; empty when it has none.
QString nameOf(const QObject *target);
// Draws every icon again, in the colors of the theme that is current now.
// Theme::apply() calls this.
void reload();

}  // namespace Icons
