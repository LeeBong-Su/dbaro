#pragma once

#include <QString>

// 실행 전 안전 확인용 간단한 문장 분석. 주석·문자열·인용 식별자 안의 단어는 무시한다.
namespace SqlSafety {

// 첫 키워드 (대문자). WITH ... 로 시작하면 WITH 절 뒤의 본문 키워드.
QString leadingKeyword(const QString &sql);

// 데이터나 구조를 바꿀 수 있는 문장 (읽기 전용 연결에서 막는다). PL/SQL 블록과 프로시저 호출도 포함한다.
bool mayModify(const QString &sql);

// 괄호 안까지 통틀어 쓰기 키워드가 있는지 (INSERT·UPDATE·DELETE·MERGE·INTO·CREATE·DROP 등).
// WITH d AS (DELETE ... RETURNING *) SELECT ..., SELECT ... INTO 처럼 첫 키워드만으로는 모르는 쓰기를 찾는다
bool hasWriteKeyword(const QString &sql);

// 트랜잭션에 남는 데이터 변경 (INSERT, UPDATE, DELETE, MERGE, UPSERT, REPLACE)
bool isDml(const QString &sql);

// 바깥쪽(괄호 밖)에 WHERE가 없는 UPDATE·DELETE
bool isUnfilteredUpdateOrDelete(const QString &sql);

// 되돌리기 어려운 DDL (DROP, TRUNCATE)
bool isDestructiveDdl(const QString &sql);

} // namespace SqlSafety
