#pragma once

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

// 연결 환경 색상을 나타내는 작은 원 아이콘. 색이 유효하지 않으면 같은 크기의 빈 아이콘을 돌려준다.
inline QIcon colorSwatchIcon(const QColor &color, int size = 12)
{
    constexpr qreal scale = 2.0; // 고해상도 화면에서도 선명하게
    QPixmap pixmap(qRound(size * scale), qRound(size * scale));
    pixmap.fill(Qt::transparent);
    if (color.isValid()) {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QRectF(2, 2, pixmap.width() - 4, pixmap.height() - 4));
    }
    pixmap.setDevicePixelRatio(scale);
    return QIcon(pixmap);
}
