#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// 터미널에 그리는 글자 표 (dbaro-cli). 한글·한자처럼 두 칸을 차지하는 글자를 세어 줄을 맞춘다.
//
//   +----+--------+
//   | ID | NAME   |
//   +----+--------+
//   |  1 | 홍길동 |
//   +----+--------+
namespace TextTable {

// 고정폭 터미널에서 차지하는 칸 수 (동아시아 넓은 글자 2칸, 결합 문자 0칸)
int displayWidth(const QString &text);

// 칸 수가 maxWidth를 넘으면 잘라서 끝에 "…"를 붙인다. 줄바꿈·탭은 공백으로.
QString fitCell(const QString &text, int maxWidth);

// rightAlign[i]가 true인 컬럼은 오른쪽 정렬 (숫자). 줄 끝은 '\n'.
// header가 false면 머리글 줄 없이 (SQL*Plus의 SET HEADING OFF). 칸 너비는 값으로만 정한다.
QString render(const QStringList &headers, const QList<QStringList> &rows, const QList<bool> &rightAlign,
               int maxCellWidth = 60, bool showHeader = true);

} // namespace TextTable
