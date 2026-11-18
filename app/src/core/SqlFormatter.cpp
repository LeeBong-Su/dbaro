#include "SqlFormatter.h"

#include "SqlCompletion.h"
#include "SqlSplitter.h"

#include <QList>
#include <QSet>
#include <QStringList>

namespace {

// ---------------------------------------------------------------- 토큰

enum class Kind { Word, Quoted, String, Number, LineComment, BlockComment, Comma, Open, Close, Dot, Semicolon, Operator };

struct Token
{
    Kind kind = Kind::Word;
    QString text;
    QString upper;            // Word의 대문자
    bool spaceBefore = false; // 원래 앞에 공백이 있었다
};

bool isNameStart(QChar c)
{
    return c.isLetter() || c == u'_';
}

bool isNameChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$' || c == u'#';
}

bool isOperatorChar(QChar c, SqlDialect dialect)
{
    static const QString chars = QStringLiteral("=<>!^~|&+-*/%@?:");
    return chars.contains(c) || (c == u'#' && dialect == SqlDialect::PostgreSql);
}

int skipQuoted(const QString &s, int open, QChar quote, bool backslash)
{
    int j = open + 1;
    while (j < s.size()) {
        if (backslash && s[j] == u'\\') {
            j += 2;
            continue;
        }
        if (s[j] == quote) {
            if (j + 1 < s.size() && s[j + 1] == quote) {
                j += 2;
                continue;
            }
            return j + 1;
        }
        ++j;
    }
    return int(s.size());
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

QList<Token> tokenize(const QString &s, SqlDialect dialect)
{
    const bool mysql = dialect == SqlDialect::MySql;
    const bool postgres = dialect == SqlDialect::PostgreSql;
    const bool oracle = dialect == SqlDialect::Oracle;
    QList<Token> tokens;
    bool space = false;
    int i = 0;
    const int n = int(s.size());
    auto add = [&](Kind kind, int from, int to) {
        Token token;
        token.kind = kind;
        token.text = s.mid(from, to - from);
        if (kind == Kind::Word)
            token.upper = token.text.toUpper();
        token.spaceBefore = space;
        tokens.append(token);
        space = false;
    };
    while (i < n) {
        const QChar c = s[i];
        const QChar next = i + 1 < n ? s[i + 1] : QChar();
        if (c.isSpace()) {
            space = true;
            ++i;
            continue;
        }
        if ((c == u'-' && next == u'-') || (mysql && c == u'#')) {
            int end = s.indexOf(u'\n', i);
            end = end < 0 ? n : end;
            int textEnd = end;
            while (textEnd > i && s[textEnd - 1].isSpace())
                --textEnd;
            add(Kind::LineComment, i, textEnd);
            i = end;
            continue;
        }
        if (c == u'/' && next == u'*') {
            const int end = s.indexOf(QStringLiteral("*/"), i + 2);
            const int stop = end < 0 ? n : end + 2;
            add(Kind::BlockComment, i, stop);
            i = stop;
            continue;
        }
        if (c == u'\'') {
            const bool backslash = mysql || (postgres && !tokens.isEmpty() && !space && tokens.last().kind == Kind::Word
                                             && tokens.last().upper == u"E");
            const int stop = skipQuoted(s, i, u'\'', backslash);
            add(Kind::String, i, stop);
            i = stop;
            continue;
        }
        if (c == u'"' || (mysql && c == u'`')) {
            const int stop = skipQuoted(s, i, c, false);
            add(Kind::Quoted, i, stop);
            i = stop;
            continue;
        }
        if (postgres && c == u'$') {
            int j = i + 1;
            while (j < n && (s[j].isLetterOrNumber() || s[j] == u'_'))
                ++j;
            if (j < n && s[j] == u'$' && !(j > i + 1 && s[i + 1].isDigit())) {
                const QString tag = s.mid(i, j - i + 1);
                const int end = s.indexOf(tag, j + 1);
                const int stop = end < 0 ? n : end + int(tag.size());
                add(Kind::String, i, stop);
                i = stop;
                continue;
            }
            add(Kind::Word, i, j); // $1 파라미터
            i = j;
            continue;
        }
        if (c.isDigit() || (c == u'.' && next.isDigit())) {
            int j = i;
            while (j < n && (s[j].isDigit() || s[j] == u'.'))
                ++j;
            if (j < n && (s[j] == u'e' || s[j] == u'E') && j + 1 < n
                && (s[j + 1].isDigit() || ((s[j + 1] == u'+' || s[j + 1] == u'-') && j + 2 < n && s[j + 2].isDigit()))) {
                j += 2;
                while (j < n && s[j].isDigit())
                    ++j;
            }
            while (j < n && isNameChar(s[j])) // 1d, 10f 같은 접미사
                ++j;
            add(Kind::Number, i, j);
            i = j;
            continue;
        }
        if (isNameStart(c) || ((c == u':' || c == u'@' || c == u'&') && i + 1 < n && isNameStart(next)
                               && (c != u'&' || oracle))) {
            int j = i + 1;
            while (j < n && isNameChar(s[j]))
                ++j;
            // Oracle q'[...]' / nq'[...]'
            const QStringView word = QStringView(s).mid(i, j - i);
            if (oracle && j + 1 < n && s[j] == u'\''
                && (word.compare(u"q", Qt::CaseInsensitive) == 0 || word.compare(u"nq", Qt::CaseInsensitive) == 0)) {
                const QString closer = QString(closingFor(s[j + 1])) + u'\'';
                const int end = s.indexOf(closer, j + 2);
                const int stop = end < 0 ? n : end + int(closer.size());
                add(Kind::String, i, stop);
                i = stop;
                continue;
            }
            add(Kind::Word, i, j);
            i = j;
            continue;
        }
        switch (c.unicode()) {
        case u',': add(Kind::Comma, i, i + 1); ++i; continue;
        case u'(': add(Kind::Open, i, i + 1); ++i; continue;
        case u')': add(Kind::Close, i, i + 1); ++i; continue;
        case u'.': add(Kind::Dot, i, i + 1); ++i; continue;
        case u';': add(Kind::Semicolon, i, i + 1); ++i; continue;
        default: break;
        }
        if (isOperatorChar(c, dialect)) {
            // 연산자 글자가 이어지면 한 덩어리로 둔다 (그 안에는 공백을 넣지 않는다: <=, ||, ::, ->> ...)
            int j = i + 1;
            while (j < n && isOperatorChar(s[j], dialect) && !(s[j] == u'-' && j + 1 < n && s[j + 1] == u'-'))
                ++j;
            // 비교 연산자 바로 뒤의 부호는 따로 둔다: x<>-1 → x <> -1
            static const QSet<QString> comparisons{QStringLiteral("="), QStringLiteral("<>"), QStringLiteral("!="),
                                                   QStringLiteral("^="), QStringLiteral("<"), QStringLiteral(">"),
                                                   QStringLiteral("<="), QStringLiteral(">=")};
            if (j - i > 1 && (s[j - 1] == u'-' || s[j - 1] == u'+') && comparisons.contains(s.mid(i, j - i - 1))) {
                add(Kind::Operator, i, j - 1);
                add(Kind::Operator, j - 1, j);
            } else {
                add(Kind::Operator, i, j);
            }
            i = j;
            continue;
        }
        add(Kind::Operator, i, i + 1); // 그 밖의 글자 ([, ], { 등)
        ++i;
    }
    return tokens;
}

// ---------------------------------------------------------------- 키워드

const QSet<QString> &keywordSet()
{
    static const QSet<QString> words{
        QStringLiteral("SELECT"), QStringLiteral("FROM"), QStringLiteral("WHERE"), QStringLiteral("AND"), QStringLiteral("OR"),
        QStringLiteral("NOT"), QStringLiteral("IN"), QStringLiteral("IS"), QStringLiteral("NULL"), QStringLiteral("LIKE"),
        QStringLiteral("ILIKE"), QStringLiteral("BETWEEN"), QStringLiteral("EXISTS"), QStringLiteral("AS"), QStringLiteral("ON"),
        QStringLiteral("JOIN"), QStringLiteral("INNER"), QStringLiteral("LEFT"), QStringLiteral("RIGHT"), QStringLiteral("FULL"),
        QStringLiteral("OUTER"), QStringLiteral("CROSS"), QStringLiteral("NATURAL"), QStringLiteral("APPLY"),
        QStringLiteral("LATERAL"), QStringLiteral("GROUP"), QStringLiteral("ORDER"), QStringLiteral("BY"),
        QStringLiteral("HAVING"), QStringLiteral("ASC"), QStringLiteral("DESC"), QStringLiteral("DISTINCT"),
        QStringLiteral("UNIQUE"), QStringLiteral("UNION"), QStringLiteral("ALL"), QStringLiteral("INTERSECT"),
        QStringLiteral("MINUS"), QStringLiteral("EXCEPT"), QStringLiteral("CASE"), QStringLiteral("WHEN"),
        QStringLiteral("THEN"), QStringLiteral("ELSE"), QStringLiteral("END"), QStringLiteral("INSERT"), QStringLiteral("INTO"),
        QStringLiteral("VALUES"), QStringLiteral("UPDATE"), QStringLiteral("SET"), QStringLiteral("DELETE"),
        QStringLiteral("MERGE"), QStringLiteral("USING"), QStringLiteral("MATCHED"), QStringLiteral("WITH"),
        QStringLiteral("RECURSIVE"), QStringLiteral("LIMIT"), QStringLiteral("OFFSET"), QStringLiteral("FETCH"),
        QStringLiteral("FIRST"), QStringLiteral("NEXT"), QStringLiteral("ROWS"), QStringLiteral("ROW"), QStringLiteral("ONLY"),
        QStringLiteral("TIES"), QStringLiteral("FOR"), QStringLiteral("OF"), QStringLiteral("NOWAIT"), QStringLiteral("SKIP"),
        QStringLiteral("LOCKED"), QStringLiteral("NULLS"), QStringLiteral("LAST"), QStringLiteral("CONNECT"),
        QStringLiteral("START"), QStringLiteral("PRIOR"), QStringLiteral("NOCYCLE"), QStringLiteral("SIBLINGS"),
        QStringLiteral("OVER"), QStringLiteral("PARTITION"), QStringLiteral("RANGE"), QStringLiteral("PRECEDING"),
        QStringLiteral("FOLLOWING"), QStringLiteral("UNBOUNDED"), QStringLiteral("CURRENT"), QStringLiteral("ESCAPE"),
        QStringLiteral("ANY"), QStringLiteral("SOME"), QStringLiteral("TRUE"), QStringLiteral("FALSE"),
        QStringLiteral("RETURNING"), QStringLiteral("WINDOW"), QStringLiteral("FILTER"), QStringLiteral("WITHIN"),
        QStringLiteral("INTERVAL"), QStringLiteral("DATE"), QStringLiteral("TIMESTAMP"), QStringLiteral("SHARE"),
        QStringLiteral("ROWNUM"), QStringLiteral("ROWID"), QStringLiteral("LEVEL"), QStringLiteral("SYSDATE"),
        QStringLiteral("SYSTIMESTAMP"), QStringLiteral("REGEXP"), QStringLiteral("RLIKE"), QStringLiteral("DUPLICATE"),
        QStringLiteral("KEY"), QStringLiteral("IGNORE"), QStringLiteral("CONFLICT"), QStringLiteral("DO"),
        QStringLiteral("NOTHING"), QStringLiteral("PIVOT"), QStringLiteral("UNPIVOT"), QStringLiteral("STRAIGHT_JOIN"),
        QStringLiteral("COLLATE"), QStringLiteral("TOP"), QStringLiteral("PERCENT"),
    };
    return words;
}

// 키워드지만 함수처럼 바로 괄호가 붙는 것
bool isFunction(const QString &upper, SqlDialect dialect)
{
    return SqlCompletion::functions(dialect).contains(upper);
}

// ---------------------------------------------------------------- 정렬

enum class Clause { None, Select, From, Where, GroupBy, OrderBy, Having, SetOp, Join, On, Into, Values, Set, Using, When, With,
                    Insert, Update, Delete, Merge, Other };

struct ClauseMatch
{
    Clause clause = Clause::None;
    int words = 0;
    int width = 0; // 첫 단어 길이 (오른쪽 맞춤)
};

constexpr int kRiver = 6; // SELECT 길이: 절 키워드의 끝을 여기에 맞춘다

class Formatter
{
public:
    Formatter(const QList<Token> &tokens, SqlDialect dialect, int firstColumn)
        : m_t(tokens)
        , m_dialect(dialect)
        , m_column(firstColumn)
    {
    }

    QString run()
    {
        block(0, int(m_t.size()), m_column);
        QString out = m_out;
        while (!out.isEmpty() && out.back().isSpace())
            out.chop(1);
        return out;
    }

private:
    const Token &at(int k) const { return m_t[k]; }
    bool isWord(int k, int end, const char *word) const
    {
        return k < end && m_t[k].kind == Kind::Word && m_t[k].upper == QLatin1String(word);
    }
    int nextSignificant(int k, int end) const
    {
        while (k < end && (m_t[k].kind == Kind::LineComment || m_t[k].kind == Kind::BlockComment))
            ++k;
        return k;
    }
    int previousSignificant(int k) const
    {
        --k;
        while (k >= 0 && (m_t[k].kind == Kind::LineComment || m_t[k].kind == Kind::BlockComment))
            --k;
        return k;
    }
    int matching(int open, int end) const
    {
        int depth = 0;
        for (int k = open; k < end; ++k) {
            if (m_t[k].kind == Kind::Open)
                ++depth;
            else if (m_t[k].kind == Kind::Close && --depth == 0)
                return k;
        }
        return end;
    }

    void newline(int indent)
    {
        while (!m_out.isEmpty() && m_out.back() == u' ')
            m_out.chop(1);
        m_out += u'\n';
        m_out += QString(qMax(0, indent), u' ');
        m_column = indent;
        m_lineStart = true;
        m_pendingNewline = false;
        m_prev = nullptr;
    }

    void put(const QString &text, bool space)
    {
        if (space && !m_lineStart) {
            m_out += u' ';
            ++m_column;
        }
        m_out += text;
        const qsizetype newlineAt = text.lastIndexOf(u'\n');
        m_column = newlineAt < 0 ? m_column + int(text.size()) : int(text.size() - newlineAt - 1);
        m_lineStart = false;
    }

    bool spacedOperator(const Token &t) const
    {
        static const QSet<QString> ops{QStringLiteral("="), QStringLiteral("<>"), QStringLiteral("!="), QStringLiteral("^="),
                                       QStringLiteral("~="), QStringLiteral("<"), QStringLiteral(">"), QStringLiteral("<="),
                                       QStringLiteral(">="), QStringLiteral("<=>"), QStringLiteral("||"), QStringLiteral(":="),
                                       QStringLiteral("=>")};
        return t.kind == Kind::Operator && ops.contains(t.text);
    }

    bool needsSpace(const Token *p, const Token &t) const
    {
        if (!p)
            return false;
        if (p->kind == Kind::Open || p->kind == Kind::Dot)
            return false;
        if (t.kind == Kind::Close || t.kind == Kind::Comma || t.kind == Kind::Dot || t.kind == Kind::Semicolon)
            return false;
        if (p->kind == Kind::Comma)
            return true;
        if (t.kind == Kind::Open) {
            if (p->kind == Kind::Word && keywordSet().contains(p->upper) && !isFunction(p->upper, m_dialect))
                return true; // IN (, VALUES (, AS (, OVER (
            return t.spaceBefore; // 함수 호출 count(*)는 붙이고, INSERT INTO t (a) 처럼 띄운 것은 그대로
        }
        if (spacedOperator(t) || spacedOperator(*p))
            return true;
        const bool pWord = p->kind == Kind::Word || p->kind == Kind::Number;
        const bool tWord = t.kind == Kind::Word || t.kind == Kind::Number;
        if (pWord && tWord)
            return true; // 붙이면 한 단어가 된다
        return t.spaceBefore;
    }

    // 키워드·함수는 대문자로. 테이블 이름 자리와 schema.name 은 그대로.
    QString display(int k, int end) const
    {
        const Token &t = m_t[k];
        if (t.kind != Kind::Word || !t.text.front().isLetter())
            return t.text;
        const int prev = previousSignificant(k);
        const int next = nextSignificant(k + 1, end);
        if ((prev >= 0 && m_t[prev].kind == Kind::Dot) || (next < end && m_t[next].kind == Kind::Dot))
            return t.text;
        if (prev >= 0 && m_t[prev].kind == Kind::Word) {
            static const QSet<QString> tablePlaces{QStringLiteral("FROM"), QStringLiteral("JOIN"), QStringLiteral("INTO"),
                                                   QStringLiteral("UPDATE"), QStringLiteral("TABLE")};
            if (tablePlaces.contains(m_t[prev].upper) && !keywordSet().contains(t.upper))
                return t.text;
            if (tablePlaces.contains(m_t[prev].upper) && (next >= end || m_t[next].kind != Kind::Open)
                && t.upper != u"LATERAL" && t.upper != u"ONLY" && t.upper != u"SET")
                return t.text; // FROM dual, JOIN user 처럼 키워드와 같은 테이블 이름 (MERGE의 UPDATE SET은 제외)
        }
        if (next < end && m_t[next].kind == Kind::Open && isFunction(t.upper, m_dialect))
            return t.upper;
        if (keywordSet().contains(t.upper))
            return t.upper;
        return t.text;
    }

    void write(int k, int end, int continuation)
    {
        const Token &t = m_t[k];
        if (m_pendingNewline)
            newline(continuation);
        if (t.kind == Kind::LineComment) {
            put(t.text, !m_lineStart);
            m_pendingNewline = true; // 줄 주석 뒤에는 반드시 줄을 바꾼다
            m_prev = &t;
            return;
        }
        put(display(k, end), needsSpace(m_prev, t));
        m_prev = &t;
    }

    ClauseMatch clauseAt(int k, int end, Clause statement, Clause current, bool started) const
    {
        const Token &t = m_t[k];
        if (t.kind != Kind::Word)
            return {};
        const QString &w = t.upper;
        auto match = [&](Clause clause, int words) { return ClauseMatch{clause, words, int(t.text.size())}; };
        auto word = [&](int offset, const char *text) { return isWord(k + offset, end, text); };

        if (!started) {
            if (w == u"SELECT")
                return match(Clause::Select, 1);
            if (w == u"WITH")
                return match(Clause::With, word(1, "RECURSIVE") ? 2 : 1);
            if (w == u"INSERT")
                return match(Clause::Insert, word(1, "INTO") || word(1, "ALL") || word(1, "IGNORE") ? 2 : 1);
            if (w == u"UPDATE")
                return match(Clause::Update, 1);
            if (w == u"DELETE")
                return match(Clause::Delete, 1);
            if (w == u"MERGE")
                return match(Clause::Merge, word(1, "INTO") ? 2 : 1);
            return {};
        }
        if (w == u"SELECT")
            return match(Clause::Select, 1);
        if (w == u"FROM")
            return match(Clause::From, 1);
        if (w == u"WHERE")
            return match(Clause::Where, 1);
        if (w == u"GROUP" && word(1, "BY"))
            return match(Clause::GroupBy, 2);
        if (w == u"ORDER" && word(1, "BY"))
            return match(Clause::OrderBy, 2);
        if (w == u"ORDER" && word(1, "SIBLINGS") && word(2, "BY"))
            return match(Clause::OrderBy, 3);
        if (w == u"HAVING")
            return match(Clause::Having, 1);
        if (w == u"UNION" || w == u"INTERSECT" || w == u"EXCEPT")
            return match(Clause::SetOp, word(1, "ALL") || word(1, "DISTINCT") ? 2 : 1);
        if (w == u"MINUS")
            return match(Clause::SetOp, 1);
        // 조인
        if (w == u"JOIN" || w == u"STRAIGHT_JOIN")
            return match(Clause::Join, 1);
        if (w == u"INNER" && word(1, "JOIN"))
            return match(Clause::Join, 2);
        if ((w == u"LEFT" || w == u"RIGHT" || w == u"FULL") && word(1, "OUTER") && word(2, "JOIN"))
            return match(Clause::Join, 3);
        if ((w == u"LEFT" || w == u"RIGHT" || w == u"FULL") && word(1, "JOIN"))
            return match(Clause::Join, 2);
        if (w == u"CROSS" && (word(1, "JOIN") || word(1, "APPLY")))
            return match(Clause::Join, 2);
        if (w == u"OUTER" && word(1, "APPLY"))
            return match(Clause::Join, 2);
        if (w == u"NATURAL") {
            int words = 1;
            while (k + words < end && m_t[k + words].kind == Kind::Word && m_t[k + words].upper != u"JOIN" && words < 3)
                ++words;
            if (word(words, "JOIN"))
                return match(Clause::Join, words + 1);
        }
        if (w == u"ON" && (current == Clause::Join || (statement == Clause::Merge && current == Clause::Using)))
            return match(Clause::On, 1);
        if (w == u"INTO" && current == Clause::Select)
            return match(Clause::Into, 1);
        if (w == u"VALUES" && (statement == Clause::Insert || statement == Clause::Merge))
            return match(Clause::Values, 1);
        if (w == u"SET" && (statement == Clause::Update || statement == Clause::Merge))
            return match(Clause::Set, 1);
        if (w == u"USING" && statement == Clause::Merge && current == Clause::Merge)
            return match(Clause::Using, 1);
        if (w == u"WHEN" && statement == Clause::Merge)
            return match(Clause::When, 1);
        if (w == u"LIMIT" || w == u"OFFSET" || w == u"FETCH" || w == u"WINDOW" || w == u"RETURNING")
            return match(Clause::Other, 1);
        if (w == u"FOR" && (word(1, "UPDATE") || word(1, "SHARE")))
            return match(Clause::Other, 2);
        if ((w == u"CONNECT" && word(1, "BY")) || (w == u"START" && word(1, "WITH")))
            return match(Clause::Other, 2);
        return {};
    }

    void block(int from, int to, int base)
    {
        Clause statement = Clause::None;
        Clause clause = Clause::None;
        bool started = false;
        int depth = 0;
        int caseDepth = 0;
        bool betweenPending = false;

        for (int k = from; k < to; ++k) {
            const Token &t = m_t[k];
            const int continuation = started ? base + kRiver + 1 : base;

            if (t.kind == Kind::LineComment || t.kind == Kind::BlockComment) {
                write(k, to, continuation);
                continue;
            }

            // 괄호 안의 SELECT·WITH는 부질의: 여는 괄호 바로 뒤를 기준으로 같은 모양으로 정렬한다.
            if (t.kind == Kind::Open) {
                const int inner = nextSignificant(k + 1, to);
                if (inner < to && m_t[inner].kind == Kind::Word && (m_t[inner].upper == u"SELECT" || m_t[inner].upper == u"WITH")) {
                    const int close = matching(k, to);
                    write(k, to, continuation);
                    const int nestedBase = m_column;
                    block(k + 1, close, nestedBase);
                    if (close < to) {
                        if (m_pendingNewline) // 부질의가 줄 주석으로 끝났다: 닫는 괄호는 다음 줄에
                            newline(nestedBase);
                        put(m_t[close].text, false);
                        m_prev = &m_t[close];
                    }
                    k = close;
                    continue;
                }
                ++depth;
                write(k, to, continuation);
                continue;
            }
            if (t.kind == Kind::Close) {
                depth = qMax(0, depth - 1);
                write(k, to, continuation);
                continue;
            }

            if (t.kind == Kind::Word) {
                if (t.upper == u"CASE")
                    ++caseDepth;
                else if (t.upper == u"END" && caseDepth > 0)
                    --caseDepth;
            }

            if (depth == 0 && caseDepth == 0 && t.kind == Kind::Word) {
                const ClauseMatch match = clauseAt(k, to, statement, clause, started);
                if (match.clause != Clause::None) {
                    // DELETE FROM t 는 한 줄에 둔다.
                    const int before = previousSignificant(k);
                    const bool deleteFrom = clause == Clause::Delete && match.clause == Clause::From && before >= from
                                            && m_t[before].upper == u"DELETE";
                    if (started && !deleteFrom)
                        newline(base + qMax(0, kRiver - match.width));
                    else if (m_pendingNewline)
                        newline(base);
                    for (int w = 0; w < match.words; ++w)
                        write(k + w, to, continuation);
                    k += match.words - 1;
                    if (!started)
                        statement = match.clause;
                    clause = match.clause;
                    started = true;
                    betweenPending = false;
                    continue;
                }
                if (t.upper == u"BETWEEN")
                    betweenPending = true;
                const bool logical = t.upper == u"AND" || t.upper == u"OR";
                const bool breaksLogic = clause == Clause::Where || clause == Clause::Having || clause == Clause::On
                                         || clause == Clause::Other;
                if (logical && breaksLogic) {
                    if (t.upper == u"AND" && betweenPending) {
                        betweenPending = false;
                    } else {
                        newline(base + qMax(0, kRiver - int(t.text.size())));
                        write(k, to, continuation);
                        continue;
                    }
                }
            }

            if (t.kind == Kind::Comma && depth == 0 && caseDepth == 0) {
                const bool listClause = clause == Clause::Select || clause == Clause::From || clause == Clause::GroupBy
                                        || clause == Clause::OrderBy || clause == Clause::Set || clause == Clause::With
                                        || clause == Clause::Into;
                if (listClause) {
                    newline(base + kRiver - 1);
                    put(t.text, false);
                    m_prev = &t;
                    continue;
                }
            }
            write(k, to, continuation);
        }
    }

    const QList<Token> &m_t;
    SqlDialect m_dialect;
    QString m_out;
    int m_column = 0;
    bool m_lineStart = true;
    bool m_pendingNewline = false;
    const Token *m_prev = nullptr;
};

bool formattable(const QList<Token> &tokens)
{
    for (int k = 0; k < tokens.size(); ++k) {
        const Token &t = tokens[k];
        if (t.kind == Kind::LineComment || t.kind == Kind::BlockComment)
            continue;
        if (t.kind == Kind::Open)
            continue; // (SELECT ...) UNION (SELECT ...)
        if (t.kind != Kind::Word)
            return false;
        static const QSet<QString> starts{QStringLiteral("SELECT"), QStringLiteral("WITH"), QStringLiteral("INSERT"),
                                          QStringLiteral("UPDATE"), QStringLiteral("DELETE"), QStringLiteral("MERGE")};
        return starts.contains(t.upper);
    }
    return false;
}

} // namespace

bool SqlFormatter::canFormat(const QString &sql, SqlDialect dialect)
{
    return formattable(tokenize(sql, dialect));
}

QString SqlFormatter::formatStatement(const QString &sql, SqlDialect dialect, int firstColumn)
{
    const QList<Token> tokens = tokenize(sql, dialect);
    if (!formattable(tokens))
        return sql;
    // 맨 앞의 들여쓰기는 호출한 쪽에 이미 있으므로 결과의 첫 줄에는 넣지 않는다.
    Formatter formatter(tokens, dialect, firstColumn);
    return formatter.run();
}

QString SqlFormatter::format(const QString &script, SqlDialect dialect, int firstColumn)
{
    const QList<SqlStatement> statements = SqlSplitter::split(script, dialect);
    QString out;
    int last = 0;
    for (const SqlStatement &statement : statements) {
        const QString between = script.mid(last, statement.start - last);
        out += between;
        // 문장이 시작하는 줄의 들여쓰기 (스크립트 맨 앞이면 호출한 쪽이 알려 준 값)
        int column = firstColumn;
        if (statement.start > 0 || !out.isEmpty()) {
            const qsizetype lineStart = out.lastIndexOf(u'\n') + 1;
            column = int(out.size() - lineStart) + (lineStart == 0 ? firstColumn : 0);
        }
        out += formatStatement(script.mid(statement.start, statement.end - statement.start), dialect, column);
        last = statement.end;
    }
    out += script.mid(last);
    return out;
}
