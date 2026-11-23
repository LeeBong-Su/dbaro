#include "FontZoom.h"

#include <QCoreApplication>
#include <QSettings>
#include <QWheelEvent>

namespace {

QString key(FontZoom::Target target)
{
    return target == FontZoom::Target::Editor ? QStringLiteral("fonts/editorSize") : QStringLiteral("fonts/consoleSize");
}

} // namespace

FontZoom::FontZoom(QObject *parent)
    : QObject(parent)
{
}

FontZoom *FontZoom::instance()
{
    static FontZoom *zoom = new FontZoom(QCoreApplication::instance());
    return zoom;
}

qreal FontZoom::pointSize(Target target, qreal fallback) const
{
    const qreal saved = QSettings().value(key(target), 0.0).toReal();
    return saved >= kMinimum && saved <= kMaximum ? saved : fallback;
}

void FontZoom::setPointSize(Target target, qreal pointSize)
{
    const qreal size = qBound(kMinimum, pointSize, kMaximum);
    QSettings().setValue(key(target), size);
    emit changed(target, size);
}

bool FontZoom::handleWheel(QWheelEvent *event, Target target, qreal current)
{
    if (!(event->modifiers() & Qt::ControlModifier))
        return false;
    event->accept();
    int &remainder = m_remainder[int(target)];
    remainder += event->angleDelta().y();
    const int steps = remainder / 120;
    remainder -= steps * 120;
    if (steps != 0) {
        const qreal size = qBound(kMinimum, current + steps, kMaximum);
        if (!qFuzzyCompare(size, current))
            setPointSize(target, size);
    }
    return true;
}
