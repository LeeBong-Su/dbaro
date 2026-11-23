#pragma once

#include "SqlDialect.h"

#include <QList>
#include <QString>
#include <QStringList>

// 엑셀(.xlsx)·CSV 파일을 테이블로 불러오기.
// - .xlsx: 시트마다 셀 값을 글로 읽는다 (공유 문자열, 숫자, 참/거짓, 날짜 서식이면 날짜로)
// - .csv/.tsv/.txt: 구분자(쉼표·탭·세미콜론·|)와 인코딩(BOM, UTF-8, CP949)을 알아서 읽는다
// - 대상 컬럼 타입에 맞춰 INSERT 문을 만든다 (숫자 컬럼의 1,234는 1234로, 날짜는 여러 표기를 yyyy-MM-dd[ HH:mm:ss]로)
// - 새 테이블이면 값으로 컬럼 타입을 짐작해 CREATE TABLE을 만든다
struct ImportSheet
{
    QString name;
    QList<QStringList> rows;
};

struct ImportColumn
{
    QString name;
    QString typeName; // DB 타입 (NUMBER, VARCHAR2(40), DATE ...)
    bool numeric = false;
    bool temporal = false;
};

struct ImportOptions
{
    bool header = true;      // 첫 줄은 컬럼 이름
    bool emptyAsNull = true; // 빈 칸은 NULL
    bool trim = true;        // 앞뒤 공백 지우기
};

namespace DataImport {

// 확장자로 형식을 고른다. 시트가 하나 이상 있으면 true.
bool read(const QString &path, QList<ImportSheet> *sheets, QString *error = nullptr);
bool readXlsx(const QString &path, QList<ImportSheet> *sheets, QString *error = nullptr);
bool readDelimited(const QString &path, ImportSheet *sheet, QString *error = nullptr);

QChar detectDelimiter(const QString &text);
QList<QStringList> parseDelimited(const QString &text, QChar delimiter);

// 엑셀 날짜 일련번호 → yyyy-MM-dd[ HH:mm:ss[.zzz]] (date1904: 1904 날짜 체계 통합 문서)
QString excelDate(double serial, bool date1904 = false);
// 흔한 날짜 표기를 yyyy-MM-dd[ HH:mm:ss[.zzz]]로. 알아볼 수 없으면 빈 값.
QString normalizeDate(const QString &text);

// 파일 첫 줄(머리글)과 대상 컬럼을 이름으로 짝짓는다 (대소문자·공백·_ 무시). 머리글이 없으면 순서대로.
// 결과[대상 컬럼] = 파일 열 위치, 짝이 없으면 -1
QList<int> autoMap(const QList<ImportColumn> &target, const QStringList &header, bool hasHeader, int fileColumns);

// 데이터 줄로 INSERT 문을 만든다. mapping[i]는 대상 컬럼 i에 넣을 파일 열 (-1이면 넣지 않음). 빈 줄은 건너뛴다.
QStringList insertStatements(const QString &table, const QList<ImportColumn> &columns, const QList<int> &mapping,
                             const QList<QStringList> &rows, const ImportOptions &options, SqlDialect dialect);

// 머리글과 값으로 새 테이블의 컬럼(이름·타입)을 짐작한다
QList<ImportColumn> guessColumns(const QStringList &header, const QList<QStringList> &rows, SqlDialect dialect);
QString createTable(const QString &table, const QList<ImportColumn> &columns, SqlDialect dialect);

} // namespace DataImport
