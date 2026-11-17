#pragma once

#include "SqlDialect.h"

#include <QList>
#include <QString>

// 스크립트 안의 문장 하나. 위치는 스크립트 문자열 기준 인덱스다.
struct SqlStatement
{
    int start = 0;         // 문장의 첫 글자 (앞에 붙은 주석 포함)
    int end = 0;           // 문장 끝 바로 다음 (구분자와 끝 공백 제외)
    int terminatorEnd = 0; // 구분자(';', DELIMITER 문자열, '/' 줄)까지 포함한 끝
    QString text;          // 실행할 문장. 구분자는 빠지고, PL/SQL 블록은 마지막 END; 까지 들어간다
};

// 방언 규칙에 따라 스크립트를 실행 단위 문장으로 나눈다.
// 문자열, 인용 식별자, 주석 안의 구분자는 무시한다.
namespace SqlSplitter {
QList<SqlStatement> split(const QString &script, SqlDialect dialect);

// 커서 위치에서 실행할 문장의 인덱스. 문장 사이 빈 곳이면 바로 앞 문장, 문장이 없으면 -1.
int statementIndexAt(const QList<SqlStatement> &statements, int position);
} // namespace SqlSplitter
