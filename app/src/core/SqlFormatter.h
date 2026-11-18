#pragma once

#include "SqlDialect.h"

#include <QString>

// SQL 정렬 (Ctrl+Shift+F). 국내에서 흔히 쓰는 "강줄기" 모양으로 맞춘다.
//
//   SELECT E.EMPNO
//        , E.ENAME
//     FROM EMP E
//     JOIN DEPT D
//       ON D.DEPTNO = E.DEPTNO
//    WHERE E.SAL > 1000
//      AND E.DEPTNO IN (SELECT DEPTNO
//                         FROM DEPT
//                        WHERE LOC = 'SEOUL')
//    ORDER BY E.ENAME
//
// - 절 키워드는 오른쪽 끝을 맞추고, 목록은 앞쪽 쉼표로 줄을 나눈다. AND/OR는 줄 앞에 둔다 (BETWEEN의 AND, CASE 안은 제외)
// - 괄호 안의 SELECT(부질의)는 여는 괄호 바로 뒤를 기준으로 같은 모양으로 들여 쓴다. 다른 괄호(함수, IN 목록)는 한 줄로 둔다
// - 키워드와 알려진 함수 이름은 대문자로 바꾸고, 테이블·컬럼 이름과 문자열·주석은 그대로 둔다
// - 공백과 키워드 대소문자만 바꾸므로 뜻은 바뀌지 않는다. SELECT·WITH·INSERT·UPDATE·DELETE·MERGE만 정렬하고
//   DDL과 PL/SQL은 그대로 둔다
namespace SqlFormatter {

// 문장 하나. firstColumn은 문장이 시작하는 줄의 들여쓰기 (다음 줄들도 그만큼 들여 쓴다).
QString formatStatement(const QString &sql, SqlDialect dialect, int firstColumn = 0);

// 여러 문장. 문장 사이의 구분자·주석·빈 줄은 그대로 둔다.
QString format(const QString &script, SqlDialect dialect, int firstColumn = 0);

// 정렬하는 종류의 문장인지 (SELECT, WITH, INSERT, UPDATE, DELETE, MERGE)
bool canFormat(const QString &sql, SqlDialect dialect);

} // namespace SqlFormatter
