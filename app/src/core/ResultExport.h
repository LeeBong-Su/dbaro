#pragma once

#include "SqlDialect.h"

#include <QByteArray>
#include <QCborArray>
#include <QList>
#include <QString>

struct ExportColumn
{
    QString name;
    QString typeName;
    bool numeric = false;
};

struct ExportOptions
{
    enum class Format { Csv, Tsv, Excel, SqlInsert, Json, Markdown };

    Format format = Format::Csv;
    bool header = true;   // CSV·TSV·Excel: 첫 줄에 컬럼 이름
    bool bom = true;      // CSV·TSV: UTF-8 BOM (엑셀에서 한글이 깨지지 않게)
    QString nullText;     // CSV·TSV·Markdown에서 NULL을 쓸 글 (기본은 빈 칸)
    QString tableName;    // SQL INSERT 대상
    SqlDialect dialect = SqlDialect::Generic;
};

// 결과 표를 파일 형식으로 바꾼다. rows의 각 행은 columns와 같은 순서의 값이다.
// - CSV: RFC 4180 (쉼표, 필요할 때만 큰따옴표, 줄 끝 CRLF)
// - TSV: 탭으로 구분, 값 안의 탭·줄바꿈은 공백으로
// - Excel: .xlsx (첫 줄 굵게·고정, 숫자 컬럼은 숫자 셀. 15자리가 넘는 숫자는 정밀도를 잃지 않게 글자로)
// - SQL INSERT: 행마다 INSERT 문. Oracle 날짜는 TO_DATE/TO_TIMESTAMP, 바이너리는 DB별 16진 표기
// - JSON: 객체 배열 (컬럼 순서 유지, 숫자는 숫자로)
// - Markdown: 표
namespace ResultExport {

QByteArray build(const QList<ExportColumn> &columns, const QList<QCborArray> &rows, const ExportOptions &options);

QString formatName(ExportOptions::Format format);
QString extension(ExportOptions::Format format);
bool isBinary(ExportOptions::Format format); // 클립보드로 보낼 수 없는 형식 (Excel)

// 표의 값 하나를 글로 (NULL은 빈 문자열)
QString valueText(const QCborValue &value);

// 값을 SQL 문장에 넣을 글로 (NULL, 숫자 컬럼은 숫자 그대로, Oracle 날짜는 TO_DATE/TO_TIMESTAMP, 바이너리는 16진)
QString sqlLiteral(const ExportColumn &column, const QCborValue &value, SqlDialect dialect);

} // namespace ResultExport
