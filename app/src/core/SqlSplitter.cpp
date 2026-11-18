#include "SqlSplitter.h"

#include <QStringList>
#include <QStringView>

namespace {

bool isWordStart(QChar c)
{
    return c.isLetter() || c == u'_';
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$' || c == u'#';
}

int lineEnd(const QString &s, int from)
{
    const int index = s.indexOf(u'\n', from);
    return index < 0 ? s.size() : index;
}

// 따옴표로 감싼 부분을 건너뛴다. 닫는 따옴표 다음 위치를 돌려준다 (없으면 끝).
// 같은 따옴표 두 개('')는 이스케이프로 본다. backslash가 true면 \' 도 이스케이프다.
int skipQuoted(const QString &s, int open, QChar quote, bool backslash)
{
    const int n = s.size();
    int j = open + 1;
    while (j < n) {
        const QChar c = s[j];
        if (backslash && c == u'\\') {
            j += 2;
            continue;
        }
        if (c == quote) {
            if (j + 1 < n && s[j + 1] == quote) {
                j += 2;
                continue;
            }
            return j + 1;
        }
        ++j;
    }
    return n;
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

// Oracle: 이 단어들로 시작하는 문장은 PL/SQL 블록이라 ';'가 아니라 '/' 줄에서 끝난다.
bool startsPlsqlBlock(const QStringList &head)
{
    if (head.isEmpty())
        return false;
    if (head[0] == u"DECLARE" || head[0] == u"BEGIN")
        return true;
    if (head[0] != u"CREATE")
        return false;

    auto at = [&head](int index) { return index < head.size() ? head[index] : QString(); };
    int k = 1;
    if (at(k) == u"OR" && at(k + 1) == u"REPLACE")
        k += 2;
    while (at(k) == u"EDITIONABLE" || at(k) == u"NONEDITIONABLE")
        ++k;
    const QString kind = at(k);
    return kind == u"PROCEDURE" || kind == u"FUNCTION" || kind == u"PACKAGE" || kind == u"TRIGGER"
           || kind == u"TYPE" || kind == u"LIBRARY";
}

// SQL*Plus·psql처럼 줄 끝에서 끝나는 클라이언트 명령 (DESC[RIBE]). 엔진이 직접 처리한다.
bool isLineCommand(QStringView word)
{
    static const QStringList commands{QStringLiteral("DESC"), QStringLiteral("DESCR"), QStringLiteral("DESCRI"),
                                      QStringLiteral("DESCRIB"), QStringLiteral("DESCRIBE")};
    for (const QString &command : commands) {
        if (word.compare(command, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

} // namespace

QList<SqlStatement> SqlSplitter::split(const QString &s, SqlDialect dialect)
{
    QList<SqlStatement> result;
    const int n = s.size();
    const bool oracle = dialect == SqlDialect::Oracle;
    const bool mysql = dialect == SqlDialect::MySql;
    const bool postgres = dialect == SqlDialect::PostgreSql;
    const bool slashLines = oracle || dialect == SqlDialect::Generic;

    QString delimiter = QStringLiteral(";"); // MySQL은 DELIMITER로 바뀐다
    int stmtStart = -1;
    bool hasCode = false;
    bool plsqlBlock = false;
    bool lineCommand = false; // DESC: ';' 없이 줄 끝에서 끝난다 (MySQL의 DESC는 서버 SQL이라 제외)
    QStringList head; // 문장 앞쪽 단어들 (대문자)

    auto finish = [&](int end, int terminatorEnd) {
        if (stmtStart >= 0 && hasCode) {
            int e = end;
            while (e > stmtStart && s[e - 1].isSpace())
                --e;
            SqlStatement statement;
            statement.start = stmtStart;
            statement.end = e;
            statement.terminatorEnd = terminatorEnd;
            statement.text = s.mid(stmtStart, e - stmtStart);
            result.append(statement);
        }
        stmtStart = -1;
        hasCode = false;
        plsqlBlock = false;
        lineCommand = false;
        head.clear();
    };
    auto markStart = [&](int pos) {
        if (stmtStart < 0)
            stmtStart = pos;
    };
    auto markCode = [&](int pos) {
        markStart(pos);
        hasCode = true;
    };

    bool atLineStart = true;
    int i = 0;
    while (i < n) {
        // 줄 단위 명령은 줄 시작에서만 확인한다 (문자열·주석 밖일 때만 여기에 온다).
        if (atLineStart) {
            atLineStart = false;
            const int end = lineEnd(s, i);
            const QStringView line = QStringView(s).mid(i, end - i).trimmed();

            if (slashLines && line == u"/") {
                finish(i, end);
                i = end;
                continue;
            }
            if (mysql && !hasCode && line.startsWith(u"DELIMITER", Qt::CaseInsensitive)
                && (line.size() == 9 || line[9].isSpace())) {
                const QString newDelimiter = line.mid(9).trimmed().toString();
                if (!newDelimiter.isEmpty())
                    delimiter = newDelimiter;
                stmtStart = -1;
                head.clear();
                i = end;
                continue;
            }
        }

        const QChar c = s[i];
        const QChar next = i + 1 < n ? s[i + 1] : QChar();

        if (c == u'\n') {
            if (lineCommand)
                finish(i, i);
            atLineStart = true;
            ++i;
            continue;
        }
        if (c.isSpace()) {
            ++i;
            continue;
        }

        // 주석
        if ((c == u'-' && next == u'-') || (mysql && c == u'#')) {
            markStart(i);
            i = lineEnd(s, i);
            continue;
        }
        if (c == u'/' && next == u'*') {
            markStart(i);
            const int close = s.indexOf(QStringLiteral("*/"), i + 2);
            i = close < 0 ? n : close + 2;
            continue;
        }

        // 구분자 (PL/SQL 블록 안에서는 '/' 줄까지 계속)
        if (!plsqlBlock) {
            if (mysql) {
                if (QStringView(s).mid(i, delimiter.size()) == delimiter) {
                    finish(i, i + delimiter.size());
                    i += delimiter.size();
                    continue;
                }
            } else if (c == u';') {
                finish(i, i + 1);
                ++i;
                continue;
            }
        }

        // 문자열과 인용 식별자
        if (c == u'\'') {
            markCode(i);
            i = skipQuoted(s, i, u'\'', mysql);
            continue;
        }
        if (c == u'"') {
            markCode(i);
            i = skipQuoted(s, i, u'"', false);
            continue;
        }
        if (mysql && c == u'`') {
            markCode(i);
            i = skipQuoted(s, i, u'`', false);
            continue;
        }
        if (postgres && c == u'$') {
            int j = i + 1;
            while (j < n && (s[j].isLetterOrNumber() || s[j] == u'_'))
                ++j;
            const bool isTag = j < n && s[j] == u'$' && !(j > i + 1 && s[i + 1].isDigit());
            markCode(i);
            if (isTag) {
                const QString tag = s.mid(i, j - i + 1); // $$ 또는 $tag$
                const int close = s.indexOf(tag, j + 1);
                i = close < 0 ? n : close + tag.size();
            } else {
                i = j; // $1 같은 파라미터
            }
            continue;
        }

        // 단어
        if (isWordStart(c)) {
            // MySQL 클라이언트처럼 단어 중간이라도 구분자(END$$ 등)에서 끊는다.
            int j = i + 1;
            while (j < n && isWordChar(s[j]) && !(mysql && QStringView(s).mid(j, delimiter.size()) == delimiter))
                ++j;
            const QStringView word = QStringView(s).mid(i, j - i);
            if (!hasCode && (oracle || postgres))
                lineCommand = isLineCommand(word);
            markCode(i);

            // Oracle q'[...]' / nq'[...]' 문자열
            if (oracle && j + 1 < n && s[j] == u'\''
                && (word.compare(u"q", Qt::CaseInsensitive) == 0 || word.compare(u"nq", Qt::CaseInsensitive) == 0)) {
                const QString closer = QString(closingFor(s[j + 1])) + u'\'';
                const int close = s.indexOf(closer, j + 2);
                i = close < 0 ? n : close + closer.size();
                continue;
            }
            // PostgreSQL E'...' 문자열 (백슬래시 이스케이프)
            if (postgres && j < n && s[j] == u'\'' && word.compare(u"E", Qt::CaseInsensitive) == 0) {
                i = skipQuoted(s, j, u'\'', true);
                continue;
            }

            if (oracle && head.size() < 6) {
                head.append(word.toString().toUpper());
                plsqlBlock = plsqlBlock || startsPlsqlBlock(head);
            }
            i = j;
            continue;
        }

        markCode(i);
        ++i;
    }

    finish(n, n);
    return result;
}

int SqlSplitter::statementIndexAt(const QList<SqlStatement> &statements, int position)
{
    if (statements.isEmpty())
        return -1;

    int candidate = 0;
    for (int k = 0; k < statements.size(); ++k) {
        const SqlStatement &statement = statements[k];
        if (position < statement.start)
            break;
        candidate = k;
        if (position <= statement.terminatorEnd)
            return k;
    }
    return candidate;
}
