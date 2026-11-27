#include "Icons.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QIODevice>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>
#include <QVariant>

#include <qdebug.h>

namespace {

constexpr int kSize = 16;  // logical pixels, the size the rows of buttons and the menus ask for
// What can't be used now is still to be seen, only clearly dimmed.
constexpr qreal kDisabledOpacity = 0.4;

// The name of the icon, left on whatever shows it by Icons::setIcon().
constexpr const char *kIconName = "iconName";

// The drawing of `name`, as it is in resources/icons; empty when there is none.
QString drawing(const QString &name)
{
    QFile file(":/icons/" + name + ".svg");
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("Icons: there is no icon \"%s\"", qPrintable(name));
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

// The drawing with its strokes in `color` instead of "currentColor".
QByteArray colored(const QString &svg, const QColor &color)
{
    QString stroke = QStringLiteral("stroke=\"%1\"").arg(color.name());
    if (color.alpha() < 255) {
        stroke += QStringLiteral(" stroke-opacity=\"%1\"").arg(color.alphaF());
    }
    QString result = svg;
    result.replace(QStringLiteral("stroke=\"currentColor\""), stroke);
    return result.toUtf8();
}

// The drawing in a transparent square of `size` logical pixels, at `ratio`
// device pixels per logical pixel. The painter counts in logical pixels.
QPixmap rendered(const QByteArray &svg, int size, qreal ratio)
{
    QPixmap pixmap(int(size * ratio), int(size * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QSvgRenderer renderer(svg);
    if (renderer.isValid()) {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        renderer.render(&painter, QRectF(0, 0, size, size));
    }
    return pixmap;
}

// The icon in the colors of the current theme: as text is drawn, and dimmed
// for when it can't be used. Nothing when there is no such icon.
QIcon load(const QString &name)
{
    const QString svg = drawing(name);
    if (svg.isEmpty()) {
        return {};
    }
    QColor text = QGuiApplication::palette().color(QPalette::WindowText);
    QColor disabled = text;
    disabled.setAlphaF(kDisabledOpacity);

    const QByteArray normal = colored(svg, text);
    const QByteArray dim = colored(svg, disabled);
    QIcon icon;
    // For ordinary screens, and for Retina ones.
    for (qreal ratio : {1.0, 2.0}) {
        icon.addPixmap(rendered(normal, kSize, ratio), QIcon::Normal);
        icon.addPixmap(rendered(dim, kSize, ratio), QIcon::Disabled);
    }
    return icon;
}

QHash<QString, QIcon> &cache()
{
    static QHash<QString, QIcon> icons;
    return icons;
}

// Gives `target` the icon it is named after, as drawn for the current theme.
void apply(QObject *target, const QString &name)
{
    const QIcon icon = Icons::icon(name);
    if (auto *action = qobject_cast<QAction *>(target)) {
        action->setIcon(icon);
    } else if (auto *button = qobject_cast<QAbstractButton *>(target)) {
        button->setIcon(icon);
    }
}

}  // namespace

QIcon Icons::icon(const QString &name)
{
    if (!cache().contains(name)) {
        cache().insert(name, load(name));
    }
    return cache().value(name);
}

void Icons::setIcon(QObject *target, const QString &name)
{
    // What has the icon already is left alone: whoever shows it would only
    // have to draw it again.
    if (target->property(kIconName).toString() == name) {
        return;
    }
    target->setProperty(kIconName, name);
    apply(target, name);
}

QString Icons::nameOf(const QObject *target)
{
    return target->property(kIconName).toString();
}

void Icons::reload()
{
    cache().clear();
    // What was given an icon is found by the name left on it, where it is in
    // any window of the app.
    for (QWidget *window : QApplication::topLevelWidgets()) {
        for (QObject *target : window->findChildren<QObject *>()) {
            const QVariant name = target->property(kIconName);
            if (!name.isNull()) {
                apply(target, name.toString());
            }
        }
    }
}
