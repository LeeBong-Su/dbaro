#include "SqlIdentifiers.h"

#include <QRegularExpression>

QString SqlIdentifiers::quote(const QString &name, SqlDialect dialect)
{
    static const QRegularExpression upper(QStringLiteral("^[A-Z][A-Z0-9_$#]*$"));
    static const QRegularExpression lower(QStringLiteral("^[a-z_][a-z0-9_$]*$"));
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z_][A-Za-z0-9_$]*$"));

    switch (dialect) {
    case SqlDialect::MySql:
        if (plain.match(name).hasMatch())
            return name;
        return QStringLiteral("`%1`").arg(QString(name).replace(u'`', QStringLiteral("``")));
    case SqlDialect::PostgreSql:
        if (lower.match(name).hasMatch())
            return name;
        break;
    case SqlDialect::Oracle:
    case SqlDialect::Generic:
        if (upper.match(name).hasMatch())
            return name;
        break;
    }
    return QStringLiteral("\"%1\"").arg(QString(name).replace(u'"', QStringLiteral("\"\"")));
}

std::optional<QualifiedName> SqlIdentifiers::parseQualified(const QString &text)
{
    struct Part
    {
        QString value;
        bool quoted = false;
    };
    QList<Part> parts;
    const QString s = text.trimmed();
    int i = 0;
    const int n = int(s.size());
    auto wordChar = [](QChar c) { return c.isLetterOrNumber() || c == u'_' || c == u'$' || c == u'#'; };
    while (i < n) {
        Part part;
        const QChar c = s.at(i);
        if (c == u'"' || c == u'`' || c == u'[') {
            const QChar close = c == u'[' ? QChar(u']') : c;
            ++i;
            while (i < n) {
                if (s.at(i) == close) {
                    if (close != u']' && i + 1 < n && s.at(i + 1) == close) {
                        part.value += close;
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                part.value += s.at(i++);
            }
            part.quoted = true;
        } else if (wordChar(c)) {
            while (i < n && wordChar(s.at(i)))
                part.value += s.at(i++);
        } else {
            break;
        }
        if (part.value.isEmpty())
            break;
        parts.append(part);
        if (i < n && s.at(i) == u'.')
            ++i;
        else
            break;
    }
    if (parts.isEmpty())
        return std::nullopt;

    QualifiedName result;
    result.name = parts.last().value;
    result.nameQuoted = parts.last().quoted;
    if (parts.size() >= 2) {
        result.schema = parts.at(parts.size() - 2).value;
        result.schemaQuoted = parts.at(parts.size() - 2).quoted;
    }
    return result;
}

QString SqlIdentifiers::qualified(const QString &schema, const QString &name, SqlDialect dialect)
{
    if (schema.isEmpty())
        return quote(name, dialect);
    return quote(schema, dialect) + u'.' + quote(name, dialect);
}
