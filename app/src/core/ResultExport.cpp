#include "ResultExport.h"

#include "SqlIdentifiers.h"

#include <QDateTime>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <cmath>

namespace {

bool isNull(const QCborValue &value)
{
    return value.isNull() || value.isUndefined();
}

const QRegularExpression &numberPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^[+-]?(\\d+(\\.\\d*)?|\\.\\d+)([eE][+-]?\\d+)?$"));
    return pattern;
}

// 숫자 컬럼의 값을 그대로 숫자로 쓸 수 있으면 그 글, 아니면 빈 값
QString numericText(const ExportColumn &column, const QCborValue &value)
{
    if (value.isInteger())
        return QString::number(value.toInteger());
    if (value.isDouble()) {
        const double d = value.toDouble();
        return std::isfinite(d) ? QString::number(d, 'g', QLocale::FloatingPointShortest) : QString();
    }
    if (column.numeric && value.isString() && numberPattern().match(value.toString()).hasMatch())
        return value.toString();
    return {};
}

// ---------------------------------------------------------------- CSV, TSV, Markdown

QString csvField(const QString &text)
{
    const bool quote = text.contains(u',') || text.contains(u'"') || text.contains(u'\n') || text.contains(u'\r')
                       || (!text.isEmpty() && (text.front().isSpace() || text.back().isSpace()));
    if (!quote)
        return text;
    QString escaped = text;
    escaped.replace(u'"', QStringLiteral("\"\""));
    return u'"' + escaped + u'"';
}

QString flat(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral(" "));
    text.replace(u'\t', u' ').replace(u'\n', u' ').replace(u'\r', u' ');
    return text;
}

QByteArray delimited(const QList<ExportColumn> &columns, const QList<QCborArray> &rows, const ExportOptions &options,
                     bool csv)
{
    QString out;
    auto field = [&](const QString &text) { return csv ? csvField(text) : flat(text); };
    const QChar separator = csv ? u',' : u'\t';
    if (options.header) {
        QStringList names;
        for (const ExportColumn &column : columns)
            names << field(column.name);
        out += names.join(separator) + QStringLiteral("\r\n");
    }
    for (const QCborArray &row : rows) {
        QStringList values;
        for (int c = 0; c < columns.size(); ++c) {
            const QCborValue value = row.at(c);
            values << field(isNull(value) ? options.nullText : ResultExport::valueText(value));
        }
        out += values.join(separator) + QStringLiteral("\r\n");
    }
    QByteArray bytes = options.bom ? QByteArray("\xEF\xBB\xBF") : QByteArray();
    return bytes + out.toUtf8();
}

QByteArray markdown(const QList<ExportColumn> &columns, const QList<QCborArray> &rows, const ExportOptions &options)
{
    auto cell = [](QString text) {
        text.replace(u'|', QStringLiteral("\\|"));
        text.replace(QStringLiteral("\r\n"), QStringLiteral("<br>")).replace(u'\n', QStringLiteral("<br>")).replace(u'\r', u' ');
        return text;
    };
    QStringList names, rule;
    for (const ExportColumn &column : columns) {
        names << cell(column.name);
        rule << (column.numeric ? QStringLiteral("---:") : QStringLiteral("---"));
    }
    QString out = QStringLiteral("| %1 |\n| %2 |\n").arg(names.join(QStringLiteral(" | ")), rule.join(QStringLiteral(" | ")));
    for (const QCborArray &row : rows) {
        QStringList values;
        for (int c = 0; c < columns.size(); ++c) {
            const QCborValue value = row.at(c);
            values << cell(isNull(value) ? options.nullText : ResultExport::valueText(value));
        }
        out += QStringLiteral("| %1 |\n").arg(values.join(QStringLiteral(" | ")));
    }
    return out.toUtf8();
}

// ---------------------------------------------------------------- JSON

QString jsonString(const QString &text)
{
    QString out = QStringLiteral("\"");
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '"': out += QStringLiteral("\\\""); break;
        case '\\': out += QStringLiteral("\\\\"); break;
        case '\n': out += QStringLiteral("\\n"); break;
        case '\r': out += QStringLiteral("\\r"); break;
        case '\t': out += QStringLiteral("\\t"); break;
        default:
            if (c.unicode() < 0x20)
                out += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
            else
                out += c;
        }
    }
    return out + u'"';
}

QByteArray json(const QList<ExportColumn> &columns, const QList<QCborArray> &rows)
{
    // 같은 이름의 컬럼(조인 결과 등)은 키가 겹치지 않게 _2, _3을 붙인다.
    QStringList keys;
    QSet<QString> used;
    for (const ExportColumn &column : columns) {
        QString key = column.name;
        for (int n = 2; used.contains(key); ++n)
            key = QStringLiteral("%1_%2").arg(column.name).arg(n);
        used.insert(key);
        keys << jsonString(key);
    }
    QString out = QStringLiteral("[");
    for (int r = 0; r < rows.size(); ++r) {
        QStringList members;
        for (int c = 0; c < columns.size(); ++c) {
            const QCborValue value = rows[r].at(c);
            QString text;
            if (isNull(value))
                text = QStringLiteral("null");
            else if (value.isBool())
                text = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
            else if (const QString number = numericText(columns[c], value); !number.isEmpty())
                text = number;
            else
                text = jsonString(ResultExport::valueText(value));
            members << keys[c] + QStringLiteral(": ") + text;
        }
        out += (r == 0 ? QStringLiteral("\n  {") : QStringLiteral(",\n  {")) + members.join(QStringLiteral(", ")) + u'}';
    }
    out += rows.isEmpty() ? QStringLiteral("]\n") : QStringLiteral("\n]\n");
    return out.toUtf8();
}

// ---------------------------------------------------------------- SQL INSERT

QString sqlString(const QString &text, SqlDialect dialect)
{
    QString escaped = text;
    if (dialect == SqlDialect::MySql)
        escaped.replace(u'\\', QStringLiteral("\\\\"));
    escaped.replace(u'\'', QStringLiteral("''"));
    return u'\'' + escaped + u'\'';
}

QString sqlLiteral(const ExportColumn &column, const QCborValue &value, SqlDialect dialect)
{
    if (isNull(value))
        return QStringLiteral("NULL");
    if (value.isBool()) {
        if (dialect == SqlDialect::Oracle)
            return value.toBool() ? QStringLiteral("1") : QStringLiteral("0");
        return value.toBool() ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
    }
    if (value.isByteArray()) {
        const QString hex = QString::fromLatin1(value.toByteArray().toHex().toUpper());
        switch (dialect) {
        case SqlDialect::Oracle: return QStringLiteral("HEXTORAW('%1')").arg(hex);
        case SqlDialect::PostgreSql: return QStringLiteral("decode('%1', 'hex')").arg(hex);
        default: return QStringLiteral("X'%1'").arg(hex);
        }
    }
    if (const QString number = numericText(column, value); !number.isEmpty())
        return number;

    const QString text = ResultExport::valueText(value);
    if (dialect == SqlDialect::Oracle) {
        static const QRegularExpression dateTime(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}( \\d{2}:\\d{2}:\\d{2}(\\.\\d+)?)?$"));
        const QString type = column.typeName.toUpper();
        if ((type == u"DATE" || type.startsWith(u"TIMESTAMP")) && dateTime.match(text).hasMatch()) {
            if (text.size() == 10)
                return QStringLiteral("TO_DATE('%1', 'YYYY-MM-DD')").arg(text);
            if (type == u"DATE" || !text.contains(u'.'))
                return QStringLiteral("%1('%2', 'YYYY-MM-DD HH24:MI:SS')")
                    .arg(type == u"DATE" ? QStringLiteral("TO_DATE") : QStringLiteral("TO_TIMESTAMP"), text.left(19));
            return QStringLiteral("TO_TIMESTAMP('%1', 'YYYY-MM-DD HH24:MI:SS.FF')").arg(text);
        }
    }
    return sqlString(text, dialect);
}

QByteArray inserts(const QList<ExportColumn> &columns, const QList<QCborArray> &rows, const ExportOptions &options)
{
    const QString table = options.tableName.trimmed().isEmpty() ? QStringLiteral("TABLE_NAME") : options.tableName.trimmed();
    QStringList names;
    for (const ExportColumn &column : columns)
        names << SqlIdentifiers::quote(column.name, options.dialect);
    const QString head = QStringLiteral("INSERT INTO %1 (%2) VALUES (").arg(table, names.join(QStringLiteral(", ")));
    QString out;
    for (const QCborArray &row : rows) {
        QStringList values;
        for (int c = 0; c < columns.size(); ++c)
            values << sqlLiteral(columns[c], row.at(c), options.dialect);
        out += head + values.join(QStringLiteral(", ")) + QStringLiteral(");\n");
    }
    return out.toUtf8();
}

// ---------------------------------------------------------------- Excel (.xlsx = ZIP 안의 XML)

quint32 crc32(const QByteArray &data)
{
    static quint32 table[256];
    static bool ready = false;
    if (!ready) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    quint32 crc = 0xFFFFFFFFu;
    for (const char byte : data)
        crc = table[(crc ^ quint8(byte)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

class ZipWriter
{
public:
    void add(const QString &name, const QByteArray &data)
    {
        // qCompress = 4바이트 길이 + zlib(2바이트 머리 + deflate + 4바이트 adler32). ZIP에는 deflate만 넣는다.
        const QByteArray zlib = qCompress(data, 6);
        const QByteArray deflated = zlib.mid(6, zlib.size() - 10);
        const QByteArray fileName = name.toUtf8();
        const quint32 crc = crc32(data);
        const quint32 offset = quint32(m_out.size());

        const QDateTime now = QDateTime::currentDateTime();
        const quint16 dosTime = quint16((now.time().hour() << 11) | (now.time().minute() << 5) | (now.time().second() / 2));
        const quint16 dosDate = quint16(((now.date().year() - 1980) << 9) | (now.date().month() << 5) | now.date().day());

        auto header = [&](bool central) {
            QByteArray h;
            put32(h, central ? 0x02014b50u : 0x04034b50u);
            if (central)
                put16(h, 20); // 만든 버전
            put16(h, 20);     // 필요한 버전
            put16(h, 0x0800); // 파일 이름은 UTF-8
            put16(h, 8);      // deflate
            put16(h, dosTime);
            put16(h, dosDate);
            put32(h, crc);
            put32(h, quint32(deflated.size()));
            put32(h, quint32(data.size()));
            put16(h, quint16(fileName.size()));
            put16(h, 0); // extra
            if (central) {
                put16(h, 0); // comment
                put16(h, 0); // disk
                put16(h, 0); // internal attributes
                put32(h, 0); // external attributes
                put32(h, offset);
            }
            return h + fileName;
        };
        m_out += header(false) + deflated;
        m_central += header(true);
        ++m_count;
    }

    QByteArray finish()
    {
        const quint32 centralOffset = quint32(m_out.size());
        QByteArray end;
        put32(end, 0x06054b50u);
        put16(end, 0);
        put16(end, 0);
        put16(end, quint16(m_count));
        put16(end, quint16(m_count));
        put32(end, quint32(m_central.size()));
        put32(end, centralOffset);
        put16(end, 0);
        return m_out + m_central + end;
    }

private:
    static void put16(QByteArray &out, quint16 value)
    {
        char bytes[2];
        qToLittleEndian(value, bytes);
        out.append(bytes, 2);
    }
    static void put32(QByteArray &out, quint32 value)
    {
        char bytes[4];
        qToLittleEndian(value, bytes);
        out.append(bytes, 4);
    }

    QByteArray m_out;
    QByteArray m_central;
    int m_count = 0;
};

QString xmlText(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        const ushort u = c.unicode();
        if (u < 0x20 && u != '\t' && u != '\n' && u != '\r')
            continue; // XML에 넣을 수 없는 제어 문자
        switch (u) {
        case '&': out += QStringLiteral("&amp;"); break;
        case '<': out += QStringLiteral("&lt;"); break;
        case '>': out += QStringLiteral("&gt;"); break;
        case '"': out += QStringLiteral("&quot;"); break;
        default: out += c;
        }
    }
    return out;
}

QString columnLetters(int column)
{
    QString letters;
    for (int n = column + 1; n > 0; n = (n - 1) / 26)
        letters.prepend(QChar(u'A' + (n - 1) % 26));
    return letters;
}

QByteArray xlsx(const QList<ExportColumn> &columns, const QList<QCborArray> &rows, const ExportOptions &options)
{
    constexpr int kMaxRows = 1048576;
    constexpr int kMaxCellText = 32767;
    const QString xmlHead = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    const QString mainNs = QStringLiteral("http://schemas.openxmlformats.org/spreadsheetml/2006/main");
    const QString relNs = QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships");

    // 컬럼 폭: 이름과 앞쪽 값 길이로 어림한다 (한글은 두 칸).
    QList<int> widths;
    for (int c = 0; c < columns.size(); ++c) {
        auto width = [](const QString &text) {
            int w = 0;
            for (const QChar ch : text.left(80))
                w += ch.unicode() >= 0x1100 ? 2 : 1;
            return w;
        };
        int w = width(columns[c].name);
        for (int r = 0; r < qMin<qsizetype>(rows.size(), 200); ++r)
            w = qMax(w, width(ResultExport::valueText(rows[r].at(c))));
        widths << qBound(6, w + 2, 60);
    }

    QString sheet = xmlHead + QStringLiteral("<worksheet xmlns=\"%1\" xmlns:r=\"%2\">").arg(mainNs, relNs);
    if (options.header)
        sheet += QStringLiteral("<sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"1\" topLeftCell=\"A2\" "
                                "activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews>");
    if (!columns.isEmpty()) {
        sheet += QStringLiteral("<cols>");
        for (int c = 0; c < columns.size(); ++c)
            sheet += QStringLiteral("<col min=\"%1\" max=\"%1\" width=\"%2\" customWidth=\"1\"/>").arg(c + 1).arg(widths[c]);
        sheet += QStringLiteral("</cols>");
    }
    sheet += QStringLiteral("<sheetData>");

    auto textCell = [&](const QString &ref, const QString &text, bool bold) {
        return QStringLiteral("<c r=\"%1\" t=\"inlineStr\"%2><is><t xml:space=\"preserve\">%3</t></is></c>")
            .arg(ref, bold ? QStringLiteral(" s=\"1\"") : QString(), xmlText(text.left(kMaxCellText)));
    };
    int excelRow = 0;
    if (options.header) {
        ++excelRow;
        sheet += QStringLiteral("<row r=\"%1\">").arg(excelRow);
        for (int c = 0; c < columns.size(); ++c)
            sheet += textCell(columnLetters(c) + QString::number(excelRow), columns[c].name, true);
        sheet += QStringLiteral("</row>");
    }
    for (const QCborArray &row : rows) {
        if (excelRow >= kMaxRows)
            break;
        ++excelRow;
        sheet += QStringLiteral("<row r=\"%1\">").arg(excelRow);
        for (int c = 0; c < columns.size(); ++c) {
            const QCborValue value = row.at(c);
            if (isNull(value))
                continue;
            const QString ref = columnLetters(c) + QString::number(excelRow);
            if (value.isBool()) {
                sheet += QStringLiteral("<c r=\"%1\" t=\"b\"><v>%2</v></c>").arg(ref, value.toBool() ? QStringLiteral("1") : QStringLiteral("0"));
                continue;
            }
            const QString number = numericText(columns[c], value);
            int digits = 0;
            for (const QChar ch : number)
                digits += ch.isDigit() ? 1 : 0;
            if (!number.isEmpty() && digits <= 15) // 엑셀 숫자는 15자리까지 정확하다
                sheet += QStringLiteral("<c r=\"%1\"><v>%2</v></c>").arg(ref, number);
            else
                sheet += textCell(ref, ResultExport::valueText(value), false);
        }
        sheet += QStringLiteral("</row>");
    }
    sheet += QStringLiteral("</sheetData></worksheet>");

    const QString contentTypes =
        xmlHead
        + QStringLiteral("<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                         "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                         "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                         "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
                         "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
                         "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
                         "</Types>");
    const QString rootRels =
        xmlHead
        + QStringLiteral("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                         "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
                         "</Relationships>");
    const QString workbook = xmlHead
                             + QStringLiteral("<workbook xmlns=\"%1\" xmlns:r=\"%2\"><sheets>"
                                              "<sheet name=\"Result\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>")
                                   .arg(mainNs, relNs);
    const QString workbookRels =
        xmlHead
        + QStringLiteral("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                         "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
                         "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                         "</Relationships>");
    const QString styles =
        xmlHead
        + QStringLiteral("<styleSheet xmlns=\"%1\">"
                         "<fonts count=\"2\"><font><sz val=\"11\"/></font><font><b/><sz val=\"11\"/></font></fonts>"
                         "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
                         "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
                         "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
                         "<cellXfs count=\"2\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
                         "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/></cellXfs>"
                         "</styleSheet>")
              .arg(mainNs);

    ZipWriter zip;
    zip.add(QStringLiteral("[Content_Types].xml"), contentTypes.toUtf8());
    zip.add(QStringLiteral("_rels/.rels"), rootRels.toUtf8());
    zip.add(QStringLiteral("xl/workbook.xml"), workbook.toUtf8());
    zip.add(QStringLiteral("xl/_rels/workbook.xml.rels"), workbookRels.toUtf8());
    zip.add(QStringLiteral("xl/styles.xml"), styles.toUtf8());
    zip.add(QStringLiteral("xl/worksheets/sheet1.xml"), sheet.toUtf8());
    return zip.finish();
}

} // namespace

QString ResultExport::valueText(const QCborValue &value)
{
    switch (value.type()) {
    case QCborValue::Null:
    case QCborValue::Undefined:
        return {};
    case QCborValue::True:
        return QStringLiteral("true");
    case QCborValue::False:
        return QStringLiteral("false");
    case QCborValue::Integer:
        return QString::number(value.toInteger());
    case QCborValue::Double:
        return QString::number(value.toDouble(), 'g', QLocale::FloatingPointShortest);
    case QCborValue::ByteArray:
        return QStringLiteral("0x") + QString::fromLatin1(value.toByteArray().toHex());
    default:
        return value.toString();
    }
}

QByteArray ResultExport::build(const QList<ExportColumn> &columns, const QList<QCborArray> &rows, const ExportOptions &options)
{
    switch (options.format) {
    case ExportOptions::Format::Csv: return delimited(columns, rows, options, true);
    case ExportOptions::Format::Tsv: return delimited(columns, rows, options, false);
    case ExportOptions::Format::Excel: return xlsx(columns, rows, options);
    case ExportOptions::Format::SqlInsert: return inserts(columns, rows, options);
    case ExportOptions::Format::Json: return json(columns, rows);
    case ExportOptions::Format::Markdown: return markdown(columns, rows, options);
    }
    return {};
}

QString ResultExport::formatName(ExportOptions::Format format)
{
    switch (format) {
    case ExportOptions::Format::Csv: return QStringLiteral("CSV");
    case ExportOptions::Format::Tsv: return QStringLiteral("TSV");
    case ExportOptions::Format::Excel: return QStringLiteral("Excel (.xlsx)");
    case ExportOptions::Format::SqlInsert: return QStringLiteral("SQL INSERT");
    case ExportOptions::Format::Json: return QStringLiteral("JSON");
    case ExportOptions::Format::Markdown: return QStringLiteral("Markdown");
    }
    return {};
}

QString ResultExport::extension(ExportOptions::Format format)
{
    switch (format) {
    case ExportOptions::Format::Csv: return QStringLiteral("csv");
    case ExportOptions::Format::Tsv: return QStringLiteral("tsv");
    case ExportOptions::Format::Excel: return QStringLiteral("xlsx");
    case ExportOptions::Format::SqlInsert: return QStringLiteral("sql");
    case ExportOptions::Format::Json: return QStringLiteral("json");
    case ExportOptions::Format::Markdown: return QStringLiteral("md");
    }
    return QStringLiteral("txt");
}

bool ResultExport::isBinary(ExportOptions::Format format)
{
    return format == ExportOptions::Format::Excel;
}
