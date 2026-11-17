#include "SqlHighlighter.h"

#include <QSet>
#include <QTextBlock>

namespace {

// 여러 줄에 걸친 $$ 본문과 q'[...]' 문자열이 어떤 문자열로 닫히는지 다음 줄에 넘긴다.
struct BlockData : QTextBlockUserData
{
    QString closer;
};

const QSet<QString> &keywords()
{
    static const QSet<QString> words = [] {
        const char *list[] = {
            // 공통
            "SELECT", "FROM", "WHERE", "AND", "OR", "NOT", "IN", "IS", "NULL", "LIKE", "BETWEEN", "EXISTS", "AS",
            "ON", "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "CROSS", "NATURAL", "USING", "GROUP", "BY",
            "HAVING", "ORDER", "ASC", "DESC", "NULLS", "FIRST", "LAST", "UNION", "ALL", "ANY", "SOME", "INTERSECT",
            "EXCEPT", "MINUS", "DISTINCT", "CASE", "WHEN", "THEN", "ELSE", "END", "INSERT", "INTO", "VALUES",
            "UPDATE", "SET", "DELETE", "MERGE", "MATCHED", "CREATE", "ALTER", "DROP", "TRUNCATE", "RENAME",
            "TABLE", "VIEW", "INDEX", "UNIQUE", "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "CONSTRAINT", "CHECK",
            "DEFAULT", "SEQUENCE", "TRIGGER", "PROCEDURE", "FUNCTION", "PACKAGE", "BODY", "TYPE", "SCHEMA",
            "DATABASE", "GRANT", "REVOKE", "COMMIT", "ROLLBACK", "SAVEPOINT", "BEGIN", "DECLARE", "RETURN",
            "RETURNS", "IF", "ELSIF", "ELSEIF", "LOOP", "WHILE", "FOR", "EXIT", "CONTINUE", "WITH", "RECURSIVE",
            "OVER", "PARTITION", "ROWS", "RANGE", "UNBOUNDED", "PRECEDING", "FOLLOWING", "CURRENT", "ROW", "LIMIT",
            "OFFSET", "FETCH", "NEXT", "ONLY", "REPLACE", "TEMPORARY", "CASCADE", "RESTRICT", "EXPLAIN", "ANALYZE",
            "TRUE", "FALSE", "COLUMN", "ADD", "MODIFY", "COMMENT", "USER", "ROLE", "TO", "OF", "DO", "EXCEPTION",
            "RAISE", "CURSOR", "OPEN", "CLOSE", "EXECUTE", "IMMEDIATE", "CALL", "LOCK", "NOWAIT", "PLAN",
            // Oracle, Tibero
            "CONNECT", "PRIOR", "START", "NOCYCLE", "LEVEL", "ROWNUM", "ROWID", "SYSDATE", "SYSTIMESTAMP", "DUAL",
            "PRAGMA", "AUTONOMOUS_TRANSACTION", "OUT", "NOCOPY", "DETERMINISTIC", "PIPELINED", "REF", "SYNONYM",
            "PUBLIC", "TABLESPACE", "PURGE", "FLASHBACK", "MATERIALIZED", "OTHERS", "FORALL", "BULK", "COLLECT",
            "EDITIONABLE", "NONEDITIONABLE", "IDENTIFIED", "PIVOT", "UNPIVOT",
            // MySQL, MariaDB
            "SHOW", "DESCRIBE", "USE", "AUTO_INCREMENT", "ENGINE", "CHARSET", "COLLATE", "DELIMITER", "IGNORE",
            "DUPLICATE", "REGEXP", "RLIKE", "UNSIGNED", "ZEROFILL", "KILL", "PROCESSLIST",
            // PostgreSQL
            "RETURNING", "ILIKE", "SIMILAR", "LATERAL", "VACUUM", "COPY", "LANGUAGE", "VOLATILE", "STABLE",
            "IMMUTABLE", "SECURITY", "DEFINER", "INVOKER", "EXTENSION", "ATOMIC", "CONFLICT", "NOTHING",
        };
        QSet<QString> set;
        for (const char *word : list)
            set.insert(QString::fromLatin1(word));
        return set;
    }();
    return words;
}

const QSet<QString> &types()
{
    static const QSet<QString> words = [] {
        const char *list[] = {
            "CHAR", "VARCHAR", "VARCHAR2", "NCHAR", "NVARCHAR2", "NUMBER", "NUMERIC", "DECIMAL", "INT", "INTEGER",
            "SMALLINT", "BIGINT", "TINYINT", "MEDIUMINT", "FLOAT", "REAL", "DOUBLE", "PRECISION", "BINARY_FLOAT",
            "BINARY_DOUBLE", "DATE", "TIME", "TIMESTAMP", "INTERVAL", "YEAR", "MONTH", "DAY", "HOUR", "MINUTE",
            "SECOND", "ZONE", "LOCAL", "DATETIME", "BOOLEAN", "BOOL", "BIT", "CLOB", "NCLOB", "BLOB", "BFILE",
            "RAW", "LONG", "TEXT", "MEDIUMTEXT", "LONGTEXT", "TINYTEXT", "BYTEA", "JSON", "JSONB", "UUID",
            "SERIAL", "BIGSERIAL", "XMLTYPE", "ENUM", "VARBINARY", "BINARY", "CHARACTER", "VARYING", "PLS_INTEGER",
            "BINARY_INTEGER", "ROWTYPE",
        };
        QSet<QString> set;
        for (const char *word : list)
            set.insert(QString::fromLatin1(word));
        return set;
    }();
    return words;
}

bool isWordStart(QChar c)
{
    return c.isLetter() || c == u'_';
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$' || c == u'#';
}

// 같은 줄에서 닫는 따옴표를 찾는다. 닫는 따옴표 다음 위치, 이 줄에 없으면 -1.
int findQuoteEnd(const QString &text, int from, QChar quote, bool backslash)
{
    const int n = text.size();
    int j = from;
    while (j < n) {
        const QChar c = text[j];
        if (backslash && c == u'\\') {
            j += 2;
            continue;
        }
        if (c == quote) {
            if (j + 1 < n && text[j + 1] == quote) {
                j += 2;
                continue;
            }
            return j + 1;
        }
        ++j;
    }
    return -1;
}

QChar closingFor(QChar open)
{
    switch (open.unicode()) {
    case u'[': return u']';
    case u'{': return u'}';
    case u'(': return u')';
    case u'<': return u'>';
    default: return open;
    }
}

QTextCharFormat makeFormat(const QColor &color, bool bold = false, bool italic = false)
{
    QTextCharFormat format;
    format.setForeground(color);
    if (bold)
        format.setFontWeight(QFont::Bold);
    format.setFontItalic(italic);
    return format;
}

} // namespace

SqlHighlighter::SqlHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    setupFormats(false);
}

void SqlHighlighter::setDialect(SqlDialect dialect)
{
    if (m_dialect == dialect)
        return;
    m_dialect = dialect;
    rehighlight();
}

void SqlHighlighter::setDarkTheme(bool dark)
{
    setupFormats(dark);
    rehighlight();
}

void SqlHighlighter::setupFormats(bool dark)
{
    if (dark) {
        m_keyword = makeFormat(QColor(0xcc, 0x78, 0x32), true);
        m_type = makeFormat(QColor(0x98, 0x76, 0xaa));
        m_string = makeFormat(QColor(0x6a, 0x87, 0x59));
        m_number = makeFormat(QColor(0x68, 0x97, 0xbb));
        m_comment = makeFormat(QColor(0x80, 0x80, 0x80), false, true);
        m_identifier = makeFormat(QColor(0xc7, 0x7d, 0xbb));
        m_parameter = makeFormat(QColor(0xd5, 0xb7, 0x78));
    } else {
        m_keyword = makeFormat(QColor(0x00, 0x33, 0xb3), true);
        m_type = makeFormat(QColor(0x00, 0x62, 0x7a));
        m_string = makeFormat(QColor(0x06, 0x7d, 0x17));
        m_number = makeFormat(QColor(0x17, 0x50, 0xeb));
        m_comment = makeFormat(QColor(0x8c, 0x8c, 0x8c), false, true);
        m_identifier = makeFormat(QColor(0x87, 0x10, 0x94));
        m_parameter = makeFormat(QColor(0xb0, 0x58, 0x00));
    }
}

void SqlHighlighter::continueState(int state, const QString &closer)
{
    setCurrentBlockState(state);
    if (!closer.isEmpty()) {
        auto *data = new BlockData;
        data->closer = closer;
        setCurrentBlockUserData(data);
    }
}

void SqlHighlighter::highlightBlock(const QString &text)
{
    const bool oracle = m_dialect == SqlDialect::Oracle;
    const bool mysql = m_dialect == SqlDialect::MySql;
    const bool postgres = m_dialect == SqlDialect::PostgreSql;
    const int n = text.size();
    int i = 0;

    // 앞 줄에서 이어지는 주석·문자열을 먼저 마무리한다.
    int state = previousBlockState();
    if (state > Normal) {
        QString closer;
        if (state == Dollar || state == QQuote) {
            const auto *data = static_cast<const BlockData *>(currentBlock().previous().userData());
            closer = data ? data->closer : QString();
        }

        int end = -1;
        const QTextCharFormat *format = &m_string;
        switch (state) {
        case BlockComment: {
            const int close = text.indexOf(QStringLiteral("*/"));
            end = close < 0 ? -1 : close + 2;
            format = &m_comment;
            break;
        }
        case SingleQuote: end = findQuoteEnd(text, 0, u'\'', mysql); break;
        case EscapedQuote: end = findQuoteEnd(text, 0, u'\'', true); break;
        case DoubleQuote:
            end = findQuoteEnd(text, 0, u'"', false);
            format = &m_identifier;
            break;
        case Backtick:
            end = findQuoteEnd(text, 0, u'`', false);
            format = &m_identifier;
            break;
        case Dollar:
        case QQuote: {
            const int close = closer.isEmpty() ? -1 : text.indexOf(closer);
            end = close < 0 ? -1 : close + closer.size();
            break;
        }
        default: break;
        }

        if ((state == Dollar || state == QQuote) && closer.isEmpty())
            end = 0; // 닫는 문자열 정보를 잃었으면 상태를 끝낸다
        if (end < 0) {
            setFormat(0, n, *format);
            continueState(state, closer);
            return;
        }
        setFormat(0, end, *format);
        i = end;
    }

    while (i < n) {
        const QChar c = text[i];
        const QChar next = i + 1 < n ? text[i + 1] : QChar();

        // 주석
        if ((c == u'-' && next == u'-') || (mysql && c == u'#')) {
            setFormat(i, n - i, m_comment);
            break;
        }
        if (c == u'/' && next == u'*') {
            const int close = text.indexOf(QStringLiteral("*/"), i + 2);
            if (close < 0) {
                setFormat(i, n - i, m_comment);
                continueState(BlockComment, {});
                return;
            }
            setFormat(i, close + 2 - i, m_comment);
            i = close + 2;
            continue;
        }

        // 문자열과 인용 식별자
        if (c == u'\'' || c == u'"' || (mysql && c == u'`')) {
            const bool identifier = c != u'\'';
            const int end = findQuoteEnd(text, i + 1, c, mysql && c == u'\'');
            const QTextCharFormat &format = identifier ? m_identifier : m_string;
            if (end < 0) {
                setFormat(i, n - i, format);
                continueState(c == u'\'' ? SingleQuote : (c == u'"' ? DoubleQuote : Backtick), {});
                return;
            }
            setFormat(i, end - i, format);
            i = end;
            continue;
        }

        // PostgreSQL: $$ 본문, $1 파라미터
        if (postgres && c == u'$') {
            int j = i + 1;
            while (j < n && (text[j].isLetterOrNumber() || text[j] == u'_'))
                ++j;
            if (j < n && text[j] == u'$' && !(j > i + 1 && text[i + 1].isDigit())) {
                const QString tag = text.mid(i, j - i + 1);
                const int close = text.indexOf(tag, j + 1);
                if (close < 0) {
                    setFormat(i, n - i, m_string);
                    continueState(Dollar, tag);
                    return;
                }
                setFormat(i, close + tag.size() - i, m_string);
                i = close + tag.size();
                continue;
            }
            if (j > i + 1) {
                setFormat(i, j - i, m_parameter);
                i = j;
                continue;
            }
        }

        // 바인드 변수: :name (PostgreSQL의 ::형변환은 제외), ?
        if (c == u':' && next == u':') {
            i += 2;
            continue;
        }
        if (c == u':' && (isWordStart(next) || next.isDigit())) {
            int j = i + 1;
            while (j < n && isWordChar(text[j]))
                ++j;
            setFormat(i, j - i, m_parameter);
            i = j;
            continue;
        }
        if (c == u'?') {
            setFormat(i, 1, m_parameter);
            ++i;
            continue;
        }

        // 숫자
        if (c.isDigit() || (c == u'.' && next.isDigit())) {
            int j = i + 1;
            while (j < n && (text[j].isDigit() || text[j] == u'.'))
                ++j;
            if (j < n && (text[j] == u'e' || text[j] == u'E')) {
                int k = j + 1;
                if (k < n && (text[k] == u'+' || text[k] == u'-'))
                    ++k;
                if (k < n && text[k].isDigit()) {
                    j = k;
                    while (j < n && text[j].isDigit())
                        ++j;
                }
            }
            setFormat(i, j - i, m_number);
            i = j;
            continue;
        }

        // 단어
        if (isWordStart(c)) {
            int j = i + 1;
            while (j < n && isWordChar(text[j]))
                ++j;
            const QString word = text.mid(i, j - i);

            // Oracle q'[...]' 문자열
            if (oracle && j + 1 < n && text[j] == u'\''
                && (word.compare(u"q", Qt::CaseInsensitive) == 0 || word.compare(u"nq", Qt::CaseInsensitive) == 0)) {
                const QString closer = QString(closingFor(text[j + 1])) + u'\'';
                const int close = text.indexOf(closer, j + 2);
                if (close < 0) {
                    setFormat(i, n - i, m_string);
                    continueState(QQuote, closer);
                    return;
                }
                setFormat(i, close + closer.size() - i, m_string);
                i = close + closer.size();
                continue;
            }
            // PostgreSQL E'...' 문자열
            if (postgres && j < n && text[j] == u'\'' && word.compare(u"E", Qt::CaseInsensitive) == 0) {
                const int end = findQuoteEnd(text, j + 1, u'\'', true);
                if (end < 0) {
                    setFormat(i, n - i, m_string);
                    continueState(EscapedQuote, {});
                    return;
                }
                setFormat(i, end - i, m_string);
                i = end;
                continue;
            }

            const QString upper = word.toUpper();
            if (keywords().contains(upper))
                setFormat(i, j - i, m_keyword);
            else if (types().contains(upper))
                setFormat(i, j - i, m_type);
            i = j;
            continue;
        }

        ++i;
    }

    setCurrentBlockState(Normal);
}
