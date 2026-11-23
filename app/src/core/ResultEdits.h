#pragma once

#include "ResultExport.h"
#include "SqlDialect.h"
#include "SqlIdentifiers.h"

#include <QCborArray>
#include <QList>
#include <QSet>
#include <QStringList>

#include <optional>

// 결과 표를 고쳐서 DB에 반영하기.
// 결과가 테이블 하나에서 나왔고(SELECT ... FROM 테이블 [별칭] [WHERE ...] [ORDER BY ...]) 그 테이블의 기본 키
// 컬럼이 결과에 모두 있으면, 고친 셀·추가한 행·지운 행을 UPDATE·INSERT·DELETE로 바꾼다.
// 행은 기본 키의 원래 값으로 찾는다. 값은 컬럼 타입에 맞는 SQL 글로 넣는다 (ResultExport::sqlLiteral).
struct RowChange
{
    enum class Kind { Update, Insert, Delete };
    Kind kind = Kind::Update;
    QCborArray original; // 고치기 전 값 (Update, Delete)
    QCborArray current;  // 지금 값 (Update, Insert)
    QSet<int> changed;   // Update에서 고친 컬럼
};

namespace ResultEdits {

// 고칠 수 있는 결과인지: 테이블 하나에서 나온 SELECT면 그 테이블 이름
std::optional<QualifiedName> editTarget(const QString &sql, SqlDialect dialect);

// 바꾼 내용을 문장으로. table은 따옴표까지 붙인 이름, keyColumns는 기본 키 컬럼의 결과 안 위치.
// editable이 false인 컬럼(계산식 등)은 INSERT·UPDATE에 넣지 않는다. 문제가 있으면 빈 목록과 error.
QStringList statements(const QString &table, const QList<ExportColumn> &columns, const QList<bool> &editable,
                       const QList<int> &keyColumns, const QList<RowChange> &changes, SqlDialect dialect,
                       QString *error = nullptr);

} // namespace ResultEdits
