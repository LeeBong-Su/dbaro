#pragma once

#include <QIcon>
#include <QString>

// DBaro 자체 아이콘 (app/resources/icons/*.svg).
// SVG 안의 "#ICON" 색은 그릴 때마다 현재 테마의 아이콘 색으로 바뀐다. 비활성 상태는 흐리게 그린다.
namespace Icons {
QIcon get(const QString &name);
}
