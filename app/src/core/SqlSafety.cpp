#include "SqlSafety.h"

#include <QList>
#include <QSet>

namespace {

struct Word
{
    QString text;  // 대문자
    int depth = 0; // 괄호 깊이
};

// 주석·문자열·인용 식별자를 건너뛰며 단어와 괄호 깊이를 모은다.
QList<Word> words(const QString &sql)
{
    QList<Word> result;
    const int n = sql.size();
    int depth = 0;
    int i = 0;
    while (i < n) {
        const QChar c = sql[i];
        const QChar next = i + 1 < n ? sql[i + 1] : QChar();
        if (c == u'-' && next == u'-') {
            const int end = sql.indexOf(u'\n', i);
            i = end < 0 ? n : end;
        } else if (c == u'/' && next == u'*') {
            const int end = sql.indexOf(QStringLiteral("*/"), i + 2);
            i = end < 0 ? n : end + 2;
        } else if (c == u'\'' || c == u'"' || c == u'`') {
            int j = i + 1;
            while (j < n) {
                if (sql[j] == c) {
                    if (j + 1 < n && sql[j + 1] == c) {
                        j += 2;
                        continue;
                    }
                    break;
                }
                ++j;
            }
            i = j + 1;
        } else if (c == u'(') {
            ++depth;
            ++i;
        } else if (c == u')') {
            depth = qMax(0, depth - 1);
            ++i;
        } else if (c.isLetter() || c == u'_') {
            int j = i + 1;
            while (j < n && (sql[j].isLetterOrNumber() || sql[j] == u'_' || sql[j] == u'$' || sql[j] == u'#'))
                ++j;
            result.append({sql.mid(i, j - i).toUpper(), depth});
            i = j;
        } else {
            ++i;
        }
    }
    return result;
}

QString leadingKeywordOf(const QList<Word> &list)
{
    if (list.isEmpty())
        return {};
    if (list.first().text != u"WITH")
        return list.first().text;
    // WITH a AS (...), b AS (...) <본문>: 괄호 밖에서 AS 다음이 아닌 첫 키워드
    static const QSet<QString> bodies{QStringLiteral("SELECT"), QStringLiteral("INSERT"), QStringLiteral("UPDATE"),
                                      QStringLiteral("DELETE"), QStringLiteral("MERGE")};
    for (int k = 1; k < list.size(); ++k) {
        if (list[k].depth == 0 && bodies.contains(list[k].text))
            return list[k].text;
    }
    return list.first().text;
}

} // namespace

QString SqlSafety::leadingKeyword(const QString &sql)
{
    return leadingKeywordOf(words(sql));
}

bool SqlSafety::mayModify(const QString &sql)
{
    static const QSet<QString> modifying{
        QStringLiteral("INSERT"),   QStringLiteral("UPDATE"),  QStringLiteral("DELETE"), QStringLiteral("MERGE"),
        QStringLiteral("UPSERT"),   QStringLiteral("REPLACE"), QStringLiteral("CREATE"), QStringLiteral("ALTER"),
        QStringLiteral("DROP"),     QStringLiteral("TRUNCATE"), QStringLiteral("RENAME"), QStringLiteral("GRANT"),
        QStringLiteral("REVOKE"),   QStringLiteral("COMMENT"), QStringLiteral("BEGIN"),  QStringLiteral("DECLARE"),
        QStringLiteral("CALL"),     QStringLiteral("EXEC"),    QStringLiteral("EXECUTE"), QStringLiteral("DO"),
        QStringLiteral("LOCK"),     QStringLiteral("LOAD"),    QStringLiteral("COPY"),   QStringLiteral("VACUUM"),
    };
    return modifying.contains(leadingKeyword(sql));
}

bool SqlSafety::hasWriteKeyword(const QString &sql)
{
    static const QSet<QString> writes{
        QStringLiteral("INSERT"), QStringLiteral("UPDATE"),   QStringLiteral("DELETE"), QStringLiteral("MERGE"),
        QStringLiteral("UPSERT"), QStringLiteral("INTO"),     QStringLiteral("CREATE"), QStringLiteral("ALTER"),
        QStringLiteral("DROP"),   QStringLiteral("TRUNCATE"), QStringLiteral("GRANT"),  QStringLiteral("REVOKE"),
    };
    for (const Word &word : words(sql)) {
        if (writes.contains(word.text))
            return true;
    }
    return false;
}

bool SqlSafety::isDml(const QString &sql)
{
    static const QSet<QString> dml{QStringLiteral("INSERT"), QStringLiteral("UPDATE"), QStringLiteral("DELETE"),
                                   QStringLiteral("MERGE"), QStringLiteral("UPSERT"), QStringLiteral("REPLACE")};
    return dml.contains(leadingKeyword(sql));
}

bool SqlSafety::isUnfilteredUpdateOrDelete(const QString &sql)
{
    const QList<Word> list = words(sql);
    const QString keyword = leadingKeywordOf(list);
    if (keyword != u"UPDATE" && keyword != u"DELETE")
        return false;
    for (const Word &word : list) {
        if (word.depth == 0 && word.text == u"WHERE")
            return false;
    }
    return true;
}

bool SqlSafety::isDestructiveDdl(const QString &sql)
{
    const QString keyword = leadingKeyword(sql);
    return keyword == u"DROP" || keyword == u"TRUNCATE";
}
