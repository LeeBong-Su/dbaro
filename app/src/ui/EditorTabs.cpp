#include "EditorTabs.h"

#include "Icons.h"

#include <QMouseEvent>
#include <QToolButton>

EditorTabBar::EditorTabBar(QWidget *parent)
    : QTabBar(parent)
{
    setMouseTracking(true);
    setExpanding(false);
    setMovable(true);
    setElideMode(Qt::ElideRight);
    setUsesScrollButtons(true);
    connect(this, &QTabBar::currentChanged, this, &EditorTabBar::updateCloseButtons);
}

void EditorTabBar::tabInserted(int index)
{
    QTabBar::tabInserted(index);

    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("tabCloseButton"));
    button->setAutoRaise(true);
    button->setFixedSize(18, 18);
    button->setIconSize(QSize(12, 12));
    button->setToolTip(tr("Close (Ctrl+W)"));
    button->setFocusPolicy(Qt::NoFocus);
    connect(button, &QToolButton::clicked, this, [this, button] {
        for (int i = 0; i < count(); ++i) {
            if (tabButton(i, QTabBar::RightSide) == button) {
                emit tabCloseRequested(i);
                return;
            }
        }
    });
    setTabButton(index, QTabBar::RightSide, button);
    updateCloseButtons();
}

void EditorTabBar::mouseMoveEvent(QMouseEvent *event)
{
    QTabBar::mouseMoveEvent(event);
    const int index = tabAt(event->position().toPoint());
    if (index != m_hovered) {
        m_hovered = index;
        updateCloseButtons();
    }
}

void EditorTabBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) {
        const int index = tabAt(event->position().toPoint());
        if (index >= 0) {
            emit tabCloseRequested(index);
            return;
        }
    }
    QTabBar::mouseReleaseEvent(event);
}

void EditorTabBar::leaveEvent(QEvent *event)
{
    QTabBar::leaveEvent(event);
    m_hovered = -1;
    updateCloseButtons();
}

void EditorTabBar::updateCloseButtons()
{
    const QIcon close = Icons::get(QStringLiteral("close"));
    for (int i = 0; i < count(); ++i) {
        if (auto *button = qobject_cast<QToolButton *>(tabButton(i, QTabBar::RightSide)))
            button->setIcon(i == currentIndex() || i == m_hovered ? close : QIcon());
    }
}

EditorTabs::EditorTabs(QWidget *parent)
    : QTabWidget(parent)
{
    setTabBar(new EditorTabBar(this));
    setDocumentMode(true);
    setMovable(true);
}
