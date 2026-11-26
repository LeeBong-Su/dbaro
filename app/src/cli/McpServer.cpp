#include "McpServer.h"

#include "core/ConnectSpec.h"
#include "core/ConnectionStore.h"
#include "core/SecretStore.h"
#include "ipc/EngineClient.h"
#include "ui/AiDbTools.h"
#include "ui/MetadataService.h"
#include "ui/QuerySession.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QThread>
#include <QUuid>

#include <cstdio>
#include <iostream>
#include <string>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {

// 알고 있는 MCP 버전 (최신 먼저). 클라이언트가 이 중 하나를 원하면 그대로, 아니면 맨 앞 것
const QStringList kProtocolVersions{QStringLiteral("2025-11-25"), QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"),
                                    QStringLiteral("2024-11-05")};

void writeError(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stderr);
    std::fflush(stderr);
}

QJsonObject stringProperty(const QString &description)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("description"), description}};
}

QString environmentNote(const ConnectionProfile &profile)
{
    return profile.environment == Environment::None ? QString() : Env::name(profile.environment);
}

} // namespace

McpServer::McpServer(QObject *parent)
    : QObject(parent)
{
    m_writer = [](const QByteArray &line) {
        std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    };
}

McpServer::~McpServer()
{
    qDeleteAll(m_tools);
    m_tools.clear();
}

QString McpServer::usage()
{
    return tr("dbaro-cli mcp [options]\n"
              "  Serve DBaro's database tools to Claude Code or Claude Desktop (MCP over standard input/output).\n"
              "  Uses the connections saved in DBaro and their saved passwords.\n\n"
              "  -c, --connection <name>   Offer only this connection (repeat for more). Default: all saved connections\n"
              "  --allow-production        Allow run_query on connections marked Production\n"
              "  --max-rows <n>            Rows run_query returns at most (default 100, up to 1000)\n"
              "  --no-rows                 run_query returns column names and row counts only, no values\n"
              "  --connections-file <path> Use this connections file instead of DBaro's\n\n"
              "  Claude Code:  claude mcp add --transport stdio --scope user dbaro -- \"<path>\\dbaro-cli.exe\" mcp\n");
}

int McpServer::run(const QStringList &arguments)
{
    QCommandLineParser parser;
    const QCommandLineOption connectionOption({QStringLiteral("c"), QStringLiteral("connection")}, {}, QStringLiteral("name"));
    const QCommandLineOption productionOption(QStringLiteral("allow-production"), {});
    const QCommandLineOption maxRowsOption(QStringLiteral("max-rows"), {}, QStringLiteral("n"));
    const QCommandLineOption noRowsOption(QStringLiteral("no-rows"), {});
    const QCommandLineOption storeOption(QStringLiteral("connections-file"), {}, QStringLiteral("path"));
    const QCommandLineOption helpOption({QStringLiteral("h"), QStringLiteral("help"), QStringLiteral("?")}, {});
    parser.addOptions({connectionOption, productionOption, maxRowsOption, noRowsOption, storeOption, helpOption});
    if (!parser.parse(QStringList{QStringLiteral("dbaro-cli mcp")} + arguments)) {
        writeError(parser.errorText() + QStringLiteral("\n\n") + usage());
        return 2;
    }
    if (parser.isSet(helpOption)) {
        writeError(usage());
        return 0;
    }
    Options options;
    options.connections = parser.values(connectionOption);
    options.allowProduction = parser.isSet(productionOption);
    options.sendRows = !parser.isSet(noRowsOption);
    options.connectionsFile = parser.value(storeOption);
    if (parser.isSet(maxRowsOption)) {
        bool ok = false;
        options.maxRows = parser.value(maxRowsOption).toInt(&ok);
        if (!ok || options.maxRows < 1) {
            writeError(tr("--max-rows needs a number from 1 to 1000\n"));
            return 2;
        }
    }
    QString error;
    if (!setUp(options, &error)) {
        writeError(error + u'\n');
        return 2;
    }

#ifdef Q_OS_WIN
    // 줄 끝을 바꾸지 않는다 (JSON-RPC 메시지는 \n으로 끝난다)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // 표준 입력은 막히는 읽기라 따로 읽고 줄마다 이벤트 루프로 넘긴다
    QPointer<McpServer> self(this);
    QThread *reader = QThread::create([self] {
        std::string line;
        while (std::getline(std::cin, line)) {
            const QByteArray bytes = QByteArray::fromStdString(line);
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, bytes] {
                if (self)
                    self->handleLine(bytes);
            }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
    });
    reader->start();
    writeError(tr("DBaro MCP server: %n connection(s). Waiting for the client on standard input.\n", nullptr, int(m_profiles.size())));
    const int code = QCoreApplication::exec();
    reader->wait();
    delete reader;
    return code;
}

bool McpServer::setUp(const Options &options, QString *error)
{
    m_options = options;
    m_options.maxRows = qBound(1, options.maxRows, 1000);
    m_store = options.connectionsFile.isEmpty() ? std::make_unique<ConnectionStore>()
                                                : std::make_unique<ConnectionStore>(options.connectionsFile);
    QString loadError;
    if (!m_store->load(&loadError)) {
        *error = tr("Could not read the connections (%1): %2").arg(QDir::toNativeSeparators(m_store->filePath()), loadError);
        return false;
    }
    m_profiles.clear();
    if (options.connections.isEmpty()) {
        m_profiles = m_store->profiles();
    } else {
        for (const QString &key : options.connections) {
            const ConnectionProfile *found = nullptr;
            for (const ConnectionProfile &profile : m_store->profiles()) {
                if (profile.id == key || profile.name.compare(key, Qt::CaseInsensitive) == 0) {
                    found = &profile;
                    break;
                }
            }
            if (!found) {
                *error = tr("No saved connection named \"%1\".").arg(key);
                return false;
            }
            m_profiles.append(*found);
        }
    }
    if (m_profiles.isEmpty()) {
        *error = tr("There are no saved connections. Add one in DBaro first.");
        return false;
    }

    m_engine = new EngineClient(this);
    m_metadata = new MetadataService(m_engine, this);
    m_metadata->setProfiles(m_profiles);
    m_metadata->setSpecProvider([this](const ConnectionProfile &profile, ConnectSpec *spec, bool) {
        QString password, ignored;
        if (!passwordFor(profile, &password, &ignored))
            return false;
        *spec = ConnectSpec::fromProfile(profile, password, true);
        return true;
    });
    return true;
}

QString McpServer::instructions() const
{
    return QStringLiteral(
        "DBaro database tools: read the user's databases through the connections saved in DBaro (Oracle, PostgreSQL, "
        "MySQL, MariaDB, Tibero). Everything runs on the user's PC.\n"
        "- Call list_connections first, then pass `connection` to the other tools. Write SQL in that connection's dialect.\n"
        "- Look before you write SQL: list_tables, describe_table and get_ddl give the real names, keys and comments "
        "(comments are often in Korean and explain what columns mean).\n"
        "- run_query runs ONE read-only statement in a read-only session and returns a limited number of rows. "
        "Keep it light on big tables. explain_query shows the estimated plan without running the statement.\n"
        "- Never try to change data or structure with these tools. For INSERT, UPDATE, DELETE, DDL or a finished query "
        "the user should keep, call open_in_dbaro: it opens the SQL in a new DBaro query tab, where the user reviews and runs it.");
}

QJsonArray McpServer::toolDefinitions() const
{
    const bool single = m_profiles.size() == 1;
    const QJsonObject connection =
        stringProperty(single ? QStringLiteral("Connection name (optional: there is only one)")
                              : QStringLiteral("Connection name from list_connections"));
    const QJsonObject schema = stringProperty(QStringLiteral("Schema (database on MySQL/MariaDB) to resolve unqualified names in. "
                                                             "Omit for the connection's default."));
    const QJsonObject readOnly{{QStringLiteral("readOnlyHint"), true}, {QStringLiteral("openWorldHint"), false}};

    QJsonArray tools;
    tools.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("list_connections")},
        {QStringLiteral("description"),
         QStringLiteral("List the database connections saved in DBaro: name, DBMS, database, environment (Production is "
                        "marked) and whether the tools can use it. Call this first.")},
        {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), QJsonObject()}}},
        {QStringLiteral("annotations"), readOnly},
    });
    for (const QJsonValue &value : AiDbTools::definitions()) {
        const QJsonObject definition = value.toObject();
        const QString name = definition.value(QStringLiteral("name")).toString();
        if (name == u"propose_sql")
            continue; // open_in_dbaro가 대신한다
        QJsonObject input = definition.value(QStringLiteral("input_schema")).toObject();
        QJsonObject properties = input.value(QStringLiteral("properties")).toObject();
        properties.insert(QStringLiteral("connection"), connection);
        if (name == u"run_query" || name == u"explain_query")
            properties.insert(QStringLiteral("schema"), schema);
        input.insert(QStringLiteral("properties"), properties);
        if (!single) {
            QJsonArray required = input.value(QStringLiteral("required")).toArray();
            required.prepend(QStringLiteral("connection"));
            input.insert(QStringLiteral("required"), required);
        }
        QString description = definition.value(QStringLiteral("description")).toString();
        description.replace(QStringLiteral("propose_sql"), QStringLiteral("open_in_dbaro"));
        tools.append(QJsonObject{{QStringLiteral("name"), name},
                                 {QStringLiteral("description"), description},
                                 {QStringLiteral("inputSchema"), input},
                                 {QStringLiteral("annotations"), readOnly}});
    }
    QJsonObject openProperties{
        {QStringLiteral("sql"), stringProperty(QStringLiteral("The SQL script"))},
        {QStringLiteral("title"), stringProperty(QStringLiteral("A short title for the query tab"))},
        {QStringLiteral("connection"), connection},
    };
    QJsonArray openRequired{QStringLiteral("sql")};
    if (!single)
        openRequired.append(QStringLiteral("connection"));
    tools.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("open_in_dbaro")},
        {QStringLiteral("description"),
         QStringLiteral("Open SQL in a new query tab of DBaro on a connection, without running it. Use this for every "
                        "statement that changes data or structure (INSERT, UPDATE, DELETE, MERGE, CREATE, ALTER, DROP ...) "
                        "and for a finished query the user should review, run and keep.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), openProperties},
                     {QStringLiteral("required"), openRequired}}},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("readOnlyHint"), false}, {QStringLiteral("destructiveHint"), false},
                     {QStringLiteral("openWorldHint"), false}}},
    });
    return tools;
}

void McpServer::handleLine(const QByteArray &line)
{
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        replyError(QJsonValue::Null, -32700, QStringLiteral("Parse error"));
        return;
    }
    const QJsonObject message = document.object();
    const QString method = message.value(QStringLiteral("method")).toString();
    const bool isRequest = message.contains(QStringLiteral("id"));
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();
    if (method.isEmpty()) {
        if (isRequest && !message.contains(QStringLiteral("result")) && !message.contains(QStringLiteral("error")))
            replyError(id, -32600, QStringLiteral("Invalid request"));
        return; // 클라이언트가 보낸 응답(이 서버는 요청하지 않는다)은 버린다
    }
    if (!isRequest) {
        if (method == u"notifications/cancelled") {
            // 기다리는 호출이면 빼고, 실행 중이면 답을 보내지 않는다 (조회는 취소한다)
            const QJsonValue cancelled = params.value(QStringLiteral("requestId"));
            for (qsizetype i = 0; i < m_queue.size(); ++i) {
                if (m_queue.at(i).id == cancelled) {
                    m_queue.removeAt(i);
                    return;
                }
            }
            for (AiDbTools *tools : std::as_const(m_tools)) {
                if (tools->session())
                    tools->session()->interrupt();
            }
        }
        return; // 알림에는 답하지 않는다
    }

    if (method == u"initialize") {
        const QString requested = params.value(QStringLiteral("protocolVersion")).toString();
        reply(id, QJsonObject{
                      {QStringLiteral("protocolVersion"), kProtocolVersions.contains(requested) ? requested : kProtocolVersions.first()},
                      {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
                      {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("dbaro")},
                                                                 {QStringLiteral("title"), QStringLiteral("DBaro")},
                                                                 {QStringLiteral("version"), QCoreApplication::applicationVersion()}}},
                      {QStringLiteral("instructions"), instructions()},
                  });
    } else if (method == u"ping") {
        reply(id, QJsonObject());
    } else if (method == u"tools/list") {
        reply(id, QJsonObject{{QStringLiteral("tools"), toolDefinitions()}});
    } else if (method == u"tools/call") {
        const QString name = params.value(QStringLiteral("name")).toString();
        bool known = false;
        for (const QJsonValue &tool : toolDefinitions())
            known = known || tool.toObject().value(QStringLiteral("name")).toString() == name;
        if (!known) {
            replyError(id, -32602, QStringLiteral("Unknown tool: %1").arg(name));
            return;
        }
        m_queue.append({id, name, params.value(QStringLiteral("arguments")).toObject()});
        startNext();
    } else {
        replyError(id, -32601, QStringLiteral("Method not found: %1").arg(method));
    }
}

void McpServer::reply(const QJsonValue &id, const QJsonObject &result)
{
    m_writer(QJsonDocument(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id},
                                       {QStringLiteral("result"), result}})
                 .toJson(QJsonDocument::Compact));
}

void McpServer::replyError(const QJsonValue &id, int code, const QString &message)
{
    m_writer(QJsonDocument(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id},
                                       {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}})
                 .toJson(QJsonDocument::Compact));
}

void McpServer::startNext()
{
    if (m_busy || m_queue.isEmpty())
        return;
    m_busy = true;
    const Call call = m_queue.takeFirst();
    QPointer<McpServer> self(this);
    auto answered = std::make_shared<bool>(false);
    runTool(call, [self, call, answered](const QString &text, bool isError) {
        if (!self || *answered)
            return;
        *answered = true;
        QJsonObject result{{QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                              {QStringLiteral("text"), text}}}}};
        if (isError)
            result.insert(QStringLiteral("isError"), true);
        self->reply(call.id, result);
        self->m_busy = false;
        self->m_callSchema.clear();
        QMetaObject::invokeMethod(self, &McpServer::startNext, Qt::QueuedConnection);
    });
}

void McpServer::runTool(const Call &call, Done done)
{
    if (call.name == u"list_connections")
        return listConnections(done);

    const QString key = call.arguments.value(QStringLiteral("connection")).toString().trimmed();
    QString error;
    const ConnectionProfile *profile = nullptr;
    if (key.isEmpty()) {
        if (m_profiles.size() != 1)
            return done(tr("Which connection? Pass `connection` (see list_connections)."), true);
        profile = &m_profiles.first();
    } else if (!(profile = findProfile(key, &error))) {
        return done(error, true);
    }
    if (call.name == u"open_in_dbaro")
        return openInDbaro(*profile, call.arguments, done);

    QString password;
    if (!passwordFor(*profile, &password, &error))
        return done(error, true);
    if (call.name == u"run_query" && profile->environment == Environment::Production && !m_options.allowProduction)
        return done(tr("\"%1\" is a production connection: run_query is turned off for it. Use describe_table, get_ddl or "
                       "explain_query, or give the user the query with open_in_dbaro. (The user can allow it by adding "
                       "--allow-production to the dbaro-cli mcp command.)")
                        .arg(profile->name),
                    true);

    // 실행할 수 없는 SQL은 접속하기 전에 돌려보낸다
    QString sql = call.arguments.value(QStringLiteral("sql")).toString().trimmed();
    while (sql.endsWith(u';'))
        sql.chop(1);
    const SqlDialect dialect = sqlDialectFor(profile->type);
    if (call.name == u"run_query") {
        if (const QString problem = AiDbTools::readOnlyProblem(sql, dialect); !problem.isEmpty())
            return done(QString(problem).replace(QStringLiteral("propose_sql"), QStringLiteral("open_in_dbaro")), true);
    } else if (call.name == u"explain_query") {
        if (const QString problem = AiDbTools::explainProblem(sql, dialect); !problem.isEmpty())
            return done(problem, true);
    }

    m_callSchema = call.arguments.value(QStringLiteral("schema")).toString().trimmed();
    QJsonObject input = call.arguments;
    input.remove(QStringLiteral("connection"));
    const ConnectionProfile chosen = *profile;
    QPointer<McpServer> self(this);
    withDefaultSchema(chosen, [self, chosen, name = call.name, input, done] {
        if (!self)
            return;
        self->toolsFor(chosen)->run(name, input, [done](QString result, bool isError) {
            // GUI 도구의 propose_sql은 여기서는 open_in_dbaro다
            done(result.replace(QStringLiteral("propose_sql"), QStringLiteral("open_in_dbaro")), isError);
        });
    });
}

void McpServer::withDefaultSchema(const ConnectionProfile &profile, std::function<void()> next)
{
    // 스키마를 주지 않은 호출은 접속했을 때의 스키마 (Oracle 사용자, PostgreSQL search_path, MySQL 데이터베이스)
    if (m_defaultSchemas.contains(profile.id)) {
        next();
        return;
    }
    QPointer<McpServer> self(this);
    m_metadata->request(profile.id, QStringLiteral("metaSchemas"), {}, [self, id = profile.id, next](const QCborMap &result, const QString &error) {
        if (!self)
            return;
        if (error.isEmpty())
            self->m_defaultSchemas.insert(id, result.value(QStringLiteral("current")).toString());
        next(); // 접속하지 못했으면 도구가 같은 오류를 돌려준다
    });
}

AiDbTools *McpServer::toolsFor(const ConnectionProfile &profile)
{
    if (AiDbTools *tools = m_tools.value(profile.id))
        return tools;
    const QString id = profile.id;
    AiDbTools::Context context;
    context.metadata = m_metadata;
    context.engine = m_engine;
    context.metadataId = [id] { return id; };
    context.currentSchema = [this, id] { return m_callSchema.isEmpty() ? m_defaultSchemas.value(id) : m_callSchema; };
    context.connectSpec = [this, id](ConnectSpec *spec) {
        QString error, password;
        const ConnectionProfile *p = findProfile(id, &error);
        if (!p || !passwordFor(*p, &password, &error))
            return false;
        *spec = ConnectSpec::fromProfile(*p, password, true);
        return true;
    };
    context.profile = [this, id] {
        QString error;
        const ConnectionProfile *p = findProfile(id, &error);
        return p ? *p : ConnectionProfile();
    };
    context.policy = [this] {
        AiDbTools::Context::QueryPolicy policy;
        policy.maxRows = m_options.maxRows;
        policy.sendRows = m_options.sendRows;
        return policy;
    };
    // 운영 연결은 runTool에서 막았다. 그 밖의 확인은 Claude Code가 도구를 쓸 때마다 사용자에게 묻는다
    context.approve = [](const QString &, QString *) { return true; };
    auto *tools = new AiDbTools(context);
    m_tools.insert(id, tools);
    return tools;
}

void McpServer::listConnections(Done done)
{
    QStringList lines{tr("%n connection(s):", nullptr, int(m_profiles.size()))};
    for (const ConnectionProfile &profile : std::as_const(m_profiles)) {
        QStringList notes;
        if (const QString environment = environmentNote(profile); !environment.isEmpty())
            notes << environment;
        if (profile.environment == Environment::Production && !m_options.allowProduction)
            notes << QStringLiteral("run_query off");
        if (profile.readOnly)
            notes << QStringLiteral("read-only connection");
        QString password, error;
        if (!passwordFor(profile, &password, &error))
            notes << QStringLiteral("unavailable: password not saved in DBaro");
        QString line = QStringLiteral("- %1: %2").arg(profile.name, Db::typeName(profile.type));
        if (!profile.database.isEmpty())
            line += QStringLiteral(", database %1").arg(profile.database);
        if (!notes.isEmpty())
            line += QStringLiteral(" [%1]").arg(notes.join(QStringLiteral("; ")));
        lines << line;
    }
    done(lines.join(u'\n'), false);
}

void McpServer::openInDbaro(const ConnectionProfile &profile, const QJsonObject &arguments, Done done)
{
    const QString sql = arguments.value(QStringLiteral("sql")).toString().trimmed();
    if (sql.isEmpty())
        return done(tr("sql is required."), true);
    QString title = arguments.value(QStringLiteral("title")).toString().simplified().left(60);
    if (title.isEmpty())
        title = QStringLiteral("Claude");
    // 명령줄 길이에 걸리지 않게 SQL은 임시 파일로 넘긴다. DBaro가 읽고 지운다
    const QString path = QDir::temp().filePath(QStringLiteral("dbaro-sql-%1.sql").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(sql.toUtf8() + '\n') < 0) {
        return done(tr("Could not write %1: %2").arg(QDir::toNativeSeparators(path), file.errorString()), true);
    }
    file.close();
    QString error;
    const QStringList launchArguments{QStringLiteral("--connection"), profile.id, QStringLiteral("--open-sql"), path,
                                      QStringLiteral("--title"), title};
    const bool ok = m_launcher ? m_launcher(launchArguments, &error) : launchDbaro(launchArguments, &error);
    if (!ok) {
        QFile::remove(path);
        return done(error, true);
    }
    done(tr("Opened in a new DBaro query tab on \"%1\" (titled \"%2\"). It has not been run: the user reviews and runs it.")
             .arg(profile.name, title),
         false);
}

bool McpServer::launchDbaro(const QStringList &arguments, QString *error) const
{
#ifdef Q_OS_WIN
    const QString program = QCoreApplication::applicationDirPath() + QStringLiteral("/dbaro.exe");
#else
    const QString program = QCoreApplication::applicationDirPath() + QStringLiteral("/dbaro");
#endif
    // DBaro가 떠 있으면 그 프로세스가 받는다 (단일 실행)
    if (!QFileInfo::exists(program)) {
        *error = tr("DBaro was not found next to dbaro-cli (%1).").arg(QDir::toNativeSeparators(program));
        return false;
    }
    if (!QProcess::startDetached(program, arguments)) {
        *error = tr("Could not start %1.").arg(QDir::toNativeSeparators(program));
        return false;
    }
    return true;
}

const ConnectionProfile *McpServer::findProfile(const QString &key, QString *error) const
{
    for (const ConnectionProfile &profile : m_profiles) {
        if (profile.id == key)
            return &profile;
    }
    for (const ConnectionProfile &profile : m_profiles) {
        if (profile.name.compare(key, Qt::CaseInsensitive) == 0)
            return &profile;
    }
    QStringList names;
    for (const ConnectionProfile &profile : m_profiles)
        names << profile.name;
    *error = tr("No connection named \"%1\". Connections: %2").arg(key, names.join(QStringLiteral(", ")));
    return nullptr;
}

bool McpServer::passwordFor(const ConnectionProfile &profile, QString *password, QString *error) const
{
    if (profile.savePassword) {
        if (const std::optional<QString> saved = SecretStore::loadPassword(profile.id)) {
            *password = *saved;
            return true;
        }
    }
    const QByteArray fromEnvironment = qgetenv("DBARO_PASSWORD");
    if (!fromEnvironment.isEmpty()) {
        *password = QString::fromUtf8(fromEnvironment);
        return true;
    }
    *error = tr("The password of \"%1\" is not saved, so these tools cannot connect. In DBaro, edit the connection, "
                "check Save password and connect once.")
                 .arg(profile.name);
    return false;
}
