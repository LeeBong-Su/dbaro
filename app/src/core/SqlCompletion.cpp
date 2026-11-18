#include "SqlCompletion.h"

#include <QSet>

namespace {

struct Token
{
    enum Type { Word, Quoted, String, Number, Punct, Comment };

    Type type = Punct;
    int start = 0;
    int end = 0;        // 끝 다음 위치
    QString value;      // Word: 원문, Quoted: 따옴표를 뺀 이름, Punct: 그 글자
    bool closed = true; // 따옴표·문자열·블록 주석이 닫혔는지 (줄 주석은 늘 false)

    bool isName() const { return type == Word || type == Quoted; }
    bool is(QChar c) const { return type == Punct && value.size() == 1 && value.at(0) == c; }
    QString upper() const { return type == Word ? value.toUpper() : QString(); }
};

bool isWordStart(QChar c)
{
    return c.isLetter() || c == u'_';
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$' || c == u'#';
}

QList<Token> tokenize(const QString &s, SqlDialect dialect)
{
    QList<Token> tokens;
    const int n = int(s.size());
    int i = 0;
    while (i < n) {
        const QChar c = s.at(i);
        if (c.isSpace()) {
            ++i;
            continue;
        }
        Token t;
        t.start = i;
        if ((c == u'-' && i + 1 < n && s.at(i + 1) == u'-') || (c == u'#' && dialect == SqlDialect::MySql)) {
            t.type = Token::Comment;
            int stop = int(s.indexOf(u'\n', i));
            i = stop < 0 ? n : stop;
            t.closed = false; // 줄 끝까지는 주석이다
        } else if (c == u'/' && i + 1 < n && s.at(i + 1) == u'*') {
            t.type = Token::Comment;
            const int stop = int(s.indexOf(QStringLiteral("*/"), i + 2));
            t.closed = stop >= 0;
            i = stop < 0 ? n : stop + 2;
        } else if (c == u'\'') {
            t.type = Token::String;
            t.closed = false;
            ++i;
            while (i < n) {
                if (s.at(i) == u'\'') {
                    if (i + 1 < n && s.at(i + 1) == u'\'') {
                        i += 2;
                        continue;
                    }
                    ++i;
                    t.closed = true;
                    break;
                }
                ++i;
            }
        } else if (c == u'"' || (c == u'`' && dialect == SqlDialect::MySql)) {
            t.type = Token::Quoted;
            t.closed = false;
            ++i;
            while (i < n) {
                if (s.at(i) == c) {
                    if (i + 1 < n && s.at(i + 1) == c) {
                        t.value += c;
                        i += 2;
                        continue;
                    }
                    ++i;
                    t.closed = true;
                    break;
                }
                t.value += s.at(i++);
            }
        } else if (isWordStart(c)) {
            t.type = Token::Word;
            while (i < n && isWordChar(s.at(i)))
                ++i;
            t.value = s.mid(t.start, i - t.start);
        } else if (c.isDigit()) {
            t.type = Token::Number;
            while (i < n && (s.at(i).isLetterOrNumber() || s.at(i) == u'.'))
                ++i;
        } else {
            t.type = Token::Punct;
            t.value = c;
            ++i;
        }
        t.end = i;
        tokens.append(t);
    }
    return tokens;
}

bool inside(const Token &t, int cursor)
{
    return t.start < cursor && (cursor < t.end || (cursor == t.end && !t.closed));
}

const QSet<QString> &tableClauses()
{
    static const QSet<QString> words{QStringLiteral("FROM"), QStringLiteral("JOIN"), QStringLiteral("UPDATE"),
                                     QStringLiteral("INTO"), QStringLiteral("TABLE"), QStringLiteral("DESCRIBE"),
                                     QStringLiteral("DESC"), QStringLiteral("TRUNCATE")};
    return words;
}

const QSet<QString> &columnClauses()
{
    static const QSet<QString> words{
        QStringLiteral("SELECT"), QStringLiteral("WHERE"), QStringLiteral("ON"), QStringLiteral("AND"),
        QStringLiteral("OR"), QStringLiteral("BY"), QStringLiteral("HAVING"), QStringLiteral("SET"),
        QStringLiteral("WHEN"), QStringLiteral("THEN"), QStringLiteral("ELSE"), QStringLiteral("CASE"),
        QStringLiteral("NOT"), QStringLiteral("DISTINCT"), QStringLiteral("RETURNING"), QStringLiteral("USING"),
    };
    return words;
}

// 테이블 이름 뒤에 와도 별칭이 아닌 단어
const QSet<QString> &notAliases()
{
    static const QSet<QString> words{
        QStringLiteral("WHERE"), QStringLiteral("JOIN"), QStringLiteral("INNER"), QStringLiteral("LEFT"),
        QStringLiteral("RIGHT"), QStringLiteral("FULL"), QStringLiteral("OUTER"), QStringLiteral("CROSS"),
        QStringLiteral("NATURAL"), QStringLiteral("ON"), QStringLiteral("USING"), QStringLiteral("GROUP"),
        QStringLiteral("ORDER"), QStringLiteral("HAVING"), QStringLiteral("UNION"), QStringLiteral("INTERSECT"),
        QStringLiteral("EXCEPT"), QStringLiteral("MINUS"), QStringLiteral("LIMIT"), QStringLiteral("OFFSET"),
        QStringLiteral("FETCH"), QStringLiteral("FOR"), QStringLiteral("SET"), QStringLiteral("VALUES"),
        QStringLiteral("WINDOW"), QStringLiteral("START"), QStringLiteral("CONNECT"), QStringLiteral("PARTITION"),
        QStringLiteral("RETURNING"), QStringLiteral("SELECT"), QStringLiteral("WITH"), QStringLiteral("PIVOT"),
        QStringLiteral("UNPIVOT"), QStringLiteral("LATERAL"), QStringLiteral("STRAIGHT_JOIN"), QStringLiteral("USE"),
        QStringLiteral("FORCE"), QStringLiteral("IGNORE"), QStringLiteral("DEFAULT"), QStringLiteral("SAMPLE"),
        QStringLiteral("AS"), QStringLiteral("AND"), QStringLiteral("OR"), QStringLiteral("WHEN"),
    };
    return words;
}

// FROM/JOIN/UPDATE/INTO 뒤의 테이블과 별칭을 모은다.
QList<SqlTableRef> collectTables(const QList<Token> &tokens)
{
    QList<SqlTableRef> refs;
    const int n = int(tokens.size());
    auto at = [&](int i) -> const Token * { return i >= 0 && i < n ? &tokens[i] : nullptr; };

    for (int i = 0; i < n; ++i) {
        const QString keyword = tokens[i].upper();
        const bool list = keyword == u"FROM";
        if (!list && keyword != u"JOIN" && keyword != u"UPDATE" && keyword != u"INTO")
            continue;
        int j = i + 1;
        while (true) {
            const Token *first = at(j);
            if (!first || !first->isName() || notAliases().contains(first->upper()))
                break;
            QStringList parts{first->value};
            ++j;
            while (at(j) && at(j)->is(u'.') && at(j + 1) && at(j + 1)->isName()) {
                parts << at(j + 1)->value;
                j += 2;
            }
            SqlTableRef ref;
            ref.name = parts.last();
            if (parts.size() >= 2)
                ref.schema = parts.at(parts.size() - 2);
            if (at(j) && at(j)->upper() == u"AS")
                ++j;
            if (at(j) && at(j)->isName() && !notAliases().contains(at(j)->upper())) {
                ref.alias = at(j)->value;
                ++j;
            }
            refs.append(ref);
            if (list && at(j) && at(j)->is(u',')) {
                ++j;
                continue;
            }
            break;
        }
    }
    return refs;
}

} // namespace

SqlCompletionContext SqlCompletion::analyze(const QString &statement, int cursor, SqlDialect dialect)
{
    SqlCompletionContext context;
    cursor = qBound(0, cursor, int(statement.size()));
    const QList<Token> all = tokenize(statement, dialect);

    // 문자열이나 주석 안이면 제안하지 않는다.
    for (const Token &t : all) {
        if ((t.type == Token::String || t.type == Token::Comment) && inside(t, cursor))
            return context;
    }

    QList<Token> tokens; // 주석을 뺀 토큰
    for (const Token &t : all) {
        if (t.type != Token::Comment)
            tokens.append(t);
    }

    // 입력 중인 단어
    int prefixIndex = -1;
    for (int i = 0; i < tokens.size(); ++i) {
        const Token &t = tokens[i];
        if (t.start < cursor && cursor <= t.end) {
            if (t.type == Token::Number)
                return context;
            if (t.isName())
                prefixIndex = i;
            break;
        }
    }
    int firstAfter = 0; // 커서 앞 토큰 수
    if (prefixIndex >= 0) {
        const Token &t = tokens[prefixIndex];
        context.prefixStart = t.start;
        if (t.type == Token::Quoted) {
            context.quoted = true;
            const int contentEnd = (t.closed && cursor == t.end) ? cursor - 1 : cursor;
            context.prefix = statement.mid(t.start + 1, qMax(0, contentEnd - t.start - 1));
        } else {
            context.prefix = statement.mid(t.start, cursor - t.start);
        }
        firstAfter = prefixIndex;
    } else {
        context.prefixStart = cursor;
        while (firstAfter < tokens.size() && tokens[firstAfter].end <= cursor)
            ++firstAfter;
    }

    // "x." 또는 "a.b." 처럼 이름 뒤 점에 붙어 있으면 그 이름이 한정자다.
    int scanEnd = firstAfter; // 문맥은 한정자 앞에서부터 찾는다
    auto adjacent = [&](int i, int j) { return tokens[i].end == tokens[j].start; };
    if (scanEnd >= 2 && tokens[scanEnd - 1].is(u'.') && tokens[scanEnd - 1].end == context.prefixStart
        && tokens[scanEnd - 2].isName() && adjacent(scanEnd - 2, scanEnd - 1)) {
        context.qualifier = tokens[scanEnd - 2].value;
        scanEnd -= 2;
        if (scanEnd >= 2 && tokens[scanEnd - 1].is(u'.') && adjacent(scanEnd - 1, scanEnd)
            && tokens[scanEnd - 2].isName() && adjacent(scanEnd - 2, scanEnd - 1)) {
            context.outerQualifier = tokens[scanEnd - 2].value;
            scanEnd -= 2;
        }
    }

    // 같은 괄호 깊이에서 가장 가까운 절 키워드를 찾는다.
    QString clause;
    bool insideParens = false;
    int depth = 0;
    for (int i = scanEnd - 1; i >= 0; --i) {
        const Token &t = tokens[i];
        if (t.is(u')')) {
            ++depth;
            continue;
        }
        if (t.is(u'(')) {
            if (depth > 0)
                --depth;
            else
                insideParens = true; // 함수 인자나 IN (...) 안: 바깥 절을 따른다
            continue;
        }
        if (depth > 0 || t.type != Token::Word)
            continue;
        const QString upper = t.upper();
        if (tableClauses().contains(upper) || columnClauses().contains(upper)) {
            clause = upper;
            break;
        }
    }
    const Token *previous = scanEnd > 0 ? &tokens[scanEnd - 1] : nullptr;

    context.tables = collectTables(tokens);

    if (tableClauses().contains(clause)) {
        if (!context.qualifier.isEmpty())
            context.kind = SqlCompletionContext::Kind::Table; // schema.|
        else if (insideParens && clause == u"INTO")
            context.kind = SqlCompletionContext::Kind::Column; // INSERT INTO t (|
        else if (previous && (previous->upper() == clause || previous->is(u',')))
            context.kind = SqlCompletionContext::Kind::Table;
        else
            context.kind = SqlCompletionContext::Kind::Keyword; // FROM emp | → WHERE, JOIN ...
    } else if (columnClauses().contains(clause) || !context.qualifier.isEmpty()) {
        context.kind = SqlCompletionContext::Kind::Column;
    } else {
        context.kind = SqlCompletionContext::Kind::Keyword;
    }
    return context;
}

const SqlTableRef *SqlCompletion::findTable(const SqlCompletionContext &context, const QString &aliasOrName)
{
    for (const SqlTableRef &ref : context.tables) {
        if (!ref.alias.isEmpty() && ref.alias.compare(aliasOrName, Qt::CaseInsensitive) == 0)
            return &ref;
    }
    for (const SqlTableRef &ref : context.tables) {
        if (ref.alias.isEmpty() && ref.name.compare(aliasOrName, Qt::CaseInsensitive) == 0)
            return &ref;
    }
    return nullptr;
}

const QStringList &SqlCompletion::keywords()
{
    static const QStringList words{
        QStringLiteral("SELECT"), QStringLiteral("FROM"), QStringLiteral("WHERE"), QStringLiteral("AND"),
        QStringLiteral("OR"), QStringLiteral("NOT"), QStringLiteral("IN"), QStringLiteral("IS NULL"),
        QStringLiteral("IS NOT NULL"), QStringLiteral("LIKE"), QStringLiteral("BETWEEN"), QStringLiteral("EXISTS"),
        QStringLiteral("NOT EXISTS"), QStringLiteral("AS"), QStringLiteral("ON"), QStringLiteral("JOIN"),
        QStringLiteral("INNER JOIN"), QStringLiteral("LEFT JOIN"), QStringLiteral("LEFT OUTER JOIN"),
        QStringLiteral("RIGHT JOIN"), QStringLiteral("FULL OUTER JOIN"), QStringLiteral("CROSS JOIN"),
        QStringLiteral("GROUP BY"), QStringLiteral("ORDER BY"), QStringLiteral("HAVING"), QStringLiteral("ASC"),
        QStringLiteral("DESC"), QStringLiteral("DISTINCT"), QStringLiteral("UNION"), QStringLiteral("UNION ALL"),
        QStringLiteral("CASE"), QStringLiteral("WHEN"), QStringLiteral("THEN"), QStringLiteral("ELSE"),
        QStringLiteral("END"), QStringLiteral("NULL"), QStringLiteral("INSERT INTO"), QStringLiteral("VALUES"),
        QStringLiteral("UPDATE"), QStringLiteral("SET"), QStringLiteral("DELETE FROM"), QStringLiteral("MERGE INTO"),
        QStringLiteral("CREATE TABLE"), QStringLiteral("CREATE INDEX"), QStringLiteral("CREATE VIEW"),
        QStringLiteral("ALTER TABLE"), QStringLiteral("DROP TABLE"), QStringLiteral("TRUNCATE TABLE"),
        QStringLiteral("WITH"), QStringLiteral("PARTITION BY"), QStringLiteral("OVER"), QStringLiteral("LIMIT"),
        QStringLiteral("OFFSET"), QStringLiteral("FETCH FIRST"), QStringLiteral("ROWS ONLY"), QStringLiteral("COMMIT"),
        QStringLiteral("ROLLBACK"), QStringLiteral("EXPLAIN"), QStringLiteral("USE"), QStringLiteral("SHOW"),
        QStringLiteral("DUAL"),
    };
    return words;
}

const QStringList &SqlCompletion::functions(SqlDialect dialect)
{
    static const QStringList common{
        QStringLiteral("COUNT"), QStringLiteral("SUM"), QStringLiteral("AVG"), QStringLiteral("MIN"),
        QStringLiteral("MAX"), QStringLiteral("COALESCE"), QStringLiteral("NULLIF"), QStringLiteral("CAST"),
        QStringLiteral("UPPER"), QStringLiteral("LOWER"), QStringLiteral("TRIM"), QStringLiteral("LENGTH"),
        QStringLiteral("REPLACE"), QStringLiteral("ROUND"), QStringLiteral("ABS"), QStringLiteral("CONCAT"),
        QStringLiteral("ROW_NUMBER"), QStringLiteral("RANK"), QStringLiteral("DENSE_RANK"), QStringLiteral("LAG"),
        QStringLiteral("LEAD"), QStringLiteral("CURRENT_DATE"), QStringLiteral("CURRENT_TIMESTAMP"),
    };
    static const QStringList oracle = common + QStringList{
        QStringLiteral("NVL"), QStringLiteral("NVL2"), QStringLiteral("DECODE"), QStringLiteral("TO_CHAR"),
        QStringLiteral("TO_DATE"), QStringLiteral("TO_NUMBER"), QStringLiteral("TO_TIMESTAMP"), QStringLiteral("TRUNC"),
        QStringLiteral("SUBSTR"), QStringLiteral("INSTR"), QStringLiteral("LPAD"), QStringLiteral("RPAD"),
        QStringLiteral("ADD_MONTHS"), QStringLiteral("MONTHS_BETWEEN"), QStringLiteral("LAST_DAY"),
        QStringLiteral("LISTAGG"), QStringLiteral("REGEXP_LIKE"), QStringLiteral("REGEXP_SUBSTR"),
        QStringLiteral("REGEXP_REPLACE"), QStringLiteral("SYSDATE"), QStringLiteral("SYSTIMESTAMP"),
        QStringLiteral("ROWNUM"),
    };
    static const QStringList mysql = common + QStringList{
        QStringLiteral("IFNULL"), QStringLiteral("IF"), QStringLiteral("NOW"), QStringLiteral("CURDATE"),
        QStringLiteral("DATE_FORMAT"), QStringLiteral("STR_TO_DATE"), QStringLiteral("DATE_ADD"),
        QStringLiteral("DATE_SUB"), QStringLiteral("DATEDIFF"), QStringLiteral("GROUP_CONCAT"),
        QStringLiteral("SUBSTRING"), QStringLiteral("LPAD"), QStringLiteral("RPAD"), QStringLiteral("JSON_EXTRACT"),
    };
    static const QStringList postgres = common + QStringList{
        QStringLiteral("NOW"), QStringLiteral("TO_CHAR"), QStringLiteral("TO_DATE"), QStringLiteral("TO_TIMESTAMP"),
        QStringLiteral("DATE_TRUNC"), QStringLiteral("EXTRACT"), QStringLiteral("STRING_AGG"),
        QStringLiteral("ARRAY_AGG"), QStringLiteral("SUBSTRING"), QStringLiteral("GENERATE_SERIES"),
        QStringLiteral("JSONB_BUILD_OBJECT"),
    };
    switch (dialect) {
    case SqlDialect::Oracle: return oracle;
    case SqlDialect::MySql: return mysql;
    case SqlDialect::PostgreSql: return postgres;
    case SqlDialect::Generic: break;
    }
    return common;
}
