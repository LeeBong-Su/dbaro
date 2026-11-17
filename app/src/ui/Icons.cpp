#include "Icons.h"

#include "Theme.h"

#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace {

class SvgIconEngine : public QIconEngine
{
public:
    explicit SvgIconEngine(const QString &path)
    {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            m_svg = file.readAll();
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        if (m_svg.isEmpty())
            return;
        QByteArray svg = m_svg;
        svg.replace("#ICON", Theme::iconColor().name().toLatin1());
        QSvgRenderer renderer(svg);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (mode == QIcon::Disabled)
            painter->setOpacity(0.35);
        renderer.render(painter, QRectF(rect));
        painter->restore();
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pixmap(size * scale);
        pixmap.fill(Qt::transparent);
        {
            QPainter painter(&pixmap);
            paint(&painter, QRect(QPoint(0, 0), pixmap.size()), mode, state);
        }
        pixmap.setDevicePixelRatio(scale);
        return pixmap;
    }

    QIconEngine *clone() const override { return new SvgIconEngine(*this); }
    QString key() const override { return QStringLiteral("DBaroSvgIcon"); }

private:
    QByteArray m_svg;
};

} // namespace

QIcon Icons::get(const QString &name)
{
    static QHash<QString, QIcon> cache;
    const auto it = cache.constFind(name);
    if (it != cache.constEnd())
        return it.value();

    QIcon icon(new SvgIconEngine(QStringLiteral(":/icons/%1.svg").arg(name)));
    cache.insert(name, icon);
    return icon;
}
