#pragma once

#include "ConnectionProfile.h"

// SQL을 읽는 규칙의 계열. 하이라이트와 문장 분리가 이 값에 따라 달라진다.
// - Oracle: Oracle, Tibero. PL/SQL 블록은 한 줄에 '/'만 있는 줄로 끝난다. q'[...]' 문자열.
// - MySql: MySQL, MariaDB. DELIMITER 명령, # 주석, `식별자`, 문자열 안 백슬래시 이스케이프.
// - PostgreSql: $$ 또는 $태그$ 로 감싼 본문, E'...' 문자열.
// - Generic: 연결을 고르지 않았을 때. ';'와 '/' 줄로만 나눈다.
enum class SqlDialect { Generic, Oracle, MySql, PostgreSql };

inline SqlDialect sqlDialectFor(DbType type)
{
    switch (type) {
    case DbType::Oracle:
    case DbType::Tibero:
        return SqlDialect::Oracle;
    case DbType::MariaDB:
    case DbType::MySQL:
        return SqlDialect::MySql;
    case DbType::PostgreSQL:
        return SqlDialect::PostgreSql;
    }
    return SqlDialect::Generic;
}
