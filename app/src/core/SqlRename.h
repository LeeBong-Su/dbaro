#pragma once

#include "SqlDialect.h"

#include <QList>
#include <QString>

// 이름 바꾸기 리팩토링의 대상: 테이블 하나, 또는 그 테이블의 컬럼 하나.
// 이름은 DB에 저장된 그대로(Oracle 대문자, PostgreSQL 소문자)다.
struct RenameTarget
{
    SqlDialect dialect = SqlDialect::Generic;
    QString schema;
    QString table;
    QString column;  // 비면 테이블 이름 바꾸기
    QString newName; // DB가 저장할 새 이름 (SqlRename::storedName)

    bool isColumn() const { return !column.isEmpty(); }
    QString oldName() const { return isColumn() ? column : table; }
};

// SQL 글 안에서 바꿀 곳 하나. 위치는 글 전체 기준이다.
struct RenameMatch
{
    int start = 0;
    int length = 0;
    int line = 1;         // 1부터
    QString replacement;  // 그 자리에 넣을 글 (원래 모양대로: 따옴표, 대소문자)
    bool suggested = true; // 기본으로 바꿀지 (확실하지 않으면 false)
    QString reason;       // 왜 그렇게 봤는지 (미리보기에 보인다)
};

// SQL 글(파일, 쿼리 탭, 뷰·프로시저 소스)에서 테이블·컬럼 이름을 찾아 바꾼다.
// - 주석은 건너뛴다. 문자열 안(동적 SQL)은 찾되 기본으로 바꾸지 않는다
// - PostgreSQL $$ 본문은 코드로 본다 (함수 본문 안의 참조를 찾으려고)
// - 따옴표 없는 이름은 대소문자를 무시하고, 따옴표로 감싼 이름은 그대로 비교한다 (MySQL 백틱은 무시)
// - 테이블: 다른 스키마로 한정한 이름(other.EMP)은 바꾸지 않는다. 스키마로 한정했거나 컬럼 한정자로 쓴 이름은 확실
// - 컬럼: 그 테이블을 쓰는 문장 안에서만 찾는다. 테이블 이름·별칭(FROM emp e)·NEW·OLD로 한정했으면 확실,
//   한정자가 없으면 바꾸기로, 다른 한정자(d.dept_id)면 바꾸지 않기로 둔다
namespace SqlRename {

QList<RenameMatch> find(const QString &text, const RenameTarget &target);

// matches(겹치지 않는 것)를 바꾼 글
QString apply(const QString &text, const QList<RenameMatch> &matches);

// 사용자가 쓴 새 이름을 DB가 저장할 모양으로: 따옴표 없이 쓸 수 있으면 Oracle 대문자·PostgreSQL 소문자
QString storedName(const QString &typed, SqlDialect dialect);

// 새 이름이 쓸 수 있는 이름인지 (비었거나 따옴표·제어 문자가 있으면 이유를 돌려준다)
QString validate(const QString &typed);

// 이름을 바꾸는 문장 (끝 ; 포함): Oracle·Tibero·PostgreSQL ALTER TABLE … RENAME TO / RENAME COLUMN,
// MySQL·MariaDB RENAME TABLE / ALTER TABLE … RENAME COLUMN
QString renameStatement(const RenameTarget &target);

// 빠른 거르기: 글에 그 이름이 단어로 들어 있는지 (대소문자 무시)
bool mentions(const QString &text, const QString &name);

} // namespace SqlRename
