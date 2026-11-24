#include "BusyIndicator.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QColor accentColor(const QWidget *widget)
{
    return (widget ? widget->palette() : QApplication::palette()).color(QPalette::Link);
}

// 3/4 원호 (끝은 둥글게). angle만큼 돌린다
void paintArc(QPainter &painter, const QRectF &box, int angle, const QColor &color)
{
    const qreal width = qMax<qreal>(1.5, box.width() / 8);
    const QRectF arc = box.adjusted(width / 2, width / 2, -width / 2, -width / 2);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor track = color;
    track.setAlphaF(0.18);
    painter.setPen(QPen(track, width));
    painter.drawEllipse(arc);
    painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(arc, (90 - angle) * 16, -270 * 16);
}

// 3.2 s, 1:05, 1:02:03
QString formatElapsed(qint64 ms)
{
    if (ms < 60'000)
        return QCoreApplication::translate("BusyOverlay", "%1 s").arg(QString::number(ms / 1000.0, 'f', 1));
    const qint64 seconds = ms / 1000;
    if (seconds < 3600)
        return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600)
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

// 덮은 영역을 흐리게 (마우스는 그대로 아래로)
class OverlayDim : public QWidget
{
public:
    explicit OverlayDim(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setObjectName(QStringLiteral("busyDim"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        QColor color = palette().color(QPalette::Window);
        color.setAlpha(150);
        painter.fillRect(rect(), color);
    }
};

// 둥근 카드
class OverlayCard : public QFrame
{
public:
    explicit OverlayCard(QWidget *parent)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("busyCard"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor border = palette().color(QPalette::Text);
        border.setAlpha(60);
        painter.setPen(QPen(border, 1));
        painter.setBrush(palette().color(QPalette::Window));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    }
};

QString oneLine(const QString &text)
{
    return text.simplified();
}

} // namespace

// ---------------------------------------------------------------------------
// BusySpinner

BusySpinner::BusySpinner(int size, QWidget *parent)
    : QWidget(parent)
    , m_size(size)
{
    setFixedSize(size, size);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    m_timer = new QTimer(this);
    m_timer->setInterval(40);
    connect(m_timer, &QTimer::timeout, this, [this] {
        m_angle = (m_angle + 18) % 360;
        update();
    });
}

QSize BusySpinner::sizeHint() const
{
    return {m_size, m_size};
}

QPixmap BusySpinner::frame(int size, int angle, const QColor &color, qreal devicePixelRatio)
{
    QPixmap pixmap(QSize(size, size) * devicePixelRatio);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paintArc(painter, QRectF(1, 1, size - 2, size - 2), angle, color);
    return pixmap;
}

void BusySpinner::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    paintArc(painter, QRectF(rect()).adjusted(1, 1, -1, -1), m_angle, accentColor(this));
}

void BusySpinner::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_timer->start();
}

void BusySpinner::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

// ---------------------------------------------------------------------------
// BusyOverlay

BusyOverlay::BusyOverlay(QWidget *target, QObject *parent)
    : QObject(parent ? parent : target)
    , m_target(target)
{
    m_dim = new OverlayDim(target);
    m_dim->hide();

    m_card = new OverlayCard(target);
    m_card->hide();
    m_spinner = new BusySpinner(28, m_card);
    m_title = new QLabel(m_card);
    m_title->setObjectName(QStringLiteral("busyTitle"));
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_detail = new QLabel(m_card);
    m_detail->setObjectName(QStringLiteral("busyDetail"));
    m_elapsed = new QLabel(m_card);
    m_elapsed->setObjectName(QStringLiteral("busyElapsed"));
    for (QLabel *label : {m_detail, m_elapsed}) {
        QPalette palette = label->palette();
        QColor dim = palette.color(QPalette::WindowText);
        dim.setAlpha(170);
        palette.setColor(QPalette::WindowText, dim);
        label->setPalette(palette);
    }
    m_cancel = new QPushButton(tr("Cancel"), m_card);
    m_cancel->setObjectName(QStringLiteral("busyCancel"));
    m_cancel->setCursor(Qt::ArrowCursor);
    m_cancel->setFocusPolicy(Qt::NoFocus); // 에디터의 포커스를 빼앗지 않는다
    connect(m_cancel, &QPushButton::clicked, this, &BusyOverlay::cancelRequested);

    auto *texts = new QVBoxLayout;
    texts->setSpacing(2);
    texts->addWidget(m_title);
    texts->addWidget(m_detail);
    texts->addWidget(m_elapsed);
    auto *layout = new QHBoxLayout(m_card);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(14);
    layout->addWidget(m_spinner, 0, Qt::AlignVCenter);
    layout->addLayout(texts, 1);
    layout->addWidget(m_cancel, 0, Qt::AlignVCenter);

    m_delay = new QTimer(this);
    m_delay->setSingleShot(true);
    connect(m_delay, &QTimer::timeout, this, [this] {
        m_due = true;
        updateVisibility();
    });
    m_clockTimer = new QTimer(this);
    m_clockTimer->setInterval(100);
    connect(m_clockTimer, &QTimer::timeout, this, &BusyOverlay::tick);

    target->installEventFilter(this);
}

void BusyOverlay::start(const QString &title, const QString &detail, bool cancellable)
{
    m_active = true;
    m_due = false;
    m_cancel->setVisible(cancellable);
    setText(title, detail);
    m_clock.start();
    tick();
    m_clockTimer->start();
    m_delay->start(m_delayMs);
    updateVisibility();
}

void BusyOverlay::setText(const QString &title, const QString &detail)
{
    m_title->setText(title);
    m_fullDetail = oneLine(detail);
    m_detail->setToolTip(detail.size() > 2000 ? detail.left(2000) + QStringLiteral("...") : detail);
    m_detail->setVisible(!m_fullDetail.isEmpty());
    m_detail->setText(m_detail->fontMetrics().elidedText(m_fullDetail, Qt::ElideRight, 460));
    if (isShown())
        place();
}

void BusyOverlay::stop()
{
    m_active = false;
    m_due = false;
    m_delay->stop();
    m_clockTimer->stop();
    updateVisibility();
}

void BusyOverlay::setCompact(bool compact)
{
    m_compact = compact;
    updateVisibility();
}

bool BusyOverlay::isDimmed() const
{
    return m_dim->isVisible();
}

bool BusyOverlay::isShown() const
{
    return m_card->isVisible();
}

QString BusyOverlay::title() const
{
    return m_title->text();
}

QString BusyOverlay::detail() const
{
    return m_fullDetail;
}

QString BusyOverlay::elapsedText() const
{
    return m_elapsed->text();
}

bool BusyOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_target && (event->type() == QEvent::Resize || event->type() == QEvent::Show) && isShown())
        place();
    return QObject::eventFilter(watched, event);
}

void BusyOverlay::updateVisibility()
{
    const bool show = m_active && m_due && m_target;
    m_dim->setVisible(show && !m_compact);
    m_card->setVisible(show);
    if (show) {
        place();
        m_dim->raise();
        m_card->raise();
    }
}

void BusyOverlay::place()
{
    if (!m_target)
        return;
    const QRect area = m_target->rect();
    m_dim->setGeometry(area);
    m_card->adjustSize();
    const int width = qMin(qMax(m_card->sizeHint().width(), 340), qMax(area.width() - 32, 200));
    const int height = m_card->sizeHint().height();
    if (m_compact) {
        // 오른쪽 위 구석 (Log의 마지막 줄은 아래에 쌓이니 가리지 않는다)
        m_card->setGeometry(qMax(8, area.width() - width - 24), 10, width, height);
        return;
    }
    // 가운데보다 조금 위 (영역이 낮으면 맨 위)
    const int top = qMax(8, qMin((area.height() - height) * 2 / 5, area.height() - height - 8));
    m_card->setGeometry((area.width() - width) / 2, top, width, height);
}

void BusyOverlay::tick()
{
    m_elapsed->setText(formatElapsed(m_clock.isValid() ? m_clock.elapsed() : 0));
}

// ---------------------------------------------------------------------------
// BusyBadge

BusyBadge::BusyBadge(QWidget *target)
    : QFrame(target)
    , m_target(target)
{
    setObjectName(QStringLiteral("busyBadge"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    m_spinner = new BusySpinner(14, this);
    m_label = new QLabel(this);
    m_label->setObjectName(QStringLiteral("busyBadgeText"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 5, 12, 5);
    layout->setSpacing(7);
    layout->addWidget(m_spinner, 0, Qt::AlignVCenter);
    layout->addWidget(m_label, 0, Qt::AlignVCenter);
    hide();

    m_delay = new QTimer(this);
    m_delay->setSingleShot(true);
    connect(m_delay, &QTimer::timeout, this, [this] {
        if (m_tasks.isEmpty())
            return;
        place();
        show();
        raise();
    });
    target->installEventFilter(this);
}

int BusyBadge::begin(const QString &text)
{
    const int ticket = m_nextTicket++;
    m_tasks.append({ticket, text});
    if (m_tasks.size() == 1 && !isVisible())
        m_delay->start(m_delayMs);
    refresh();
    return ticket;
}

void BusyBadge::end(int ticket)
{
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].first == ticket) {
            m_tasks.removeAt(i);
            break;
        }
    }
    if (m_tasks.isEmpty()) {
        m_delay->stop();
        hide();
        return;
    }
    refresh();
}

QString BusyBadge::text() const
{
    return m_label->text();
}

bool BusyBadge::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_target && event->type() == QEvent::Resize && isVisible())
        place();
    return QFrame::eventFilter(watched, event);
}

void BusyBadge::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor fill = palette().color(QPalette::Window);
    fill.setAlpha(240);
    QColor border = accentColor(this);
    border.setAlpha(170);
    painter.setPen(QPen(border, 1));
    painter.setBrush(fill);
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.drawRoundedRect(box, box.height() / 2, box.height() / 2);
}

void BusyBadge::refresh()
{
    if (m_tasks.isEmpty())
        return;
    QString text = m_tasks.last().second;
    if (m_tasks.size() > 1)
        text += QStringLiteral("  (+%1)").arg(m_tasks.size() - 1);
    m_label->setText(text);
    if (isVisible())
        place();
}

void BusyBadge::place()
{
    if (!m_target)
        return;
    adjustSize();
    const QSize size = sizeHint();
    move(qMax(4, m_target->width() - size.width() - 14), 10);
    resize(size);
}
