#include "AiDbTools.h"

#include "MetadataService.h"
#include "QuerySession.h"
#include "ai/AiSettings.h"
#include "core/SqlSafety.h"
#include "core/SqlSplitter.h"

#include <QAbstractButton>
#include <QCborArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QRegularExpression>
#include <QWidget>

namespace {

constexpr int kMaxResultChars = 40000;
constexpr int kMaxCellChars = 300;

QJsonObject tool(const QString &name, const QString &description, const QJsonObject &properties, const QStringList &required)
{
    return QJsonObject{
        {QStringLiteral("name"), name},
        {QStringLiteral("description"), description},
        {QStringLiteral("input_schema"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("properties"), properties},
                     {QStringLiteral("required"), QJsonArray::fromStringList(required)}}},
    };
}

QJsonObject stringProperty(const QString &description)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("description"), description}};
}

QString limited(QString text)
{
    if (text.size() > kMaxResultChars)
        text = text.left(kMaxResultChars) + QStringLiteral("\n... (cut off at %1 characters)").arg(kMaxResultChars);
    return text;
}

QString cellText(const QCborValue &value)
{
    if (value.isNull() || value.isUndefined())
        return QStringLiteral("NULL");
    if (value.isByteArray())
        return QStringLiteral("<binary %1 bytes>").arg(value.toByteArray().size());
    QString text;
    if (value.isString())
        text = value.toString();
    else if (value.isInteger())
        text = QString::number(value.toInteger());
    else if (value.isDouble())
        text = QString::number(value.toDouble(), 'g', 15);
    else if (value.isBool())
        text = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    else
        text = QString::fromUtf8(QJsonDocument::fromVariant(value.toVariant()).toJson(QJsonDocument::Compact));
    text.replace(u'\n', QStringLiteral("\\n")).replace(u'\t', u' ');
    if (text.size() > kMaxCellChars)
        text = text.left(kMaxCellChars) + QStringLiteral("…");
    return text;
}

QStringList strings(const QCborValue &value)
{
    QStringList list;
    for (const QCborValue &item : value.toArray())
        list << item.toString();
    return list;
}

// 대소문자가 섞였거나 특수 문자가 있는 이름은 그대로(따옴표로 감싼 것처럼) 찾는다
bool needsExactCase(const QString &name)
{
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z_][A-Za-z0-9_$#]*$"));
    return !plain.match(name).hasMatch() || (name != name.toUpper() && name != name.toLower());
}

} // namespace

AiDbTools::AiDbTools(const Context &context, QObject *parent)
    : QObject(parent)
    , m_context(context)
{
    m_session = new QuerySession(context.engine, this);
}

AiDbTools::~AiDbTools() = default;

QJsonArray AiDbTools::definitions()
{
    const QJsonObject schema = stringProperty(QStringLiteral("Schema name (database on MySQL/MariaDB). Omit for the current schema."));
    return QJsonArray{
        tool(QStringLiteral("list_schemas"),
             QStringLiteral("List the schemas (databases on MySQL/MariaDB) of this connection and which one is current. "
                            "Call this when you don't know which schema holds the tables the user means."),
             {}, {}),
        tool(QStringLiteral("list_tables"),
             QStringLiteral("List the tables, views and routines of a schema with their comments. Call this before writing SQL "
                            "against tables you have not seen yet, or to find a table by name or comment."),
             QJsonObject{{QStringLiteral("schema"), schema},
                         {QStringLiteral("filter"), stringProperty(QStringLiteral("Case-insensitive text the name or comment must contain"))}},
             {}),
        tool(QStringLiteral("describe_table"),
             QStringLiteral("Columns (type, nullability, default, comment), primary key, foreign keys, referencing tables and "
                            "indexes of a table or view. Call this before writing SQL that uses a table, to get the exact column "
                            "names and join keys."),
             QJsonObject{{QStringLiteral("schema"), schema}, {QStringLiteral("table"), stringProperty(QStringLiteral("Table or view name"))}},
             {QStringLiteral("table")}),
        tool(QStringLiteral("get_ddl"),
             QStringLiteral("DDL of a table, view, procedure, function, package, trigger or sequence. Call this when you need "
                            "the full definition, such as a view's query or a routine's body."),
             QJsonObject{{QStringLiteral("schema"), schema}, {QStringLiteral("name"), stringProperty(QStringLiteral("Object name"))}},
             {QStringLiteral("name")}),
        tool(QStringLiteral("run_query"),
             QStringLiteral("Run ONE read-only statement (SELECT, WITH ... SELECT, SHOW, DESCRIBE) and return up to max_rows rows. "
                            "It rejects anything that could change data or structure. Use it to look at data, counts or "
                            "distributions, or to check a query before you suggest it. Keep it cheap: filter or aggregate "
                            "instead of reading whole large tables. The user may be asked to approve it."),
             QJsonObject{{QStringLiteral("sql"), stringProperty(QStringLiteral("One read-only SQL statement, without a trailing semicolon"))},
                         {QStringLiteral("max_rows"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                      {QStringLiteral("description"), QStringLiteral("Rows to return (default and upper limit set by the user)")}}}},
             {QStringLiteral("sql")}),
        tool(QStringLiteral("explain_query"),
             QStringLiteral("Estimated execution plan of one statement; the statement is not executed. Call this when the user "
                            "asks why a query is slow or how to tune it, and to check an index is used."),
             QJsonObject{{QStringLiteral("sql"), stringProperty(QStringLiteral("One SQL statement"))}}, {QStringLiteral("sql")}),
        tool(QStringLiteral("propose_sql"),
             QStringLiteral("Show SQL to the user with an 'Open in query tab' button, without running it. Use this for every "
                            "statement that changes data or structure (INSERT, UPDATE, DELETE, MERGE, CREATE, ALTER, DROP ...) "
                            "and for a finished query the user should run and keep."),
             QJsonObject{{QStringLiteral("sql"), stringProperty(QStringLiteral("The SQL script"))},
                         {QStringLiteral("title"), stringProperty(QStringLiteral("A short title for the query tab"))},
                         {QStringLiteral("explanation"), stringProperty(QStringLiteral("What it does and what to check before running it"))}},
             {QStringLiteral("sql"), QStringLiteral("title")}),
    };
}

QString AiDbTools::explainProblem(const QString &sql, SqlDialect dialect)
{
    // 계획만 본다 (실행하지 않는다). 문장 하나: PostgreSQL은 ;로 이은 다음 문장을 실행할 수 있다
    const QList<SqlStatement> statements = SqlSplitter::split(sql, dialect);
    if (statements.size() != 1)
        return tr("Only one statement can be explained at a time.");
    static const QSet<QString> explainable = {QStringLiteral("SELECT"), QStringLiteral("INSERT"), QStringLiteral("UPDATE"),
                                              QStringLiteral("DELETE"), QStringLiteral("MERGE"),  QStringLiteral("VALUES"),
                                              QStringLiteral("TABLE")};
    const QString keyword = SqlSafety::leadingKeyword(statements.first().text);
    if (!explainable.contains(keyword))
        return tr("Only SELECT, INSERT, UPDATE, DELETE and MERGE can be explained (got %1).")
            .arg(keyword.isEmpty() ? tr("an unknown statement") : keyword);
    return {};
}

QString AiDbTools::readOnlyProblem(const QString &sql, SqlDialect dialect)
{
    const QList<SqlStatement> statements = SqlSplitter::split(sql, dialect);
    if (statements.isEmpty())
        return tr("The SQL is empty.");
    if (statements.size() > 1)
        return tr("Only one statement can be run at a time.");
    const QString text = statements.first().text;
    static const QSet<QString> allowed = {QStringLiteral("SELECT"), QStringLiteral("SHOW"), QStringLiteral("DESC"),
                                          QStringLiteral("DESCRIBE"), QStringLiteral("EXPLAIN"), QStringLiteral("VALUES"),
                                          QStringLiteral("TABLE")};
    const QString keyword = SqlSafety::leadingKeyword(text);
    if (!allowed.contains(keyword) || SqlSafety::mayModify(text))
        return tr("Only read-only statements can be run (got %1). Use propose_sql for statements that change data or structure.")
            .arg(keyword.isEmpty() ? tr("an unknown statement") : keyword);
    static const QRegularExpression locking(QStringLiteral("\\bFOR\\s+(UPDATE|SHARE|NO\\s+KEY\\s+UPDATE|KEY\\s+SHARE)\\b|\\bLOCK\\s+IN\\s+SHARE\\s+MODE\\b"),
                                            QRegularExpression::CaseInsensitiveOption);
    if (locking.match(text).hasMatch())
        return tr("Locking reads (FOR UPDATE / FOR SHARE) are not run.");
    // WITH 안의 DELETE, SELECT ... INTO (PostgreSQL은 테이블을 만들고 MySQL은 서버에 파일을 쓴다).
    // SHOW CREATE TABLE은 읽기다
    if (keyword != u"SHOW" && SqlSafety::hasWriteKeyword(text))
        return tr("Only read-only statements can be run (this one writes data, e.g. WITH ... DELETE or SELECT ... INTO). "
                  "Use propose_sql instead.");
    static const QRegularExpression analyze(QStringLiteral("^\\s*EXPLAIN\\b[^;]*\\bANALY[SZ]E\\b"), QRegularExpression::CaseInsensitiveOption);
    if (analyze.match(text).hasMatch())
        return tr("EXPLAIN ANALYZE runs the statement; use explain_query for the estimated plan.");
    return {};
}

QString AiDbTools::schemaOr(const QJsonObject &input) const
{
    const QString schema = input.value(QStringLiteral("schema")).toString().trimmed();
    if (!schema.isEmpty())
        return schema;
    return m_context.currentSchema ? m_context.currentSchema() : QString();
}

void AiDbTools::metadataCall(const QString &method, const QCborMap &params, bool refresh,
                             std::function<void(const QCborMap &, const QString &)> done)
{
    if (!m_context.metadata) {
        done({}, tr("Not connected."));
        return;
    }
    m_context.metadata->request(m_context.metadataId(), method, params, std::move(done), true, refresh);
}

void AiDbTools::run(const QString &name, const QJsonObject &input, AiAgent::ToolDone done)
{
    if (name == u"list_schemas") {
        metadataCall(QStringLiteral("metaSchemas"), {}, false, [done](const QCborMap &result, const QString &error) {
            if (!error.isEmpty())
                return done(error, true);
            done(QStringLiteral("Schemas: %1\nCurrent: %2")
                     .arg(strings(result.value(QStringLiteral("names"))).join(QStringLiteral(", ")),
                          result.value(QStringLiteral("current")).toString()),
                 false);
        });
    } else if (name == u"list_tables") {
        const QString schema = schemaOr(input);
        const QString filter = input.value(QStringLiteral("filter")).toString().trimmed();
        QCborMap params;
        params.insert(QStringLiteral("schema"), schema);
        metadataCall(QStringLiteral("metaObjects"), params, false, [done, schema, filter](const QCborMap &result, const QString &error) {
            if (!error.isEmpty())
                return done(error, true);
            QStringList lines;
            int total = 0;
            for (const QCborValue &value : result.value(QStringLiteral("objects")).toArray()) {
                const QCborMap o = value.toMap();
                const QString objectName = o.value(QStringLiteral("name")).toString();
                const QString remarks = o.value(QStringLiteral("remarks")).toString();
                if (!filter.isEmpty() && !objectName.contains(filter, Qt::CaseInsensitive) && !remarks.contains(filter, Qt::CaseInsensitive))
                    continue;
                ++total;
                if (lines.size() < 400)
                    lines << QStringLiteral("%1 %2%3").arg(o.value(QStringLiteral("type")).toString(), objectName,
                                                            remarks.isEmpty() ? QString() : QStringLiteral(" -- ") + remarks.simplified());
            }
            QString text = QStringLiteral("Schema %1: %2 object(s)%3\n").arg(schema).arg(total)
                               .arg(filter.isEmpty() ? QString() : QStringLiteral(" matching \"%1\"").arg(filter));
            text += lines.join(u'\n');
            if (total > lines.size())
                text += QStringLiteral("\n... %1 more (use filter)").arg(total - lines.size());
            done(limited(text), false);
        });
    } else if (name == u"describe_table") {
        const QString table = input.value(QStringLiteral("table")).toString().trimmed();
        if (table.isEmpty())
            return done(tr("table is required."), true);
        const QString given = input.value(QStringLiteral("schema")).toString().trimmed();
        QCborMap params;
        if (!given.isEmpty())
            params.insert(QStringLiteral("schema"), given);
        params.insert(QStringLiteral("name"), table);
        params.insert(QStringLiteral("schemaQuoted"), needsExactCase(given));
        params.insert(QStringLiteral("nameQuoted"), needsExactCase(table));
        if (given.isEmpty() && !schemaOr({}).isEmpty())
            params.insert(QStringLiteral("currentSchema"), schemaOr({}));
        metadataCall(QStringLiteral("tableInfo"), params, false, [done](const QCborMap &t, const QString &error) {
            if (!error.isEmpty())
                return done(error, true);
            QStringList lines;
            lines << QStringLiteral("%1 %2.%3%4").arg(t.value(QStringLiteral("type")).toString(), t.value(QStringLiteral("schema")).toString(),
                                                      t.value(QStringLiteral("name")).toString(),
                                                      t.value(QStringLiteral("remarks")).toString().isEmpty()
                                                          ? QString()
                                                          : QStringLiteral(" -- ") + t.value(QStringLiteral("remarks")).toString());
            lines << QStringLiteral("Columns:");
            for (const QCborValue &value : t.value(QStringLiteral("columns")).toArray()) {
                const QCborMap c = value.toMap();
                QString line = QStringLiteral("  %1 %2").arg(c.value(QStringLiteral("name")).toString(), c.value(QStringLiteral("type")).toString());
                if (!c.value(QStringLiteral("nullable")).toBool())
                    line += QStringLiteral(" NOT NULL");
                if (c.value(QStringLiteral("primaryKey")).toBool())
                    line += QStringLiteral(" PK");
                if (!c.value(QStringLiteral("default")).toString().isEmpty())
                    line += QStringLiteral(" DEFAULT ") + c.value(QStringLiteral("default")).toString();
                if (!c.value(QStringLiteral("remarks")).toString().isEmpty())
                    line += QStringLiteral(" -- ") + c.value(QStringLiteral("remarks")).toString();
                lines << line;
            }
            const QStringList pk = strings(t.value(QStringLiteral("primaryKey")));
            if (!pk.isEmpty())
                lines << QStringLiteral("Primary key: (%1)").arg(pk.join(QStringLiteral(", ")));
            for (const QCborValue &value : t.value(QStringLiteral("foreignKeys")).toArray()) {
                const QCborMap fk = value.toMap();
                lines << QStringLiteral("Foreign key %1: (%2) -> %3.%4 (%5)")
                             .arg(fk.value(QStringLiteral("name")).toString(), strings(fk.value(QStringLiteral("columns"))).join(QStringLiteral(", ")),
                                  fk.value(QStringLiteral("refSchema")).toString(), fk.value(QStringLiteral("refTable")).toString(),
                                  strings(fk.value(QStringLiteral("refColumns"))).join(QStringLiteral(", ")));
            }
            for (const QCborValue &value : t.value(QStringLiteral("referencedBy")).toArray()) {
                const QCborMap fk = value.toMap();
                lines << QStringLiteral("Referenced by %1.%2 (%3) -> (%4)")
                             .arg(fk.value(QStringLiteral("schema")).toString(), fk.value(QStringLiteral("table")).toString(),
                                  strings(fk.value(QStringLiteral("columns"))).join(QStringLiteral(", ")),
                                  strings(fk.value(QStringLiteral("refColumns"))).join(QStringLiteral(", ")));
            }
            for (const QCborValue &value : t.value(QStringLiteral("indexes")).toArray()) {
                const QCborMap index = value.toMap();
                lines << QStringLiteral("Index %1%2: (%3)").arg(index.value(QStringLiteral("name")).toString(),
                                                              index.value(QStringLiteral("unique")).toBool() ? QStringLiteral(" UNIQUE") : QString(),
                                                              strings(index.value(QStringLiteral("columns"))).join(QStringLiteral(", ")));
            }
            done(limited(lines.join(u'\n')), false);
        });
    } else if (name == u"get_ddl") {
        const QString objectName = input.value(QStringLiteral("name")).toString().trimmed();
        if (objectName.isEmpty())
            return done(tr("name is required."), true);
        const QString given = input.value(QStringLiteral("schema")).toString().trimmed();
        QCborMap params;
        if (!given.isEmpty())
            params.insert(QStringLiteral("schema"), given);
        params.insert(QStringLiteral("name"), objectName);
        params.insert(QStringLiteral("schemaQuoted"), needsExactCase(given));
        params.insert(QStringLiteral("nameQuoted"), needsExactCase(objectName));
        if (given.isEmpty() && !schemaOr({}).isEmpty())
            params.insert(QStringLiteral("currentSchema"), schemaOr({}));
        metadataCall(QStringLiteral("ddl"), params, true, [done](const QCborMap &result, const QString &error) {
            if (!error.isEmpty())
                return done(error, true);
            done(limited(result.value(QStringLiteral("script")).toString()), false);
        });
    } else if (name == u"run_query") {
        runQuery(input, std::move(done));
    } else if (name == u"explain_query") {
        explain(input, std::move(done));
    } else if (name == u"propose_sql") {
        const QString sql = input.value(QStringLiteral("sql")).toString().trimmed();
        if (sql.isEmpty())
            return done(tr("sql is required."), true);
        emit sqlProposed(sql, input.value(QStringLiteral("title")).toString().trimmed(), input.value(QStringLiteral("explanation")).toString());
        done(QStringLiteral("Shown to the user with an 'Open in query tab' button. It has not been run; do not assume it was."), false);
    } else {
        done(tr("Unknown tool: %1").arg(name), true);
    }
}

void AiDbTools::withSession(std::function<void()> work, std::function<void(const QString &)> fail)
{
    const QString key = m_context.metadataId ? m_context.metadataId() : QString();
    if (m_session->isConnected() && key == m_sessionKey) {
        work();
        return;
    }
    if (m_session->isConnecting()) {
        m_session->whenConnected([work, fail](bool ok) {
            if (ok)
                work();
            else
                fail(tr("Could not connect."));
        });
        return;
    }
    ConnectSpec spec;
    if (!m_context.connectSpec || !m_context.connectSpec(&spec)) {
        fail(tr("Not connected (no password)."));
        return;
    }
    spec.readOnly = true;   // AI의 조회는 읽기 전용 연결에서
    spec.autoCommit = true;
    m_sessionKey = key;
    QPointer<AiDbTools> self(this);
    m_session->connectTo(spec, m_context.profile ? m_context.profile().id : QString(), [self, work, fail](bool ok) {
        if (!self)
            return;
        if (ok)
            work();
        else
            fail(tr("Could not connect to the database."));
    });
}

void AiDbTools::withSchemaSession(std::function<void()> work, std::function<void(const QString &)> fail)
{
    // 지금 쿼리 탭과 같은 스키마에서 이름을 찾게 한다
    const QString schema = m_context.currentSchema ? m_context.currentSchema() : QString();
    QPointer<AiDbTools> self(this);
    withSession([self, schema, work] {
        if (!self)
            return;
        if (!schema.isEmpty() && self->m_session->currentSchema().compare(schema, Qt::CaseInsensitive) != 0) {
            self->m_session->setSchema(schema, [work](bool) { work(); });
            return;
        }
        work();
    }, std::move(fail));
}

void AiDbTools::runQuery(const QJsonObject &input, AiAgent::ToolDone done)
{
    const ConnectionProfile profile = m_context.profile ? m_context.profile() : ConnectionProfile();
    const SqlDialect dialect = sqlDialectFor(profile.type);
    QString sql = input.value(QStringLiteral("sql")).toString().trimmed();
    while (sql.endsWith(u';'))
        sql.chop(1);
    if (const QString problem = readOnlyProblem(sql, dialect); !problem.isEmpty())
        return done(problem, true);
    Context::QueryPolicy policy;
    bool askBeforeQuery = false;
    if (m_context.policy) {
        policy = m_context.policy();
    } else {
        const AiSettings settings = AiSettings::load();
        policy.maxRows = settings.maxRows;
        policy.sendRows = settings.sendRows;
        askBeforeQuery = settings.askBeforeQuery;
    }
    const int maxRows = qBound(1, input.value(QStringLiteral("max_rows")).toInt(policy.maxRows), policy.maxRows);

    if (m_context.approve) {
        QString reason;
        if (!m_context.approve(sql, &reason))
            return done(reason.isEmpty() ? tr("The user declined to run this query.") : reason, true);
    } else if (askBeforeQuery || profile.environment == Environment::Production) {
        // 운영 DB이거나 설정에서 켰으면 실행 전에 묻는다
        QMessageBox box(QMessageBox::Question, tr("AI Assistant"),
                        tr("The AI assistant wants to run this read-only query on %1%2:")
                            .arg(profile.name, profile.environment == Environment::Production ? tr(" (PRODUCTION)") : QString()),
                        QMessageBox::Yes | QMessageBox::No, m_context.dialogParent);
        box.setInformativeText(sql.left(2000));
        box.button(QMessageBox::Yes)->setText(tr("Run"));
        box.setDefaultButton(QMessageBox::No);
        if (box.exec() != QMessageBox::Yes)
            return done(tr("The user declined to run this query."), true);
    }

    QPointer<AiDbTools> self(this);
    auto execute = [self, sql, maxRows, done, sendRows = policy.sendRows] {
        if (!self)
            return;
        QCborMap params;
        params.insert(QStringLiteral("sql"), sql);
        params.insert(QStringLiteral("pageSize"), maxRows);
        params.insert(QStringLiteral("queryTimeoutSec"), 60);
        self->m_session->call(QStringLiteral("execute"), params, [self, done, maxRows, sendRows](const QCborMap &result, const EngineError &error) {
            if (!self)
                return;
            if (error.isError())
                return done(error.message, true);
            if (const QString cursor = result.value(QStringLiteral("cursorId")).toString(); !cursor.isEmpty()) {
                QCborMap close;
                close.insert(QStringLiteral("cursorId"), cursor);
                self->m_session->call(QStringLiteral("closeCursor"), close, {});
            }
            if (result.value(QStringLiteral("kind")).toString() != u"resultSet")
                return done(QStringLiteral("Done (no result set)."), false);
            QStringList header;
            for (const QCborValue &column : result.value(QStringLiteral("columns")).toArray())
                header << column.toMap().value(QStringLiteral("name")).toString();
            const QCborArray rows = result.value(QStringLiteral("rows")).toArray();
            const bool more = result.value(QStringLiteral("hasMore")).toBool();
            QString text = QStringLiteral("%1 row(s)%2, %3 ms\n").arg(rows.size()).arg(more ? QStringLiteral(" (first %1; more rows exist)").arg(maxRows) : QString())
                               .arg(result.value(QStringLiteral("elapsedMs")).toInteger());
            text += header.join(QStringLiteral(" | ")) + u'\n';
            if (!sendRows) {
                text += QStringLiteral("(Row values are not shared: the user turned off sending query results to the AI.)");
                return done(text, false);
            }
            for (const QCborValue &row : rows) {
                QStringList cells;
                for (const QCborValue &cell : row.toArray())
                    cells << cellText(cell);
                text += cells.join(QStringLiteral(" | ")) + u'\n';
                if (text.size() > kMaxResultChars)
                    break;
            }
            done(limited(text), false);
        });
    };
    withSchemaSession(execute, [done](const QString &error) { done(error, true); });
}

void AiDbTools::explain(const QJsonObject &input, AiAgent::ToolDone done)
{
    QString sql = input.value(QStringLiteral("sql")).toString().trimmed();
    while (sql.endsWith(u';'))
        sql.chop(1);
    if (sql.isEmpty())
        return done(tr("sql is required."), true);
    if (const QString problem = explainProblem(sql, sqlDialectFor(m_context.profile().type)); !problem.isEmpty())
        return done(problem, true);
    QPointer<AiDbTools> self(this);
    withSchemaSession([self, sql, done] {
        if (!self)
            return;
        QCborMap params;
        params.insert(QStringLiteral("sql"), sql);
        params.insert(QStringLiteral("actual"), false);
        self->m_session->call(QStringLiteral("explain"), params, [done](const QCborMap &result, const EngineError &error) {
            if (error.isError())
                return done(error.message, true);
            QString text = result.value(QStringLiteral("text")).toString();
            if (text.isEmpty())
                text = QString::fromUtf8(QJsonDocument::fromVariant(result.toVariantMap()).toJson(QJsonDocument::Indented));
            done(limited(text), false);
        });
    }, [done](const QString &error) { done(error, true); });
}

void AiDbTools::disconnectSession()
{
    m_session->disconnectFromServer();
    m_sessionKey.clear();
}
