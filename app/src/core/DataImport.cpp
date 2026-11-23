#include "DataImport.h"

#include "ResultExport.h"
#include "SqlIdentifiers.h"
#include "TextFile.h"

#include <QCborValue>
#include <QDate>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include <QtCore/private/qzipreader_p.h>

#include <cmath>

namespace {

// ---------------------------------------------------------------- xlsx

QString readText(QXmlStreamReader &xml)
{
    return xml.readElementText(QXmlStreamReader::IncludeChildElements);
}

QStringList sharedStrings(const QByteArray &data)
{
    QStringList strings;
    QXmlStreamReader xml(data);
    QString current;
    bool inItem = false;
    int phonetic = 0; // <rPh> 안의 읽기(후리가나)는 값이 아니다
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == u"si") {
                inItem = true;
                current.clear();
            } else if (xml.name() == u"rPh") {
                ++phonetic;
            } else if (xml.name() == u"t" && inItem && phonetic == 0) {
                current += xml.readElementText();
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == u"si") {
                strings << current;
                inItem = false;
            } else if (xml.name() == u"rPh") {
                --phonetic;
            }
        }
    }
    return strings;
}

bool isDateFormat(int id, const QString &code)
{
    if ((id >= 14 && id <= 22) || (id >= 45 && id <= 47))
        return true;
    if (code.isEmpty())
        return false;
    // 따옴표 안 글자와 [색] 같은 꾸밈을 빼고 날짜·시간 기호가 있는지 본다
    QString plain;
    bool quoted = false;
    int bracket = 0;
    for (const QChar c : code) {
        if (c == u'"')
            quoted = !quoted;
        else if (!quoted && c == u'[')
            ++bracket;
        else if (!quoted && c == u']')
            bracket = qMax(0, bracket - 1);
        else if (!quoted && bracket == 0 && c != u'\\')
            plain += c.toLower();
    }
    return plain.contains(u'y') || plain.contains(u'd') || plain.contains(u'h') || plain.contains(u's');
}

// 셀 서식 번호 → 날짜 서식인지
QList<bool> dateStyles(const QByteArray &data)
{
    QHash<int, QString> customFormats;
    QList<bool> styles;
    QXmlStreamReader xml(data);
    bool inCellXfs = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == u"numFmt") {
                customFormats.insert(xml.attributes().value(u"numFmtId").toInt(), xml.attributes().value(u"formatCode").toString());
            } else if (xml.name() == u"cellXfs") {
                inCellXfs = true;
            } else if (inCellXfs && xml.name() == u"xf") {
                const int id = xml.attributes().value(u"numFmtId").toInt();
                styles << isDateFormat(id, customFormats.value(id));
            }
        } else if (xml.isEndElement() && xml.name() == u"cellXfs") {
            inCellXfs = false;
        }
    }
    return styles;
}

int columnIndex(QStringView reference)
{
    int column = 0;
    for (const QChar c : reference) {
        if (!c.isLetter())
            break;
        column = column * 26 + (c.toUpper().unicode() - u'A' + 1);
    }
    return column - 1;
}

QString numberText(double value)
{
    if (std::floor(value) == value && std::fabs(value) < 1e15)
        return QString::number(qint64(value));
    return QString::number(value, 'g', 15); // 엑셀은 15자리까지 정확하다 (0.30000000000000004 → 0.3)
}

QList<QStringList> sheetRows(const QByteArray &data, const QStringList &strings, const QList<bool> &dates, bool date1904)
{
    QList<QStringList> rows;
    QXmlStreamReader xml(data);
    QStringList row;
    int rowNumber = 0;
    bool inRow = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == u"row") {
            const int number = xml.attributes().value(u"r").toInt();
            // 건너뛴 줄은 빈 줄로 채운다 (줄 번호를 맞춰 둔다)
            while (number > 0 && rowNumber + 1 < number) {
                rows.append(QStringList());
                ++rowNumber;
            }
            rowNumber = number > 0 ? number : rowNumber + 1;
            row.clear();
            inRow = true;
        } else if (xml.isEndElement() && xml.name() == u"row") {
            rows.append(row);
            inRow = false;
        } else if (inRow && xml.isStartElement() && xml.name() == u"c") {
            const QXmlStreamAttributes attributes = xml.attributes();
            const QString type = attributes.value(u"t").toString();
            const int style = attributes.value(u"s").toInt();
            int column = attributes.hasAttribute(u"r") ? columnIndex(attributes.value(u"r")) : int(row.size());
            QString value;
            QString raw;
            while (!(xml.isEndElement() && xml.name() == u"c") && !xml.atEnd()) {
                xml.readNext();
                if (xml.isStartElement() && xml.name() == u"v")
                    raw = xml.readElementText();
                else if (xml.isStartElement() && xml.name() == u"is")
                    raw = readText(xml);
            }
            if (type == u"s") {
                value = strings.value(raw.toInt());
            } else if (type == u"inlineStr" || type == u"str") {
                value = raw;
            } else if (type == u"b") {
                value = raw == u"1" ? QStringLiteral("1") : QStringLiteral("0");
            } else if (type == u"e") {
                value.clear(); // #N/A 같은 오류 값
            } else if (!raw.isEmpty()) {
                bool ok = false;
                const double number = raw.toDouble(&ok);
                if (ok && style >= 0 && style < dates.size() && dates[style])
                    value = DataImport::excelDate(number, date1904);
                else
                    value = ok ? numberText(number) : raw;
            }
            if (column < 0)
                column = int(row.size());
            while (row.size() < column)
                row.append(QString());
            if (row.size() == column)
                row.append(value);
            else
                row[column] = value;
        }
    }
    return rows;
}

// ---------------------------------------------------------------- 값

const QRegularExpression &numberPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^[+-]?(\\d+(\\.\\d*)?|\\.\\d+)([eE][+-]?\\d+)?$"));
    return pattern;
}

// 숫자로 보이면 쉼표(천 단위)를 뺀 숫자, 아니면 빈 값
QString numericValue(const QString &text)
{
    if (numberPattern().match(text).hasMatch())
        return text;
    static const QRegularExpression grouped(QStringLiteral("^[+-]?\\d{1,3}(,\\d{3})+(\\.\\d+)?$"));
    if (grouped.match(text).hasMatch())
        return QString(text).remove(u',');
    return {};
}

QString identifier(const QString &header, int index, SqlDialect dialect)
{
    QString name = header.trimmed();
    name.replace(QRegularExpression(QStringLiteral("[\\s\\-./()]+")), QStringLiteral("_"));
    name.remove(QRegularExpression(QStringLiteral("[^\\w]"), QRegularExpression::UseUnicodePropertiesOption));
    if (name.isEmpty() || name.front().isDigit())
        name = QStringLiteral("COLUMN_%1").arg(index + 1) + (name.isEmpty() ? QString() : u'_' + name);
    switch (dialect) {
    case SqlDialect::Oracle:
    case SqlDialect::Generic:
        return name.toUpper();
    case SqlDialect::PostgreSql:
        return name.toLower();
    case SqlDialect::MySql:
        return name;
    }
    return name;
}

} // namespace

QString DataImport::excelDate(double serial, bool date1904)
{
    const QDate base = date1904 ? QDate(1904, 1, 1) : QDate(1899, 12, 30);
    double days = std::floor(serial);
    // 엑셀은 1900년을 윤년으로 잘못 다룬다: 1900-03-01 이전(일련번호 60 이하)은 하루 당긴다
    if (!date1904 && serial < 61 && serial >= 1)
        days += 1;
    const qint64 ms = qRound64((serial - std::floor(serial)) * 86400000.0);
    QDateTime when(base.addDays(qint64(days)), QTime(0, 0));
    when = when.addMSecs(ms);
    if (serial < 1 && serial > 0) // 시간만
        return when.time().toString(when.time().msec() ? QStringLiteral("HH:mm:ss.zzz") : QStringLiteral("HH:mm:ss"));
    if (when.time() == QTime(0, 0))
        return when.date().toString(Qt::ISODate);
    return when.toString(when.time().msec() ? QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz") : QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString DataImport::normalizeDate(const QString &text)
{
    // yyyy-M-d, yyyy/MM/dd, yyyy.MM.dd (두 구분자가 같아야 한다), yyyyMMdd (8자리) + 선택: 시:분[:초[.소수]]
    static const QRegularExpression pattern(QStringLiteral(
        "^(\\d{4})(?:([-/.])(\\d{1,2})\\2(\\d{1,2})|(\\d{2})(\\d{2}))"
        "(?:[ T](\\d{1,2}):(\\d{2})(?::(\\d{2})(?:\\.(\\d{1,9}))?)?)?$"));
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch())
        return {};
    const bool separated = !match.captured(2).isEmpty();
    const QDate date(match.captured(1).toInt(), (separated ? match.captured(3) : match.captured(5)).toInt(),
                     (separated ? match.captured(4) : match.captured(6)).toInt());
    if (!date.isValid())
        return {};
    if (match.captured(7).isEmpty())
        return date.toString(Qt::ISODate);
    const QTime time(match.captured(7).toInt(), match.captured(8).toInt(), match.captured(9).toInt());
    if (!time.isValid())
        return {};
    QString out = date.toString(Qt::ISODate) + u' ' + time.toString(QStringLiteral("HH:mm:ss"));
    if (!match.captured(10).isEmpty())
        out += u'.' + match.captured(10);
    return out;
}

bool DataImport::readXlsx(const QString &path, QList<ImportSheet> *sheets, QString *error)
{
    QZipReader zip(path);
    if (!zip.isReadable() || zip.status() != QZipReader::NoError) {
        if (error)
            *error = QStringLiteral("Not a readable .xlsx file.");
        return false;
    }
    const QByteArray workbook = zip.fileData(QStringLiteral("xl/workbook.xml"));
    if (workbook.isEmpty()) {
        if (error)
            *error = QStringLiteral("The file has no workbook (is it an .xlsx file?).");
        return false;
    }

    // 시트 이름과 관계 id, 날짜 체계
    QList<QPair<QString, QString>> sheetIds;
    bool date1904 = false;
    {
        QXmlStreamReader xml(workbook);
        xml.setNamespaceProcessing(false);
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement())
                continue;
            if (xml.name() == u"sheet")
                sheetIds.append({xml.attributes().value(u"name").toString(), xml.attributes().value(u"r:id").toString()});
            else if (xml.name() == u"workbookPr")
                date1904 = xml.attributes().value(u"date1904") == u"1" || xml.attributes().value(u"date1904") == u"true";
        }
    }
    QHash<QString, QString> targets;
    {
        QXmlStreamReader xml(zip.fileData(QStringLiteral("xl/_rels/workbook.xml.rels")));
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == u"Relationship") {
                QString target = xml.attributes().value(u"Target").toString();
                target = target.startsWith(u'/') ? target.mid(1) : QStringLiteral("xl/") + target;
                targets.insert(xml.attributes().value(u"Id").toString(), target);
            }
        }
    }
    const QStringList strings = sharedStrings(zip.fileData(QStringLiteral("xl/sharedStrings.xml")));
    const QList<bool> dates = dateStyles(zip.fileData(QStringLiteral("xl/styles.xml")));

    sheets->clear();
    for (const auto &[name, id] : std::as_const(sheetIds)) {
        const QByteArray data = zip.fileData(targets.value(id));
        if (data.isEmpty())
            continue;
        sheets->append({name, sheetRows(data, strings, dates, date1904)});
    }
    if (sheets->isEmpty() && error)
        *error = QStringLiteral("No sheets found.");
    return !sheets->isEmpty();
}

QChar DataImport::detectDelimiter(const QString &text)
{
    // 첫 몇 줄에서 따옴표 밖의 구분자를 센다. 줄마다 같은 수로 나오는 것을 고른다.
    const QList<QChar> candidates{u'\t', u',', u';', u'|'};
    QHash<QChar, QList<int>> counts;
    int lines = 0;
    bool quoted = false;
    QHash<QChar, int> current;
    for (int i = 0; i < text.size() && lines < 20; ++i) {
        const QChar c = text[i];
        if (c == u'"') {
            quoted = !quoted;
        } else if (!quoted && c == u'\n') {
            for (QChar d : candidates)
                counts[d].append(current.value(d));
            current.clear();
            ++lines;
        } else if (!quoted && candidates.contains(c)) {
            current[c]++;
        }
    }
    if (!current.isEmpty())
        for (QChar d : candidates)
            counts[d].append(current.value(d));
    QChar best = u',';
    int bestScore = 0;
    for (QChar d : candidates) {
        const QList<int> list = counts.value(d);
        if (list.isEmpty() || list.first() == 0)
            continue;
        bool consistent = true;
        for (int n : list)
            consistent = consistent && (n == list.first());
        const int score = list.first() * (consistent ? 2 : 1) + (d == u'\t' ? 1 : 0);
        if (score > bestScore) {
            bestScore = score;
            best = d;
        }
    }
    return best;
}

QList<QStringList> DataImport::parseDelimited(const QString &text, QChar delimiter)
{
    QList<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false;
    bool fieldStarted = false;
    const int n = int(text.size());
    for (int i = 0; i < n; ++i) {
        const QChar c = text[i];
        if (quoted) {
            if (c == u'"') {
                if (i + 1 < n && text[i + 1] == u'"') {
                    field += u'"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == u'"' && !fieldStarted) {
            quoted = true;
            fieldStarted = true;
        } else if (c == delimiter) {
            row << field;
            field.clear();
            fieldStarted = false;
        } else if (c == u'\n') {
            row << field;
            rows << row;
            row.clear();
            field.clear();
            fieldStarted = false;
        } else if (c != u'\r') {
            field += c;
            fieldStarted = true;
        }
    }
    if (fieldStarted || !field.isEmpty() || !row.isEmpty()) {
        row << field;
        rows << row;
    }
    return rows;
}

bool DataImport::readDelimited(const QString &path, ImportSheet *sheet, QString *error)
{
    QString text;
    TextFileFormat format;
    if (!TextFile::read(path, &text, &format, error))
        return false;
    sheet->name = QFileInfo(path).fileName();
    sheet->rows = parseDelimited(text, detectDelimiter(text));
    return true;
}

bool DataImport::read(const QString &path, QList<ImportSheet> *sheets, QString *error)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == u"xlsx" || suffix == u"xlsm")
        return readXlsx(path, sheets, error);
    if (suffix == u"xls") {
        if (error)
            *error = QStringLiteral("Old .xls files are not supported. Save the file as .xlsx or .csv in Excel first.");
        return false;
    }
    ImportSheet sheet;
    if (!readDelimited(path, &sheet, error))
        return false;
    sheets->clear();
    sheets->append(sheet);
    return true;
}

QList<int> DataImport::autoMap(const QList<ImportColumn> &target, const QStringList &header, bool hasHeader, int fileColumns)
{
    auto key = [](QString name) {
        name = name.toLower();
        name.remove(QRegularExpression(QStringLiteral("[\\s_\\-]")));
        return name;
    };
    QList<int> mapping;
    QSet<int> used;
    for (int i = 0; i < target.size(); ++i) {
        int found = -1;
        if (hasHeader) {
            for (int c = 0; c < header.size(); ++c) {
                if (!used.contains(c) && key(header[c]) == key(target[i].name)) {
                    found = c;
                    break;
                }
            }
        } else if (i < fileColumns) {
            found = i;
        }
        if (found >= 0)
            used.insert(found);
        mapping.append(found);
    }
    return mapping;
}

QStringList DataImport::insertStatements(const QString &table, const QList<ImportColumn> &columns, const QList<int> &mapping,
                                         const QList<QStringList> &rows, const ImportOptions &options, SqlDialect dialect)
{
    QStringList names;
    QList<int> targets;
    for (int i = 0; i < columns.size() && i < mapping.size(); ++i) {
        if (mapping[i] < 0)
            continue;
        names << SqlIdentifiers::quote(columns[i].name, dialect);
        targets << i;
    }
    if (targets.isEmpty())
        return {};
    const QString head = QStringLiteral("INSERT INTO %1 (%2) VALUES (").arg(table, names.join(QStringLiteral(", ")));

    QStringList out;
    out.reserve(rows.size());
    for (const QStringList &row : rows) {
        bool empty = true;
        for (const QString &cell : row)
            empty = empty && cell.trimmed().isEmpty();
        if (empty)
            continue; // 빈 줄

        QStringList values;
        for (int i : std::as_const(targets)) {
            const ImportColumn &column = columns[i];
            QString text = row.value(mapping[i]);
            if (options.trim)
                text = text.trimmed();
            QCborValue value;
            if (text.isEmpty() && options.emptyAsNull) {
                value = QCborValue(QCborValue::Null);
            } else if (column.numeric) {
                const QString number = numericValue(text);
                value = number.isEmpty() ? QCborValue(text) : QCborValue(number);
            } else if (column.temporal) {
                const QString date = normalizeDate(text);
                value = QCborValue(date.isEmpty() ? text : date);
            } else {
                value = QCborValue(text);
            }
            values << ResultExport::sqlLiteral({column.name, column.typeName, column.numeric}, value, dialect);
        }
        out << head + values.join(QStringLiteral(", ")) + u')';
    }
    return out;
}

QList<ImportColumn> DataImport::guessColumns(const QStringList &header, const QList<QStringList> &rows, SqlDialect dialect)
{
    int count = int(header.size());
    for (const QStringList &row : rows)
        count = qMax(count, int(row.size()));

    QList<ImportColumn> columns;
    QSet<QString> used;
    for (int c = 0; c < count; ++c) {
        bool numeric = true, integer = true, dates = true, times = false, any = false;
        int length = 1, scale = 0, digits = 1;
        for (const QStringList &row : rows) {
            const QString text = row.value(c).trimmed();
            if (text.isEmpty())
                continue;
            any = true;
            length = qMax(length, int(text.size()));
            const QString number = numericValue(text);
            if (number.isEmpty() || number.contains(u'e') || number.contains(u'E')) {
                numeric = false;
            } else {
                const qsizetype dot = number.indexOf(u'.');
                if (dot >= 0) {
                    integer = false;
                    scale = qMax(scale, int(number.size() - dot - 1));
                }
                digits = qMax(digits, int((dot >= 0 ? dot : number.size()) - (number.startsWith(u'-') || number.startsWith(u'+') ? 1 : 0)));
            }
            const QString date = normalizeDate(text);
            if (date.isEmpty() || (numeric && !text.contains(u'-') && !text.contains(u'/') && !text.contains(u'.')))
                dates = false;
            else if (date.size() > 10)
                times = true;
        }
        if (!any)
            numeric = dates = false;

        ImportColumn column;
        QString name = identifier(header.value(c), c, dialect);
        for (int n = 2; used.contains(name.toUpper()); ++n)
            name = identifier(header.value(c), c, dialect) + QStringLiteral("_%1").arg(n);
        used.insert(name.toUpper());
        column.name = name;
        const int size = qBound(10, ((length + 9) / 10) * 10, 4000);
        if (dates) {
            column.temporal = true;
            switch (dialect) {
            case SqlDialect::Oracle: column.typeName = times ? QStringLiteral("TIMESTAMP") : QStringLiteral("DATE"); break;
            case SqlDialect::MySql: column.typeName = times ? QStringLiteral("DATETIME") : QStringLiteral("DATE"); break;
            default: column.typeName = times ? QStringLiteral("TIMESTAMP") : QStringLiteral("DATE"); break;
            }
        } else if (numeric) {
            column.numeric = true;
            const int precision = qMin(38, digits + scale + 2);
            switch (dialect) {
            case SqlDialect::Oracle:
                column.typeName = integer ? QStringLiteral("NUMBER(%1)").arg(qMax(precision, 10)) : QStringLiteral("NUMBER(%1,%2)").arg(qMax(precision, 10)).arg(scale);
                break;
            default:
                column.typeName = integer ? (digits > 9 ? QStringLiteral("BIGINT") : QStringLiteral("INTEGER"))
                                          : QStringLiteral("DECIMAL(%1,%2)").arg(qMax(precision, 10)).arg(scale);
                break;
            }
        } else {
            column.typeName = dialect == SqlDialect::Oracle ? QStringLiteral("VARCHAR2(%1 CHAR)").arg(size)
                                                            : QStringLiteral("VARCHAR(%1)").arg(size);
        }
        columns.append(column);
    }
    return columns;
}

QString DataImport::createTable(const QString &table, const QList<ImportColumn> &columns, SqlDialect dialect)
{
    QStringList lines;
    for (const ImportColumn &column : columns)
        lines << QStringLiteral("    %1 %2").arg(SqlIdentifiers::quote(column.name, dialect), column.typeName);
    return QStringLiteral("CREATE TABLE %1 (\n%2\n)").arg(table, lines.join(QStringLiteral(",\n")));
}
