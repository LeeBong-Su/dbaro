#include "SqlRename.h"

#include "SqlIdentifiers.h"
#include "SqlSplitter.h"

#include <QObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

struct Token
{
    enum Kind { Word, Quoted, Dot, String, Other };
    Kind kind = Other;
    int start = 0;
    int length = 0;
    QString value; // Word·Quoted: 이름 (따옴표 뺀 것)
    QChar quote;   // Quoted: 감싼 따옴표
};

bool wordStart(QChar c)
{
    return c.isLetter() || c == u'_';
}

bool wordChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$' || c == u'#';
}

// SQL 글을 낱말로 나눈다. 주석과 공백은 버리고, 문자열은 String 하나로 둔다.
// PostgreSQL $태그$는 구분자만 버리고 안을 계속 읽는다 (함수 본문).
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
        // 주석
        if (c == u'-' && i + 1 < n && s.at(i + 1) == u'-') {
            while (i < n && s.at(i) != u'\n')
                ++i;
            continue;
        }
        if (c == u'#' && dialect == SqlDialect::MySql) {
            while (i < n && s.at(i) != u'\n')
                ++i;
            continue;
        }
        if (c == u'/' && i + 1 < n && s.at(i + 1) == u'*') {
            const int end = int(s.indexOf(QStringLiteral("*/"), i + 2));
            i = end < 0 ? n : end + 2;
            continue;
        }
        // Oracle q'[...]' 문자열
        if ((c == u'q' || c == u'Q') && dialect == SqlDialect::Oracle && i + 2 < n && s.at(i + 1) == u'\'') {
            const QChar open = s.at(i + 2);
            const QChar close = open == u'[' ? QChar(u']') : open == u'(' ? QChar(u')') : open == u'{' ? QChar(u'}') : open == u'<' ? QChar(u'>') : open;
            int j = i + 3;
            while (j + 1 < n && !(s.at(j) == close && s.at(j + 1) == u'\''))
                ++j;
            const int end = std::min(n, j + 2);
            tokens.append({Token::String, i, end - i, {}, {}});
            i = end;
            continue;
        }
        // 문자열 (MySQL은 "…"도 문자열, 백슬래시 이스케이프)
        if (c == u'\'' || (c == u'"' && dialect == SqlDialect::MySql)) {
            int j = i + 1;
            while (j < n) {
                if (dialect == SqlDialect::MySql && s.at(j) == u'\\') {
                    j += 2;
                    continue;
                }
                if (s.at(j) == c) {
                    if (j + 1 < n && s.at(j + 1) == c) {
                        j += 2;
                        continue;
                    }
                    break;
                }
                ++j;
            }
            const int end = std::min(n, j + 1);
            tokens.append({Token::String, i, end - i, {}, {}});
            i = end;
            continue;
        }
        // PostgreSQL $태그$: 구분자만 건너뛴다
        if (c == u'$' && dialect == SqlDialect::PostgreSql) {
            int j = i + 1;
            while (j < n && (s.at(j).isLetterOrNumber() || s.at(j) == u'_'))
                ++j;
            if (j < n && s.at(j) == u'$' && (j == i + 1 || !s.at(i + 1).isDigit())) {
                i = j + 1;
                continue;
            }
        }
        // 따옴표 식별자
        if (c == u'"' || (c == u'`' && dialect == SqlDialect::MySql)) {
            QString value;
            int j = i + 1;
            while (j < n) {
                if (s.at(j) == c) {
                    if (j + 1 < n && s.at(j + 1) == c) {
                        value += c;
                        j += 2;
                        continue;
                    }
                    break;
                }
                value += s.at(j++);
            }
            const int end = std::min(n, j + 1);
            tokens.append({Token::Quoted, i, end - i, value, c});
            i = end;
            continue;
        }
        if (wordStart(c)) {
            int j = i + 1;
            while (j < n && wordChar(s.at(j)))
                ++j;
            tokens.append({Token::Word, i, j - i, s.mid(i, j - i), {}});
            i = j;
            continue;
        }
        if (c.isDigit()) {
            int j = i + 1;
            while (j < n && (s.at(j).isLetterOrNumber() || s.at(j) == u'.'))
                ++j;
            tokens.append({Token::Other, i, j - i, {}, {}});
            i = j;
            continue;
        }
        tokens.append({c == u'.' ? Token::Dot : Token::Other, i, 1, {}, {}});
        ++i;
    }
    return tokens;
}

bool isName(const Token &t)
{
    return t.kind == Token::Word || t.kind == Token::Quoted;
}

// 낱말이 DB에 저장된 이름 stored와 같은지: 따옴표 없으면 대소문자 무시, 따옴표면 그대로 (MySQL 백틱은 무시)
bool same(const Token &t, const QString &stored)
{
    if (t.kind == Token::Word)
        return t.value.compare(stored, Qt::CaseInsensitive) == 0;
    if (t.kind == Token::Quoted)
        return t.quote == u'`' ? t.value.compare(stored, Qt::CaseInsensitive) == 0 : t.value == stored;
    return false;
}

// 원래 낱말 모양대로 새 이름을 쓴다
QString replacementFor(const Token &t, const QString &stored, SqlDialect dialect)
{
    if (t.kind == Token::Quoted) {
        QString inner = stored;
        inner.replace(t.quote, QString(2, t.quote));
        return t.quote + inner + t.quote;
    }
    // 따옴표 없이 쓸 수 없는 이름이면 감싼다
    const QString quoted = SqlIdentifiers::quote(stored, dialect);
    if (quoted != stored)
        return quoted;
    if (t.value == t.value.toLower())
        return stored.toLower();
    if (t.value == t.value.toUpper())
        return stored.toUpper();
    return stored;
}

int lineOf(const QString &text, int position, QList<int> *lineStarts)
{
    if (lineStarts->isEmpty()) {
        lineStarts->append(0);
        for (int i = 0; i < text.size(); ++i) {
            if (text.at(i) == u'\n')
                lineStarts->append(i + 1);
        }
    }
    return int(std::upper_bound(lineStarts->cbegin(), lineStarts->cend(), position) - lineStarts->cbegin());
}

// 별칭이 될 수 없는 낱말 (테이블 이름 뒤에 오는 키워드)
const QSet<QString> &stopWords()
{
    static const QSet<QString> words = {
        QStringLiteral("WHERE"), QStringLiteral("ON"), QStringLiteral("JOIN"), QStringLiteral("INNER"), QStringLiteral("LEFT"),
        QStringLiteral("RIGHT"), QStringLiteral("FULL"), QStringLiteral("OUTER"), QStringLiteral("CROSS"), QStringLiteral("NATURAL"),
        QStringLiteral("SET"), QStringLiteral("GROUP"), QStringLiteral("ORDER"), QStringLiteral("HAVING"), QStringLiteral("UNION"),
        QStringLiteral("INTERSECT"), QStringLiteral("EXCEPT"), QStringLiteral("MINUS"), QStringLiteral("VALUES"), QStringLiteral("SELECT"),
        QStringLiteral("USING"), QStringLiteral("WITH"), QStringLiteral("CONNECT"), QStringLiteral("START"), QStringLiteral("PARTITION"),
        QStringLiteral("WINDOW"), QStringLiteral("LIMIT"), QStringLiteral("FETCH"), QStringLiteral("FOR"), QStringLiteral("RETURNING"),
        QStringLiteral("AND"), QStringLiteral("OR"), QStringLiteral("AS"), QStringLiteral("LOOP"), QStringLiteral("THEN"),
        QStringLiteral("WHEN"), QStringLiteral("ADD"), QStringLiteral("MODIFY"), QStringLiteral("DROP"), QStringLiteral("RENAME"),
        QStringLiteral("REFERENCING"), QStringLiteral("BEFORE"), QStringLiteral("AFTER"), QStringLiteral("INSTEAD"), QStringLiteral("EACH"),
        QStringLiteral("IS"), QStringLiteral("IN"), QStringLiteral("INTO"), QStringLiteral("FROM"), QStringLiteral("OFFSET"),
        QStringLiteral("SAMPLE"), QStringLiteral("PIVOT"), QStringLiteral("UNPIVOT"), QStringLiteral("LATERAL"), QStringLiteral("STRAIGHT_JOIN"),
    };
    return words;
}

} // namespace

QList<RenameMatch> SqlRename::find(const QString &text, const RenameTarget &target)
{
    QList<RenameMatch> matches;
    if (target.table.isEmpty() || target.newName.isEmpty() || !mentions(text, target.oldName()))
        return matches;
    const QList<Token> tokens = tokenize(text, target.dialect);
    QList<int> lineStarts;
    const QString old = target.oldName();
    auto add = [&](const Token &t, bool suggested, const QString &reason) {
        RenameMatch m;
        m.start = t.start;
        m.length = t.length;
        m.line = lineOf(text, t.start, &lineStarts);
        m.replacement = replacementFor(t, target.newName, target.dialect);
        m.suggested = suggested;
        m.reason = reason;
        matches.append(m);
    };
    // 문자열 안: 동적 SQL일 수 있다 (기본으로 바꾸지 않는다)
    const QRegularExpression inString(QStringLiteral("(?<![\\w$#])%1(?![\\w$#])").arg(QRegularExpression::escape(old)),
                                      QRegularExpression::CaseInsensitiveOption);
    auto addStringMatches = [&](const Token &t) {
        const QString literal = text.mid(t.start, t.length);
        auto it = inString.globalMatch(literal);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            Token word{Token::Word, t.start + int(m.capturedStart()), int(m.capturedLength()), m.captured(), {}};
            add(word, false, QObject::tr("inside a string (dynamic SQL?)"));
        }
    };

    if (!target.isColumn()) {
        for (int i = 0; i < tokens.size(); ++i) {
            const Token &t = tokens.at(i);
            if (t.kind == Token::String) {
                addStringMatches(t);
                continue;
            }
            if (!isName(t) || !same(t, target.table))
                continue;
            const bool qualified = i >= 2 && tokens.at(i - 1).kind == Token::Dot && isName(tokens.at(i - 2));
            const bool qualifier = i + 2 < tokens.size() && tokens.at(i + 1).kind == Token::Dot && isName(tokens.at(i + 2));
            if (qualified) {
                // 스키마.테이블: 같은 스키마일 때만. 다른 한정자면 다른 스키마 테이블이거나 같은 이름의 컬럼이다
                if (target.schema.isEmpty() || !same(tokens.at(i - 2), target.schema))
                    continue;
                add(t, true, QObject::tr("qualified by the schema"));
            } else if (qualifier) {
                add(t, true, QObject::tr("qualifies a column"));
            } else {
                add(t, true, QString());
            }
        }
        return matches;
    }

    // 컬럼: 그 테이블을 쓰는 문장 안에서만
    const QList<SqlStatement> statements = SqlSplitter::split(text, target.dialect);
    int statementIndex = 0;
    int ti = 0;
    for (; statementIndex < statements.size(); ++statementIndex) {
        const SqlStatement &statement = statements.at(statementIndex);
        // 이 문장의 낱말 [first, last)
        while (ti < tokens.size() && tokens.at(ti).start < statement.start)
            ++ti;
        const int first = ti;
        while (ti < tokens.size() && tokens.at(ti).start < statement.terminatorEnd)
            ++ti;
        const int last = ti;

        // 테이블을 쓰는 곳과 별칭
        bool usesTable = false;
        QSet<QString> qualifiers; // 대문자로 (따옴표 없는 별칭)
        qualifiers.insert(QStringLiteral("NEW"));
        qualifiers.insert(QStringLiteral("OLD"));
        for (int i = first; i < last; ++i) {
            const Token &t = tokens.at(i);
            if (!isName(t) || !same(t, target.table))
                continue;
            const bool qualified = i - 2 >= first && tokens.at(i - 1).kind == Token::Dot && isName(tokens.at(i - 2));
            if (qualified && (target.schema.isEmpty() || !same(tokens.at(i - 2), target.schema)))
                continue; // 다른 스키마
            usesTable = true;
            if (i + 1 < last && tokens.at(i + 1).kind == Token::Dot)
                continue; // emp.col·emp.col%TYPE: 한정자로 쓴 곳 (뒤에 별칭이 오지 않는다)
            qualifiers.insert(t.value.toUpper());
            int j = i + 1;
            if (j < last && tokens.at(j).kind == Token::Word && tokens.at(j).value.compare(QLatin1String("AS"), Qt::CaseInsensitive) == 0)
                ++j;
            if (j < last && isName(tokens.at(j)) && !stopWords().contains(tokens.at(j).value.toUpper())
                && !(j + 1 < last && tokens.at(j + 1).kind == Token::Dot))
                qualifiers.insert(tokens.at(j).kind == Token::Quoted ? tokens.at(j).value : tokens.at(j).value.toUpper());
        }
        // 테이블 이름 자체도 한정자로 받는다 (emp.dept_id)
        qualifiers.insert(target.table.toUpper());
        if (!usesTable)
            continue;

        for (int i = first; i < last; ++i) {
            const Token &t = tokens.at(i);
            if (t.kind == Token::String) {
                addStringMatches(t);
                continue;
            }
            if (!isName(t) || !same(t, target.column))
                continue;
            // 뒤에 . 이 오면 컬럼이 아니라 한정자다 (같은 이름의 별칭·테이블)
            if (i + 1 < last && tokens.at(i + 1).kind == Token::Dot)
                continue;
            if (i - 2 >= first && tokens.at(i - 1).kind == Token::Dot && isName(tokens.at(i - 2))) {
                const Token &q = tokens.at(i - 2);
                const QString key = q.kind == Token::Quoted ? q.value : q.value.toUpper();
                if (qualifiers.contains(key) || qualifiers.contains(q.value.toUpper()))
                    add(t, true, QObject::tr("qualified by %1").arg(q.value));
                else
                    add(t, false, QObject::tr("qualified by %1 (another table?)").arg(q.value));
            } else {
                add(t, true, QObject::tr("not qualified"));
            }
        }
    }
    return matches;
}

QString SqlRename::apply(const QString &text, const QList<RenameMatch> &matches)
{
    QList<RenameMatch> sorted = matches;
    std::sort(sorted.begin(), sorted.end(), [](const RenameMatch &a, const RenameMatch &b) { return a.start > b.start; });
    QString result = text;
    int limit = int(result.size()) + 1;
    for (const RenameMatch &m : std::as_const(sorted)) {
        if (m.start + m.length > limit)
            continue; // 겹치면 건너뛴다
        result.replace(m.start, m.length, m.replacement);
        limit = m.start;
    }
    return result;
}

QString SqlRename::storedName(const QString &typed, SqlDialect dialect)
{
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z][A-Za-z0-9_$#]*$"));
    const QString name = typed.trimmed();
    if (!plain.match(name).hasMatch())
        return name; // 따옴표로 감싸야 하는 이름: 쓴 그대로
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

QString SqlRename::validate(const QString &typed)
{
    const QString name = typed.trimmed();
    if (name.isEmpty())
        return QObject::tr("Enter the new name.");
    for (const QChar c : name) {
        if (c == u'"' || c == u'`' || c.category() == QChar::Other_Control)
            return QObject::tr("The name cannot contain quotes or control characters.");
    }
    if (name.size() > 128)
        return QObject::tr("The name is too long.");
    return {};
}

QString SqlRename::renameStatement(const RenameTarget &target)
{
    const SqlDialect d = target.dialect;
    const QString table = SqlIdentifiers::qualified(target.schema, target.table, d);
    const QString fresh = SqlIdentifiers::quote(target.newName, d);
    if (d == SqlDialect::MySql) {
        if (target.isColumn())
            return QStringLiteral("ALTER TABLE %1 RENAME COLUMN %2 TO %3;").arg(table, SqlIdentifiers::quote(target.column, d), fresh);
        return QStringLiteral("RENAME TABLE %1 TO %2;").arg(table, SqlIdentifiers::qualified(target.schema, target.newName, d));
    }
    if (target.isColumn())
        return QStringLiteral("ALTER TABLE %1 RENAME COLUMN %2 TO %3;").arg(table, SqlIdentifiers::quote(target.column, d), fresh);
    return QStringLiteral("ALTER TABLE %1 RENAME TO %2;").arg(table, fresh);
}

bool SqlRename::mentions(const QString &text, const QString &name)
{
    if (name.isEmpty())
        return false;
    qsizetype from = 0;
    while (true) {
        const qsizetype at = text.indexOf(name, from, Qt::CaseInsensitive);
        if (at < 0)
            return false;
        const qsizetype end = at + name.size();
        const bool before = at == 0 || !wordChar(text.at(at - 1));
        const bool after = end >= text.size() || !wordChar(text.at(end));
        if (before && after)
            return true;
        from = at + 1;
    }
}
