#pragma once

#include <QTabBar>
#include <QTabWidget>

// 쿼리 탭 막대. 닫기 버튼은 선택된 탭과 마우스를 올린 탭에만 보이고, 가운데 버튼 클릭으로도 닫는다.
// 버튼 자리는 늘 잡아 두어서 버튼이 나타나고 사라져도 탭 폭이 흔들리지 않는다.
class EditorTabBar : public QTabBar
{
    Q_OBJECT

public:
    explicit EditorTabBar(QWidget *parent = nullptr);

protected:
    void tabInserted(int index) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void updateCloseButtons();

    int m_hovered = -1;
};

class EditorTabs : public QTabWidget
{
    Q_OBJECT

public:
    explicit EditorTabs(QWidget *parent = nullptr);
};
