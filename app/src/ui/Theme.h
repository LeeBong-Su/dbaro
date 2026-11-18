#pragma once

#include <QColor>

// DBaro 화면 테마. OS 설정과 상관없이 같은 모습이 되도록 Fusion 스타일에 자체 색상을 입힌다.
// 기본은 다크 그레이다. 선택은 QSettings("appearance/theme")에 저장한다.
namespace Theme {

enum class Kind { Dark, Light };

Kind current();
Kind load();
void save(Kind kind);
void apply(Kind kind);

QColor iconColor(); // 단색 아이콘을 칠할 색 (현재 테마 기준)
QColor searchMatchColor();  // 에디터에서 찾은 글자 바탕
QColor currentMatchColor(); // 찾은 것 중 지금 고른 것의 바탕

} // namespace Theme
