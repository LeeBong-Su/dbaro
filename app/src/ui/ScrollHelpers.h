#pragma once

class QAbstractScrollArea;

// Shift + 마우스 휠로 가로 스크롤한다 (Windows 탐색기·브라우저와 같은 동작).
// Qt 기본 동작은 Shift + 휠이 세로 페이지 단위 스크롤이라 바꾼다.
void enableShiftWheelHorizontalScroll(QAbstractScrollArea *area);
