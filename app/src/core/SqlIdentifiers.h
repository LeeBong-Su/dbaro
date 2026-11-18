#pragma once

#include "SqlDialect.h"

#include <QString>

#include <optional>

// "schema.name", "\"Mixed\".\"Case\"", "`db`.`table`" 같은 이름. 따옴표로 감싼 부분은 대소문자를 그대로 쓴다.
struct QualifiedName
{
    QString schema; // 없으면 빈 값
    QString name;
    bool schemaQuoted = false;
    bool nameQuoted = false;
};

// SQL 식별자(테이블·컬럼 이름)를 문장에 넣을 때 쓰는 도우미.
// 따옴표가 필요 없는 평범한 이름은 그대로 두고, 대소문자·공백·특수 문자가 섞인 이름만 감싼다.
// - Oracle·Tibero·Generic: 대문자 이름은 그대로, 나머지는 "이름"
// - PostgreSQL: 소문자 이름은 그대로, 나머지는 "이름"
// - MySQL·MariaDB: 영문·숫자·_ 이름은 그대로, 나머지는 `이름`
namespace SqlIdentifiers {
QString quote(const QString &name, SqlDialect dialect);
QString qualified(const QString &schema, const QString &name, SqlDialect dialect);
// 앞뒤 공백·끝의 ; , 는 무시하고, 이름 뒤에 다른 글자(별칭 등)가 오면 거기서 멈춘다. 이름이 없으면 nullopt.
std::optional<QualifiedName> parseQualified(const QString &text);
} // namespace SqlIdentifiers
