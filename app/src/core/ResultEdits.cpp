#include "ResultEdits.h"

#include <QSet>

#include <algorithm>

namespace {

struct Word
{
    QString text;  // 원래 글 (인용 식별자는 따옴표 포함)
    QString upper; // 대문자 (인용 식별자는 그대로)
    bool quoted = false;
    bool punct = false; // ( ) , . ; 같은 기호
    int depth = 0;      // 괄호 깊이
};

// 문자열·주석을 건너뛴 단어와 기호
QList<Word> words(const QString &s, SqlDialect dialect)
{
    QList<Word> out;
    int depth = 0;
    int i = 0;
    const int n = int(s.size());
    while (i < n) {
        const QChar c = s[i];
        if (c.isSpace()) {
            ++i;
        } else if (s.mid(i, 2) == u"--" || (dialect == SqlDialect::MySql && c == u'#')) {
            const int end = s.indexOf(u'\n', i);
            i = end < 0 ? n : end + 1;
        } else if (s.mid(i, 2) == u"/*") {
            const int end = s.indexOf(QStringLiteral("*/"), i + 2);
            i = end < 0 ? n : end + 2;
        } else if (c == u'\'') {
            int j = i + 1;
            while (j < n && !(s[j] == u'\'' && (j + 1 >= n || s[j + 1] != u'\'')))
                j += (s[j] == u'\'' ? 2 : 1);
            i = j + 1;
            out.append({QStringLiteral("''"), QStringLiteral("''"), false, true, depth}); // 문자열 자리
        } else if (c == u'"' || (dialect == SqlDialect::MySql && c == u'`')) {
            int j = i + 1;
            while (j < n && s[j] != c)
                ++j;
            const QString name = s.mid(i + 1, j - i - 1);
            out.append({s.mid(i, j - i + 1), name, true, false, depth});
            i = j + 1;
        } else if (c.isLetter() || c == u'_' || c == u'$') {
            int j = i + 1;
            while (j < n && (s[j].isLetterOrNumber() || s[j] == u'_' || s[j] == u'$' || s[j] == u'#'))
                ++j;
            const QString word = s.mid(i, j - i);
            out.append({word, word.toUpper(), false, false, depth});
            i = j;
        } else {
            if (c == u')')
                depth = qMax(0, depth - 1);
            out.append({QString(c), QString(c), false, true, depth});
            if (c == u'(')
                ++depth;
            ++i;
        }
    }
    return out;
}

bool isWord(const Word &w, const char *text)
{
    return !w.quoted && !w.punct && w.upper == QLatin1String(text);
}

QString columnName(const QString &name, SqlDialect dialect)
{
    return SqlIdentifiers::quote(name, dialect);
}

} // namespace

std::optional<QualifiedName> ResultEdits::editTarget(const QString &sql, SqlDialect dialect)
{
    const QList<Word> list = words(sql, dialect);
    if (list.isEmpty() || !isWord(list.first(), "SELECT"))
        return std::nullopt;
    static const QSet<QString> blocked{QStringLiteral("DISTINCT"), QStringLiteral("UNIQUE"), QStringLiteral("UNION"),
                                       QStringLiteral("INTERSECT"), QStringLiteral("MINUS"), QStringLiteral("EXCEPT"),
                                       QStringLiteral("GROUP"), QStringLiteral("HAVING"), QStringLiteral("CONNECT"),
                                       QStringLiteral("JOIN"), QStringLiteral("PIVOT"), QStringLiteral("UNPIVOT")};
    int from = -1;
    for (int k = 0; k < list.size(); ++k) {
        const Word &w = list[k];
        if (w.depth != 0 || w.quoted || w.punct)
            continue;
        if (blocked.contains(w.upper))
            return std::nullopt; // 집계·조인·합집합 결과는 행을 하나의 테이블 행으로 되짚을 수 없다
        if (w.upper == u"FROM" && from < 0)
            from = k;
    }
    if (from < 0 || from + 1 >= list.size())
        return std::nullopt;

    // FROM 뒤: [스키마.]테이블 [AS] [별칭] 다음은 끝이거나 WHERE / ORDER / FETCH / LIMIT / OFFSET / FOR
    int k = from + 1;
    auto namePart = [&](int index) { return index < list.size() && !list[index].punct; };
    if (!namePart(k))
        return std::nullopt; // FROM (부질의)
    QualifiedName name;
    name.name = list[k].quoted ? list[k].upper : list[k].text;
    name.nameQuoted = list[k].quoted;
    ++k;
    if (k + 1 < list.size() && list[k].text == u"." && namePart(k + 1)) {
        name.schema = name.name;
        name.schemaQuoted = name.nameQuoted;
        name.name = list[k + 1].quoted ? list[k + 1].upper : list[k + 1].text;
        name.nameQuoted = list[k + 1].quoted;
        k += 2;
    }
    static const QSet<QString> after{QStringLiteral("WHERE"), QStringLiteral("ORDER"), QStringLiteral("FETCH"),
                                     QStringLiteral("LIMIT"), QStringLiteral("OFFSET"), QStringLiteral("FOR")};
    if (k < list.size() && isWord(list[k], "AS"))
        ++k;
    if (k < list.size() && !list[k].punct && !after.contains(list[k].upper))
        ++k; // 별칭
    if (k < list.size() && list[k].text == u";")
        ++k;
    if (k < list.size() && !(list[k].depth == 0 && !list[k].quoted && after.contains(list[k].upper)))
        return std::nullopt; // 쉼표로 이은 테이블, 다른 무엇
    return name;
}

QStringList ResultEdits::statements(const QString &table, const QList<ExportColumn> &columns, const QList<bool> &editable,
                                    const QList<int> &keyColumns, const QList<RowChange> &changes, SqlDialect dialect,
                                    QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QStringList();
    };
    if (keyColumns.isEmpty())
        return fail(QStringLiteral("The table has no primary key in the result, so changed rows cannot be found."));

    auto where = [&](const QCborArray &original) {
        QStringList parts;
        for (int c : keyColumns) {
            const QCborValue value = original.at(c);
            parts << (value.isNull() || value.isUndefined()
                          ? QStringLiteral("%1 IS NULL").arg(columnName(columns[c].name, dialect))
                          : QStringLiteral("%1 = %2").arg(columnName(columns[c].name, dialect),
                                                          ResultExport::sqlLiteral(columns[c], value, dialect)));
        }
        return parts.join(QStringLiteral(" AND "));
    };

    QStringList out;
    // DELETE → UPDATE → INSERT 순서 (지운 행의 키를 새 행이 다시 쓸 수 있게)
    for (const RowChange &change : changes) {
        if (change.kind == RowChange::Kind::Delete)
            out << QStringLiteral("DELETE FROM %1 WHERE %2").arg(table, where(change.original));
    }
    for (const RowChange &change : changes) {
        if (change.kind != RowChange::Kind::Update || change.changed.isEmpty())
            continue;
        QStringList sets;
        QList<int> changed(change.changed.begin(), change.changed.end());
        std::sort(changed.begin(), changed.end());
        for (int c : changed) {
            if (c < editable.size() && !editable[c])
                continue;
            sets << QStringLiteral("%1 = %2").arg(columnName(columns[c].name, dialect),
                                                  ResultExport::sqlLiteral(columns[c], change.current.at(c), dialect));
        }
        if (!sets.isEmpty())
            out << QStringLiteral("UPDATE %1 SET %2 WHERE %3").arg(table, sets.join(QStringLiteral(", ")), where(change.original));
    }
    for (const RowChange &change : changes) {
        if (change.kind != RowChange::Kind::Insert)
            continue;
        QStringList names, values;
        for (int c = 0; c < columns.size(); ++c) {
            const QCborValue value = change.current.at(c);
            if ((c < editable.size() && !editable[c]) || value.isNull() || value.isUndefined())
                continue; // 비워 둔 컬럼은 DB 기본값을 쓴다
            names << columnName(columns[c].name, dialect);
            values << ResultExport::sqlLiteral(columns[c], value, dialect);
        }
        if (names.isEmpty())
            continue;
        out << QStringLiteral("INSERT INTO %1 (%2) VALUES (%3)").arg(table, names.join(QStringLiteral(", ")),
                                                                   values.join(QStringLiteral(", ")));
    }
    return out;
}
