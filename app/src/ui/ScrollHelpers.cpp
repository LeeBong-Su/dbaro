#include "ScrollHelpers.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QScrollBar>
#include <QWheelEvent>

namespace {

class ShiftWheelFilter : public QObject
{
public:
    explicit ShiftWheelFilter(QAbstractScrollArea *area)
        : QObject(area)
        , m_area(area)
    {
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() != QEvent::Wheel)
            return false;
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (!(wheel->modifiers() & Qt::ShiftModifier))
            return false;

        const QPoint angle = wheel->angleDelta();
        const QPoint pixel = wheel->pixelDelta();
        if (angle.y() == 0 && pixel.y() == 0)
            return false; // 이미 가로 휠(터치패드 등)

        QScrollBar *bar = m_area->horizontalScrollBar();
        if (!bar || bar->maximum() <= bar->minimum())
            return true; // 가로로 넘치는 내용이 없으면 아무것도 하지 않는다

        // 휠 한 칸(120)당 세로 스크롤과 같은 줄 수만큼 옆으로 옮긴다. 고해상도 휠의 작은 값은 모아서 쓴다.
        int delta = 0;
        if (!pixel.isNull()) {
            delta = pixel.y();
        } else {
            m_remainder += angle.y() * bar->singleStep() * QApplication::wheelScrollLines();
            delta = m_remainder / 120;
            m_remainder -= delta * 120;
        }
        bar->setValue(bar->value() - delta);
        wheel->accept();
        return true;
    }

private:
    QAbstractScrollArea *m_area;
    int m_remainder = 0;
};

} // namespace

void enableShiftWheelHorizontalScroll(QAbstractScrollArea *area)
{
    area->viewport()->installEventFilter(new ShiftWheelFilter(area));
}
