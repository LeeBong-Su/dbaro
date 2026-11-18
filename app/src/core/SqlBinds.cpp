#include "SqlBinds.h"

#include <QRegularExpression>

namespace {

bool isNameStart(QChar c)
{
    return c.isLetter() || c == u'_';
}

bool isNameChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$' || c == u'#';
}

// 닫는 따옴표 다음 위치 (같은 따옴표 두 개는 이스케이프, backslash면 \ 도 이스케이프)
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

// 주석을 건너뛴 앞쪽 단어들 (대문자)
QStringList leadingWords(const QString &s, int count)
{
    QStringList words;
    int i = 0;
    while (i < s.size() && words.size() < count) {
        const QChar c = s[i];
        if (c.isSpace()) {
            ++i;
        } else if (s.mid(i, 2) == u"--") {
            const int end = s.indexOf(u'\n', i);
            i = end < 0 ? int(s.size()) : end + 1;
        } else if (s.mid(i, 2) == u"/*") {
            const int end = s.indexOf(QStringLiteral("*/"), i + 2);
            i = end < 0 ? int(s.size()) : end + 2;
        } else if (isNameStart(c)) {
            int j = i + 1;
            while (j < s.size() && isNameChar(s[j]))
                ++j;
            words << s.mid(i, j - i).toUpper();
            i = j;
        } else {
            break;
        }
    }
    return words;
}

// 트리거·프로시저 등 PL/SQL(또는 저장 루틴) 본문을 만드는 문장. 본문의 :NEW, :OLD, :x 는 바인드가 아니다.
bool createsRoutine(const QString &sql)
{
    const QStringList head = leadingWords(sql, 6);
    if (head.isEmpty() || head[0] != u"CREATE")
        return false;
    for (const QString &word : head) {
        if (word == u"PROCEDURE" || word == u"FUNCTION" || word == u"PACKAGE" || word == u"TRIGGER" || word == u"TYPE"
            || word == u"LIBRARY")
            return true;
    }
    return false;
}

int variableIndex(QList<BindVariable> *variables, BindVariable::Kind kind, const QString &name)
{
    for (int i = 0; i < variables->size(); ++i) {
        const BindVariable &variable = variables->at(i);
        if (variable.kind == kind && variable.name.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    }
    BindVariable variable;
    variable.kind = kind;
    variable.name = name;
    variables->append(variable);
    return int(variables->size()) - 1;
}

QString quoted(const QString &text, SqlDialect dialect)
{
    QString escaped = text;
    if (dialect == SqlDialect::MySql)
        escaped.replace(u'\\', QStringLiteral("\\\\"));
    escaped.replace(u'\'', QStringLiteral("''"));
    return u'\'' + escaped + u'\'';
}

} // namespace

QString BindVariable::label() const
{
    switch (kind) {
    case Kind::Named: return u':' + name;
    case Kind::Substitution: return (define ? QStringLiteral("&&") : QStringLiteral("&")) + name;
    case Kind::Positional: return u'?' + name;
    }
    return name;
}

QString BindVariable::key() const
{
    switch (kind) {
    case Kind::Named: return u':' + name.toUpper();
    case Kind::Substitution: return u'&' + name.toUpper();
    case Kind::Positional: return u'?' + name;
    }
    return name;
}

QList<BindOccurrence> SqlBinds::scan(const QString &s, SqlDialect dialect, QList<BindVariable> *variables,
                                     int *positionalCounter)
{
    QList<BindOccurrence> found;
    if (createsRoutine(s))
        return found;

    const bool oracle = dialect == SqlDialect::Oracle;
    const bool mysql = dialect == SqlDialect::MySql;
    const bool postgres = dialect == SqlDialect::PostgreSql;
    const int n = int(s.size());
    int bracketDepth = 0; // PostgreSQL 배열 조각 arr[1:2]

    int i = 0;
    while (i < n) {
        const QChar c = s[i];
        const QChar next = i + 1 < n ? s[i + 1] : QChar();

        // 주석
        if ((c == u'-' && next == u'-') || (mysql && c == u'#')) {
            const int end = s.indexOf(u'\n', i);
            i = end < 0 ? n : end + 1;
            continue;
        }
        if (c == u'/' && next == u'*') {
            const int end = s.indexOf(QStringLiteral("*/"), i + 2);
            i = end < 0 ? n : end + 2;
            continue;
        }
        // 문자열과 인용 식별자
        if (c == u'\'') {
            const bool backslash = mysql || (postgres && i > 0 && (s[i - 1] == u'E' || s[i - 1] == u'e'));
            i = skipQuoted(s, i, u'\'', backslash);
            continue;
        }
        if (c == u'"' || (mysql && c == u'`')) {
            i = skipQuoted(s, i, c, false);
            continue;
        }
        if (postgres && c == u'$') {
            int j = i + 1;
            while (j < n && (s[j].isLetterOrNumber() || s[j] == u'_'))
                ++j;
            if (j < n && s[j] == u'$' && !(j > i + 1 && s[i + 1].isDigit())) {
                const QString tag = s.mid(i, j - i + 1);
                const int end = s.indexOf(tag, j + 1);
                i = end < 0 ? n : end + int(tag.size());
                continue;
            }
            i = j;
            continue;
        }
        if (postgres && c == u'[') {
            ++bracketDepth;
            ++i;
            continue;
        }
        if (postgres && c == u']') {
            bracketDepth = qMax(0, bracketDepth - 1);
            ++i;
            continue;
        }
        // 이름 안의 글자 (a:b 같은 것은 바인드가 아니다)
        if (isNameStart(c) || c.isDigit()) {
            int j = i + 1;
            while (j < n && isNameChar(s[j]))
                ++j;
            // Oracle q'[...]' / nq'[...]' 문자열
            const QStringView word = QStringView(s).mid(i, j - i);
            if (oracle && j + 1 < n && s[j] == u'\''
                && (word.compare(u"q", Qt::CaseInsensitive) == 0 || word.compare(u"nq", Qt::CaseInsensitive) == 0)) {
                const QString closer = QString(closingFor(s[j + 1])) + u'\'';
                const int end = s.indexOf(closer, j + 2);
                i = end < 0 ? n : end + int(closer.size());
                continue;
            }
            i = j;
            continue;
        }

        // :name, :1  (::type 변환, := 대입, 배열 조각은 제외)
        if (c == u':' && bracketDepth == 0 && (i == 0 || s[i - 1] != u':') && next != u':' && next != u'='
            && (isNameStart(next) || next.isDigit())) {
            int j = i + 1;
            while (j < n && isNameChar(s[j]))
                ++j;
            const int index = variableIndex(variables, BindVariable::Kind::Named, s.mid(i + 1, j - i - 1));
            found.append({i, j - i, index});
            i = j;
            continue;
        }
        // ?
        if (c == u'?' && !postgres) {
            const int number = ++*positionalCounter;
            BindVariable variable;
            variable.kind = BindVariable::Kind::Positional;
            variable.name = QString::number(number);
            variables->append(variable);
            found.append({i, 1, int(variables->size()) - 1});
            ++i;
            continue;
        }
        // &name, &&name  (끝의 . 은 이름과 뒤 글자를 이어 붙일 때 쓰고 함께 사라진다)
        if (oracle && c == u'&') {
            const bool define = next == u'&';
            const int nameStart = i + (define ? 2 : 1);
            if (nameStart < n && isNameStart(s[nameStart])) {
                int j = nameStart + 1;
                while (j < n && isNameChar(s[j]))
                    ++j;
                const int index = variableIndex(variables, BindVariable::Kind::Substitution, s.mid(nameStart, j - nameStart));
                if (define)
                    (*variables)[index].define = true;
                if (j < n && s[j] == u'.')
                    ++j;
                found.append({i, j - i, index});
                i = j;
                continue;
            }
        }
        ++i;
    }
    return found;
}

QString SqlBinds::literal(const BindVariable &variable, const Value &value, SqlDialect dialect)
{
    if (variable.kind == BindVariable::Kind::Substitution)
        return value.text;
    switch (value.type) {
    case Value::Type::Raw:
        return value.text;
    case Value::Type::Null:
        return QStringLiteral("NULL");
    case Value::Type::Text:
        return quoted(value.text, dialect);
    case Value::Type::Auto:
        break;
    }
    static const QRegularExpression number(QStringLiteral("^[+-]?(\\d+(\\.\\d*)?|\\.\\d+)([eE][+-]?\\d+)?$"));
    const QString trimmed = value.text.trimmed();
    if (number.match(trimmed).hasMatch())
        return trimmed;
    if (trimmed.compare(QStringLiteral("null"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("NULL");
    return quoted(value.text, dialect);
}

QString SqlBinds::substitute(const QString &sql, const QList<BindOccurrence> &occurrences, const QStringList &replacements,
                             QList<SqlEdit> *edits)
{
    QString out;
    int last = 0;
    for (const BindOccurrence &occurrence : occurrences) {
        out += QStringView(sql).mid(last, occurrence.start - last);
        const QString value = replacements.value(occurrence.variable);
        if (edits)
            edits->append({int(out.size()), occurrence.length, int(value.size())});
        out += value;
        last = occurrence.start + occurrence.length;
    }
    out += QStringView(sql).mid(last);
    return out;
}
