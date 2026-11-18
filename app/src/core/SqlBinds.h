#pragma once

#include "SqlDialect.h"
#include "SqlSplitter.h"

#include <QList>
#include <QString>
#include <QStringList>

// 문장 안의 바인드 변수. 실행 전에 값을 물어 그 자리에 넣는다.
// - :name, :1   이름 바인드 (모든 DB). 이름은 대소문자를 가리지 않는다
// - &name, &&name  SQL*Plus 치환 변수 (Oracle·Tibero). 입력한 글을 그대로 넣는다. &&는 한 번 정하면 탭에서 다시 묻지 않는다
// - ?           위치 바인드 (PostgreSQL 제외: JSONB 연산자 ?와 겹친다)
struct BindVariable
{
    enum class Kind { Named, Substitution, Positional };

    Kind kind = Kind::Named;
    QString name;        // emp_id, 1 (위치 바인드는 순번)
    bool define = false; // &&name

    QString label() const; // :emp_id, &dept, &&dept, ?1
    QString key() const;   // 값을 기억할 때 쓰는 이름 (:EMP_ID, &DEPT, ?1)
};

struct BindOccurrence
{
    int start = 0;
    int length = 0;
    int variable = 0; // 변수 목록의 위치
};

namespace SqlBinds {

struct Value
{
    enum class Type { Auto, Text, Raw, Null };
    Type type = Type::Auto;
    QString text;
};

// 문장 하나에서 바인드 변수를 찾아 variables에 더한다 (같은 이름은 하나로). 문자열·주석·인용 식별자 안은 무시한다.
// :NEW·:OLD가 들어 있는 트리거나 프로시저 본문을 만드는 CREATE 문은 건너뛴다.
// positionalCounter는 스크립트 전체에서 ?의 순번을 이어 매기는 데 쓴다.
QList<BindOccurrence> scan(const QString &sql, SqlDialect dialect, QList<BindVariable> *variables, int *positionalCounter);

// 값을 문장에 넣을 글로 바꾼다.
// Auto: 숫자면 그대로, NULL이면 NULL, 나머지는 따옴표 문자열. Text: 따옴표 문자열. Raw: 그대로. Null: NULL.
// 치환 변수(&)는 SQL*Plus처럼 늘 입력한 그대로 넣는다.
QString literal(const BindVariable &variable, const Value &value, SqlDialect dialect);

// occurrences 자리를 변수별 글(replacements[변수 위치])로 바꾼 문장. edits에 바꾼 자리를 남긴다.
QString substitute(const QString &sql, const QList<BindOccurrence> &occurrences, const QStringList &replacements,
                   QList<SqlEdit> *edits);

} // namespace SqlBinds
