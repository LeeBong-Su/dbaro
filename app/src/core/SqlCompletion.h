#pragma once

#include "SqlDialect.h"

#include <QList>
#include <QString>
#include <QStringList>

// 자동완성용 문맥 분석. 문장 하나와 커서 위치를 받아 무엇을 제안할지 정한다.
// 완전한 파서가 아니라 입력 중인(문법이 아직 맞지 않는) SQL에서도 동작하도록 토큰을 앞뒤로 살핀다.

struct SqlTableRef
{
    QString schema; // 비어 있으면 현재 스키마
    QString name;
    QString alias;  // 없으면 빈 값
    QString nameText; // 문장에 쓴 그대로의 테이블 이름 (따옴표 포함, 스키마 제외)
    int position = -1; // 문장 안에서 이 테이블이 나온 곳 (스키마 포함 이름의 시작)

    // 컬럼 앞에 붙일 이름: 별칭, 없으면 쓴 그대로의 테이블 이름
    QString qualifier() const { return alias.isEmpty() ? nameText : alias; }
};

struct SqlCompletionContext
{
    enum class Kind {
        None,    // 문자열·주석 안 등: 제안하지 않음
        Keyword, // 키워드만 (FROM emp | 다음처럼 테이블 이름 뒤)
        Table,   // FROM, JOIN, UPDATE, INTO, TABLE 뒤: 테이블·뷰와 스키마
        Column,  // SELECT, WHERE, ON, SET, BY 등 뒤: 컬럼, 별칭, 함수, 키워드
    };

    Kind kind = Kind::None;
    QString prefix;         // 커서 앞까지 입력한 단어 (따옴표는 뺀 값)
    int prefixStart = 0;    // 문장 안에서 바꿔 넣을 부분의 시작 (여는 따옴표 포함)
    bool quoted = false;    // 단어가 따옴표로 시작함
    QString qualifier;      // "x.|"의 x (따옴표를 뺀 이름). 없으면 빈 값
    QString outerQualifier; // "a.b.|"의 a
    QList<SqlTableRef> tables; // 문장에서 FROM/JOIN/UPDATE/INTO 뒤에 나온 테이블
    // JOIN t ON | : 방금 JOIN한 테이블 (tables의 위치). 외래 키로 이은 조건을 제안한다. 아니면 -1
    int joinTable = -1;
    // JOIN | : 테이블 이름 자리. 앞의 테이블과 외래 키로 이어진 테이블을 ON 조건까지 제안한다
    bool afterJoin = false;
};


namespace SqlCompletion {

SqlCompletionContext analyze(const QString &statement, int cursor, SqlDialect dialect);

// 제안할 키워드와 함수 (대문자)
const QStringList &keywords();
const QStringList &functions(SqlDialect dialect);

// 문장 안의 별칭이나 테이블 이름에 맞는 테이블 (대소문자 무시). 없으면 nullptr.
const SqlTableRef *findTable(const SqlCompletionContext &context, const QString &aliasOrName);

} // namespace SqlCompletion
