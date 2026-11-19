#pragma once

#include <QColor>
#include <QList>
#include <QString>

class QWidget;

// Windows 작업 표시줄 연동. 다른 OS에서는 아무것도 하지 않는다.
namespace WindowsIntegration {

// 작업 표시줄이 DBaro 창들과 점프 목록을 한 앱으로 묶는 이름 (설치본 바로 가기의 System.AppUserModel.ID와 같다).
// 창을 만들기 전에 한 번 부른다. 부르지 않은 프로세스(테스트 도구)는 점프 목록을 바꾸지 않는다.
void setAppUserModelId();

struct JumpItem
{
    QString title;
    QString arguments;   // dbaro.exe에 넘길 인자 (--connection <id> 등)
    QString description; // 마우스를 올렸을 때 보이는 설명
};

// 작업 표시줄 아이콘 오른쪽 클릭 메뉴. category 아래에 items, 맨 아래 "작업"에 tasks를 둔다.
// 사용자가 목록에서 지운 항목은 다시 넣지 않는다 (Windows 규칙).
void updateJumpList(const QString &category, const QList<JumpItem> &items, const QList<JumpItem> &tasks);

// 창 테두리 색 (Windows 11). 잘못된 색이면 기본값으로 되돌린다.
void setBorderColor(QWidget *window, const QColor &color);

// 다른 프로세스가 띄운 창 요청을 받아 앞으로 가져올 수 있게 허락한다 (요청을 보내는 쪽에서 부른다).
void allowForegroundForOtherProcess();

// 창을 맨 앞으로 (최소화되어 있으면 되살린다)
void bringToFront(QWidget *window);

} // namespace WindowsIntegration
