#include "CliApp.h"

#include "CliConsole.h"
#include "core/ConnectionStore.h"
#include "core/ResultExport.h"
#include "core/ScriptFolders.h"
#include "core/SqlBinds.h"
#include "core/SecretStore.h"

#include "core/SqlSafety.h"
#include "core/SqlSplitter.h"
#include "core/TextFile.h"
#include "core/TextTable.h"
#include "ipc/EngineClient.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>

#include <utility>

namespace {

QCborMap map(std::initializer_list<std::pair<QString, QCborValue>> items)
{
    QCborMap result;
    for (const auto &[key, value] : items)
        result.insert(key, value);
    return result;
}

// 영어 복수형 (번역 파일이 없어도 "1 row", "3 rows")
QString count(qint64 n, const char *one, const char *many)
{
    return QStringLiteral("%1 %2").arg(n).arg(QLatin1String(n == 1 ? one : many));
}

QString elapsedText(qint64 ms)
{
    return ms < 1000 ? QStringLiteral("%1 ms").arg(ms) : QStringLiteral("%1 s").arg(double(ms) / 1000.0, 0, 'f', 2);
}

// 스키마를 바꾸는 문장 (실행한 뒤 프롬프트의 스키마를 다시 읽는다)
bool changesSchema(const QString &sql)
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\s*(?:ALTER\\s+SESSION\\s+SET\\s+CURRENT_SCHEMA\\b|USE\\s|"
                       "SET\\s+(?:SESSION\\s+|LOCAL\\s+)?(?:SEARCH_PATH|SCHEMA)\\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(sql).hasMatch();
}

QString preview(const QString &sql)
{
    const QString text = sql.trimmed();
    return text.size() > 300 ? text.left(300) + QStringLiteral(" ...") : text;
}

} // namespace

// 문장 묶음 하나를 실행하는 동안의 상태
struct CliApp::ScriptRun
{
    QString script;
    QList<SqlStatement> statements;
    int index = 0;
    bool allOk = true;
    bool schemaMayChange = false;
    bool vertical = false; // 마지막 문장을 세로로 (\G)
    int firstLine = 0;     // script 첫 줄의 파일 줄 번호 (파일이 아니면 0)
    std::shared_ptr<CliApp::ScriptFrame> frame;
    std::function<void(bool)> done;
};

// 스크립트 파일 하나를 실행하는 동안의 상태 (@파일, @@파일, START, -f, 파이프)
struct CliApp::ScriptFrame
{
    QString path;      // 보여 줄 이름
    QString dir;       // @@가 찾는 폴더
    QStringList lines;
    int index = 0;     // 다음에 처리할 줄
    QString buffer;    // 아직 끝나지 않은 문장
    int bufferLine = 0;
    QStringList args;  // &1, &2 ...
    int statements = 0;
    int failed = 0;
    bool aborted = false;
    QElapsedTimer timer;
    std::function<void(bool)> done;
};

// 문장 하나를 실행하는 동안의 상태 (결과를 페이지로 모은다)
struct CliApp::StatementRun
{
    SqlStatement statement;
    QElapsedTimer timer;
    QCborArray columns;
    QList<QCborArray> rows;
    QString cursorId;
    QStringList serverOutput;
    bool hasMore = false;
    bool ok = true;
    bool vertical = false;
    std::function<void(bool)> done;
};

CliApp::CliApp(QObject *parent)
    : QObject(parent)
{
    m_out = [](const QString &text) { CliConsole::writeOut(text); };
    m_err = [](const QString &text) { CliConsole::writeErr(text); };
    m_readLine = [](QString *line) { return CliConsole::readLine(line); };
    m_readHidden = [](QString *line) { return CliConsole::readHiddenLine(line); };
    m_interactiveInput = CliConsole::stdinIsConsole();
}

CliApp::~CliApp()
{
    disconnectSession();
}

void CliApp::setOutput(Writer out, Writer err)
{
    m_out = std::move(out);
    m_err = std::move(err);
}

void CliApp::setInput(LineReader readLine, LineReader readHidden, bool interactive)
{
    m_readLine = std::move(readLine);
    m_readHidden = std::move(readHidden);
    m_interactiveInput = interactive;
}

void CliApp::setSpecOverride(std::function<void(const ConnectionProfile &, ConnectSpec *)> override)
{
    m_specOverride = std::move(override);
}

QString CliApp::usage()
{
    return QStringLiteral(
        "Usage: dbaro-cli [options] [connection] [sql]\n"
        "\n"
        "Runs SQL on a connection saved in DBaro. Without SQL it starts an interactive prompt\n"
        "(end statements with ';', PL/SQL blocks with a '/' line). SQL can also come from stdin.\n"
        "\n"
        "  dbaro-cli mcp [options]    Serve DBaro's database tools to Claude Code or Claude Desktop\n"
        "                             (MCP server, see dbaro-cli mcp --help)\n"
        "\n"
        "Options:\n"
        "  -l, --list                 List saved connections\n"
        "  -c, --connection <name>    Connection name or id (or a unique part of the name)\n"
        "  -e, --execute <sql>        Run SQL (several statements allowed) and exit\n"
        "  -f, --file <path>          Run a SQL script file and exit\n"
        "      --format <format>      table (default), csv, tsv or json\n"
        "  -o, --output <path>        Write results to a file (UTF-8)\n"
        "      --bom                  Start the output file with a UTF-8 BOM (for Excel)\n"
        "      --max-rows <n>         Rows to fetch per result (table: 1000, others: all; 0 = all)\n"
        "      --max-width <n>        Widest table cell in columns (default 60; 0 = never shorten)\n"
        "      --schema <name>        Switch to this schema after connecting\n"
        "      --continue-on-error    Keep running -e statements after a failed one\n"
        "      --stop-on-error        Stop a script file at the first error (default: go on, like SQL*Plus)\n"
        "      --no-autocommit        Do not commit automatically (interactive: \\commit, \\rollback)\n"
        "      --no-history           Do not add statements to DBaro's history (F8)\n"
        "  -y, --yes                  Do not ask before UPDATE/DELETE without WHERE or DROP on production\n"
        "  -q, --quiet                Only print results and errors\n"
        "      --gui                  Open the connection's window in DBaro and exit\n"
        "      --connections-file <path>  Use another connections.json\n"
        "  -v, --version              Show the version\n"
        "  -h, --help                 Show this help\n"
        "\n"
        "Passwords come from DBaro's saved password, then the DBARO_PASSWORD environment variable,\n"
        "then a prompt. Exit code: 0 success, 1 a statement failed, 2 usage or connection error.\n");
}

QString CliApp::formatName(Format format)
{
    switch (format) {
    case Format::Table: return QStringLiteral("table");
    case Format::Csv: return QStringLiteral("csv");
    case Format::Tsv: return QStringLiteral("tsv");
    case Format::Json: return QStringLiteral("json");
    }
    return {};
}

bool CliApp::parseFormat(const QString &text, Format *format)
{
    const QString key = text.trimmed().toLower();
    for (const Format candidate : {Format::Table, Format::Csv, Format::Tsv, Format::Json}) {
        if (key == formatName(candidate)) {
            *format = candidate;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 명령줄 프로그램

int CliApp::run(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsOptions);
    const QCommandLineOption listOption({QStringLiteral("l"), QStringLiteral("list")}, {});
    const QCommandLineOption connectionOption({QStringLiteral("c"), QStringLiteral("connection")}, {}, QStringLiteral("name"));
    const QCommandLineOption executeOption({QStringLiteral("e"), QStringLiteral("execute")}, {}, QStringLiteral("sql"));
    const QCommandLineOption fileOption({QStringLiteral("f"), QStringLiteral("file")}, {}, QStringLiteral("path"));
    const QCommandLineOption formatOption(QStringLiteral("format"), {}, QStringLiteral("format"));
    const QCommandLineOption outputOption({QStringLiteral("o"), QStringLiteral("output")}, {}, QStringLiteral("path"));
    const QCommandLineOption bomOption(QStringLiteral("bom"), {});
    const QCommandLineOption maxRowsOption(QStringLiteral("max-rows"), {}, QStringLiteral("n"));
    const QCommandLineOption maxWidthOption(QStringLiteral("max-width"), {}, QStringLiteral("n"));
    const QCommandLineOption schemaOption(QStringLiteral("schema"), {}, QStringLiteral("name"));
    const QCommandLineOption continueOption(QStringLiteral("continue-on-error"), {});
    const QCommandLineOption stopOption(QStringLiteral("stop-on-error"), {});
    const QCommandLineOption noAutoCommitOption(QStringLiteral("no-autocommit"), {});
    const QCommandLineOption noHistoryOption(QStringLiteral("no-history"), {});
    const QCommandLineOption yesOption({QStringLiteral("y"), QStringLiteral("yes")}, {});
    const QCommandLineOption quietOption({QStringLiteral("q"), QStringLiteral("quiet")}, {});
    const QCommandLineOption guiOption(QStringLiteral("gui"), {});
    const QCommandLineOption storeOption(QStringLiteral("connections-file"), {}, QStringLiteral("path"));
    const QCommandLineOption versionOption({QStringLiteral("v"), QStringLiteral("version")}, {});
    const QCommandLineOption helpOption({QStringLiteral("h"), QStringLiteral("help"), QStringLiteral("?")}, {});
    parser.addOptions({listOption, connectionOption, executeOption, fileOption, formatOption, outputOption, bomOption,
                       maxRowsOption, maxWidthOption, schemaOption, continueOption, stopOption, noAutoCommitOption, noHistoryOption, yesOption,
                       quietOption, guiOption, storeOption, versionOption, helpOption});

    if (!parser.parse(QStringList{QStringLiteral("dbaro-cli")} + arguments)) {
        err(parser.errorText() + QStringLiteral("\n\n") + usage());
        return 2;
    }
    if (parser.isSet(helpOption)) {
        m_out(usage());
        return 0;
    }
    if (parser.isSet(versionOption)) {
        m_out(QStringLiteral("dbaro-cli %1\n").arg(QCoreApplication::applicationVersion()));
        return 0;
    }
    if (parser.isSet(formatOption) && !parseFormat(parser.value(formatOption), &m_format)) {
        err(tr("Unknown format: %1 (use table, csv, tsv or json)\n").arg(parser.value(formatOption)));
        return 2;
    }
    if (parser.isSet(maxRowsOption)) {
        bool ok = false;
        m_maxRows = parser.value(maxRowsOption).toInt(&ok);
        if (!ok || m_maxRows < 0) {
            err(tr("--max-rows needs a number (0 = all rows)\n"));
            return 2;
        }
    } else if (m_format != Format::Table) {
        m_maxRows = 0; // 파일로 뽑을 때는 모두
    }
    if (parser.isSet(maxWidthOption)) {
        bool ok = false;
        m_maxCellWidth = parser.value(maxWidthOption).toInt(&ok);
        if (!ok || m_maxCellWidth < 0) {
            err(tr("--max-width needs a number (0 = never shorten)\n"));
            return 2;
        }
    }
    m_stopOnError = !parser.isSet(continueOption);
    m_scriptStopOnError = parser.isSet(stopOption);
    m_autoCommit = !parser.isSet(noAutoCommitOption);
    m_recordHistory = !parser.isSet(noHistoryOption);
    m_quiet = parser.isSet(quietOption);
    m_bom = parser.isSet(bomOption);
    m_schemaOption = parser.value(schemaOption);

    m_store = parser.isSet(storeOption) ? std::make_unique<ConnectionStore>(parser.value(storeOption))
                                        : std::make_unique<ConnectionStore>();
    QString loadError;
    if (!m_store->load(&loadError)) {
        err(tr("Could not read the connections (%1): %2\n").arg(QDir::toNativeSeparators(m_store->filePath()), loadError));
        return 2;
    }

    if (parser.isSet(listOption)) {
        printConnections();
        return 0;
    }

    QStringList positional = parser.positionalArguments();
    QString connectionKey = parser.value(connectionOption);
    if (connectionKey.isEmpty() && !positional.isEmpty())
        connectionKey = positional.takeFirst();
    if (connectionKey.isEmpty()) {
        err(tr("Choose a connection with -c <name>. Saved connections:\n"));
        printConnections();
        return 2;
    }
    QString findError;
    const ConnectionProfile *profile = findConnection(connectionKey, &findError);
    if (!profile) {
        err(findError + u'\n');
        return 2;
    }
    m_profile = *profile;
    m_dialect = sqlDialectFor(m_profile.type);

    if (parser.isSet(guiOption)) {
        openGui(m_profile);
        return 0;
    }

    // 실행할 SQL: -e, 두 번째 인자, -f, 파이프로 넘긴 표준 입력. 없으면 대화형.
    QString script;
    QString scriptPath; // -f 또는 표준 입력이면 파일처럼 한 줄씩 (SQL*Plus 명령, @@, 줄 번호)
    bool haveScript = false;
    if (parser.isSet(executeOption)) {
        script = parser.values(executeOption).join(QStringLiteral(";\n"));
        haveScript = true;
    } else if (!positional.isEmpty()) {
        script = positional.join(u' ');
        haveScript = true;
    } else if (parser.isSet(fileOption)) {
        QFile file(parser.value(fileOption));
        if (!file.open(QIODevice::ReadOnly)) {
            err(tr("Could not open %1: %2\n").arg(QDir::toNativeSeparators(file.fileName()), file.errorString()));
            return 2;
        }
        file.close();
        TextFileFormat format;
        QString readError;
        if (!TextFile::read(parser.value(fileOption), &script, &format, &readError)) {
            err(tr("Could not read %1: %2\n").arg(QDir::toNativeSeparators(parser.value(fileOption)), readError));
            return 2;
        }
        scriptPath = QFileInfo(parser.value(fileOption)).absoluteFilePath();
        haveScript = true;
    } else if (!m_interactiveInput) {
        script = CliConsole::readAll();
        scriptPath = QStringLiteral("stdin");
        haveScript = true;
    }

    if (parser.isSet(outputOption)) {
        m_outputFile.setFileName(parser.value(outputOption));
        if (!m_outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            err(tr("Could not write %1: %2\n").arg(QDir::toNativeSeparators(m_outputFile.fileName()),
                                                   m_outputFile.errorString()));
            return 2;
        }
        if (m_bom)
            m_outputFile.write("\xEF\xBB\xBF");
    }

    // 콘솔에서 직접 쓰면 위험한 문장 전에 묻는다 (-y로 끈다)
    if (m_interactiveInput && !parser.isSet(yesOption)) {
        m_confirm = [this](const QString &question) {
            m_err(question + tr(" [y/N] "));
            QString answer;
            return m_readLine(&answer) && answer.trimmed().startsWith(u'y', Qt::CaseInsensitive);
        };
    }

    if (m_interactiveInput) {
        m_askValue = [this](const QString &prompt, bool hide, QString *value) {
            m_err(prompt);
            return hide ? m_readHidden(value) : m_readLine(value);
        };
    }

    if (m_recordHistory)
        m_history = std::make_unique<QueryHistory>();
    if (!m_engine) {
        m_engine = new EngineClient(this);
        m_ownsEngine = true;
    }

    // Ctrl+C: 실행 중이면 그 문장을 취소하고, 입력 중이면 쓰던 문장을 버린다 (콘솔 스레드에서 불린다)
    CliConsole::setInterruptHandler([this] {
        if (m_running)
            QMetaObject::invokeMethod(this, &CliApp::cancelRunning, Qt::QueuedConnection);
        else
            m_clearInput = true;
    });

    QString password;
    if (!passwordFor(m_profile, &password))
        return 2;
    ConnectSpec spec = ConnectSpec::fromProfile(m_profile, password, m_autoCommit);
    if (m_specOverride)
        m_specOverride(m_profile, &spec);
    spec.schema = m_schemaOption; // 비어 있으면 접속 설정의 기본 스키마
    if (!m_quiet && m_interactiveInput)
        m_err(tr("Connecting to %1 ...\n").arg(m_profile.name));
    bool connected = false;
    await([&](std::function<void()> finish) {
        connectAsync(spec, [&connected, finish](bool ok) {
            connected = ok;
            finish();
        });
    });
    if (!connected)
        return 2;

    int code = 0;
    if (haveScript) {
        bool ok = false;
        m_interrupted = false;
        await([&](std::function<void()> finish) {
            const auto done = [&ok, finish](bool result) {
                ok = result;
                finish();
            };
            if (scriptPath.isEmpty())
                runScriptAsync(script, done);
            else
                runFileText(script, scriptPath, {}, done);
        });
        code = ok ? 0 : 1;
    } else {
        code = repl();
    }
    disconnectSession();
    CliConsole::setInterruptHandler({});
    if (m_outputFile.isOpen())
        m_outputFile.close();
    return code;
}

void CliApp::await(const std::function<void(std::function<void()>)> &start)
{
    bool finished = false;
    QEventLoop loop;
    start([&finished, &loop] {
        finished = true;
        loop.quit();
    });
    if (!finished)
        loop.exec();
}

const ConnectionProfile *CliApp::findConnection(const QString &key, QString *error) const
{
    const QList<ConnectionProfile> &profiles = m_store->profiles();
    for (const ConnectionProfile &profile : profiles) {
        if (profile.id == key)
            return &profile;
    }
    for (const ConnectionProfile &profile : profiles) {
        if (profile.name.compare(key, Qt::CaseInsensitive) == 0)
            return &profile;
    }
    // 이름의 앞부분, 그다음 이름 일부로 하나만 맞으면 그 연결
    for (const bool prefix : {true, false}) {
        QList<const ConnectionProfile *> matches;
        for (const ConnectionProfile &profile : profiles) {
            if (prefix ? profile.name.startsWith(key, Qt::CaseInsensitive) : profile.name.contains(key, Qt::CaseInsensitive))
                matches << &profile;
        }
        if (matches.size() == 1)
            return matches.first();
        if (matches.size() > 1) {
            QStringList names;
            for (const ConnectionProfile *match : std::as_const(matches))
                names << match->name;
            *error = tr("\"%1\" matches several connections: %2").arg(key, names.join(QStringLiteral(", ")));
            return nullptr;
        }
    }
    *error = tr("No saved connection named \"%1\". Use -l to list them.").arg(key);
    return nullptr;
}

bool CliApp::passwordFor(const ConnectionProfile &profile, QString *password)
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
    if (!m_interactiveInput) { // 표준 입력이 스크립트라 물을 수 없다
        err(tr("No saved password for \"%1\". Save it in DBaro or set DBARO_PASSWORD.\n").arg(profile.name));
        return false;
    }
    m_err(tr("Password for %1 on \"%2\": ").arg(profile.user, profile.name));
    return m_readHidden(password);
}

// ---------------------------------------------------------------------------
// GUI 안의 CLI 탭

void CliApp::startEmbedded(EngineClient *engine, const ConnectionProfile &profile, const ConnectSpec &spec,
                           std::function<void(bool)> done)
{
    m_embedded = true;
    m_engine = engine;
    m_ownsEngine = false;
    m_profile = profile;
    m_dialect = sqlDialectFor(profile.type);
    m_autoCommit = spec.autoCommit;
    m_interactiveInput = true;
    // 창 안에서는 화면을 넘어도 가로 스크롤로 보면 되니 자르지 않고 모두 보여 준다 (\max, \width로 바꿀 수 있다)
    m_maxRows = 0;
    m_maxCellWidth = 0;
    m_out(tr("Connecting to %1 ...\n").arg(profile.name));
    connectAsync(spec, [this, done](bool ok) {
        if (ok)
            printBanner();
        emit stateChanged();
        done(ok);
    });
}

void CliApp::printBanner()
{
    m_out(tr("Connected to %1 (%2), auto-commit %3.\n"
             "End statements with ';' (PL/SQL blocks with a '/' line). \\help lists the commands.\n")
              .arg(m_profile.name, m_serverInfo, m_autoCommit ? tr("on") : tr("off")));
}

// ---------------------------------------------------------------------------
// 엔진

void CliApp::request(const QString &method, const QCborMap &params, Reply reply)
{
    if (!m_engine) {
        reply({}, EngineError::of(tr("The engine is not available.")));
        return;
    }
    // 탭을 닫아 이 객체가 없어진 뒤에 온 응답은 버린다
    QPointer<CliApp> self(this);
    m_engine->request(method, params, [self, reply](const QCborMap &result, const EngineError &error) {
        if (self)
            reply(result, error);
    });
}

void CliApp::connectAsync(const ConnectSpec &spec, std::function<void(bool)> done)
{
    const QString requestedSchema = spec.schema;
    request(QStringLiteral("connect"), spec.toCbor(), [this, done, requestedSchema](const QCborMap &result, const EngineError &error) {
        if (error.isError()) {
            err(tr("Connection failed: %1\n").arg(error.message));
            done(false);
            return;
        }
        m_sessionId = result.value(QStringLiteral("sessionId")).toString();
        m_serverInfo = QStringLiteral("%1 %2").arg(result.value(QStringLiteral("product")).toString(),
                                                   result.value(QStringLiteral("productVersion")).toString().section(u'\n', 0, 0));
        m_currentSchema = result.value(QStringLiteral("schema")).toString();
        const QString schemaError = result.value(QStringLiteral("schemaError")).toString();
        if (!schemaError.isEmpty())
            err(tr("Could not switch to schema %1: %2\n").arg(requestedSchema, schemaError));
        done(true);
    });
}

void CliApp::disconnectSession()
{
    if (m_sessionId.isEmpty() || !m_engine)
        return;
    const QCborMap params = map({{QStringLiteral("sessionId"), m_sessionId}});
    m_sessionId.clear();
    m_pendingTransaction = false;
    if (!m_ownsEngine) {
        // GUI의 엔진: 세션만 닫는다 (커밋하지 않은 변경은 엔진이 롤백한다)
        m_engine->request(QStringLiteral("disconnect"), params);
        return;
    }
    await([&](std::function<void()> finish) {
        m_engine->request(QStringLiteral("disconnect"), params, [finish](const QCborMap &, const EngineError &) { finish(); });
    });
    m_engine->stop();
}

void CliApp::cancelRunning()
{
    if (!m_running || m_sessionId.isEmpty())
        return;
    m_interrupted = true;
    m_engine->request(QStringLiteral("cancel"), map({{QStringLiteral("sessionId"), m_sessionId}}));
    err(tr("Cancelling...\n"));
}

void CliApp::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged(busy);
}

// ---------------------------------------------------------------------------
// 실행

bool CliApp::confirmSafe(const QList<SqlStatement> &statements)
{
    // 읽기 전용 연결에서는 바꾸는 문장을 막는다 (GUI 쿼리 탭과 같다)
    for (const SqlStatement &statement : statements) {
        if (m_profile.readOnly && SqlSafety::mayModify(statement.text)) {
            err(tr("Blocked on a read-only connection: %1\n").arg(preview(statement.text)));
            return false;
        }
    }
    if (!m_confirm)
        return true;
    for (const SqlStatement &statement : statements) {
        QString question;
        if (SqlSafety::isUnfilteredUpdateOrDelete(statement.text))
            question = tr("This statement changes every row in the table:\n%1\nRun it anyway?").arg(preview(statement.text));
        else if (m_profile.environment == Environment::Production && SqlSafety::isDestructiveDdl(statement.text))
            question = tr("You are about to run this on the PRODUCTION connection \"%1\":\n%2\nRun it anyway?")
                           .arg(m_profile.name, preview(statement.text));
        if (!question.isEmpty() && !m_confirm(question)) {
            err(tr("Cancelled.\n"));
            return false;
        }
    }
    return true;
}

void CliApp::runScriptAsync(const QString &script, std::function<void(bool)> done, const FramePtr &frame, int firstLine,
                            bool vertical)
{
    auto run = std::make_shared<ScriptRun>();
    run->script = script;
    run->frame = frame;
    run->firstLine = firstLine;
    run->vertical = vertical;
    // MySQL DELIMITER를 앞에 붙였으면 그 줄만큼 줄 번호를 당긴다
    if (firstLine > 0 && m_dialect == SqlDialect::MySql && m_delimiter != u";" && script.startsWith(QStringLiteral("DELIMITER ")))
        run->firstLine = firstLine - 1;
    for (SqlStatement statement : SqlSplitter::split(script, m_dialect)) {
        if (statement.text.trimmed().isEmpty())
            continue;
        // 문장 끝의 \G: 세로로 (MySQL처럼). -e "select 1\G"
        QString text = statement.text.trimmed();
        if (text.endsWith(QStringLiteral("\\G")) || text.endsWith(QStringLiteral("\\g"))) {
            if (text.endsWith(QStringLiteral("\\G")))
                run->vertical = true;
            text.chop(2);
            statement.text = text;
        }
        run->statements << statement;
    }
    run->done = std::move(done);
    if (!confirmSafe(run->statements)) {
        run->done(false);
        return;
    }
    runNext(run);
}

void CliApp::runNext(const std::shared_ptr<ScriptRun> &script)
{
    if (m_interrupted && script->index < script->statements.size()) {
        script->allOk = false;
        script->index = int(script->statements.size());
    }
    if (script->index >= script->statements.size()) {
        if (!script->schemaMayChange) {
            script->done(script->allOk);
            return;
        }
        refreshSchemaAsync([script] { script->done(script->allOk); });
        return;
    }
    SqlStatement statement = script->statements[script->index++];
    const bool last = script->index == script->statements.size();
    script->schemaMayChange = script->schemaMayChange || changesSchema(statement.text);
    // 파일에서 온 문장이면 오류 글에 파일:줄을 붙인다
    m_location.clear();
    if (script->frame && script->firstLine > 0) {
        const QString leading = script->script.left(statement.start);
        int offset = int(leading.count(u'\n'));
        // 문장 앞의 빈 줄과 -- 주석 줄은 건너뛰고, SQL이 시작하는 줄
        const QStringList bodyLines = script->script.mid(statement.start, statement.end - statement.start).split(u'\n');
        for (const QString &bodyLine : bodyLines) {
            const QString trimmedLine = bodyLine.trimmed();
            if (!trimmedLine.isEmpty() && !trimmedLine.startsWith(QStringLiteral("--")))
                break;
            ++offset;
        }
        m_location = QStringLiteral("%1:%2").arg(QFileInfo(script->frame->path).fileName()).arg(script->firstLine + offset);
    }
    const bool stopOnError = script->frame ? m_scriptStopOnError : m_stopOnError;
    QCborArray binds;
    QList<int> outVars;
    if (!substitute(&statement, script->frame) || !prepareBinds(&statement, &binds, &outVars)) {
        script->allOk = false;
        if (script->frame)
            ++script->frame->failed;
        if (stopOnError)
            script->index = int(script->statements.size());
        runNext(script);
        return;
    }
    if (m_echo && script->frame) {
        QString echoed = statement.text.trimmed();
        echoed.replace(u'\n', QStringLiteral("\n     "));
        out(QStringLiteral("SQL> %1\n").arg(echoed));
    }
    if (script->frame)
        ++script->frame->statements;
    runStatementAsync(statement, script->vertical && last, binds, outVars, [this, script, stopOnError](bool ok) {
        if (!ok) {
            script->allOk = false;
            if (script->frame)
                ++script->frame->failed;
            if (stopOnError)
                script->index = int(script->statements.size());
        }
        runNext(script);
    });
}

void CliApp::runStatementAsync(const SqlStatement &statement, bool vertical, const QCborArray &binds,
                               const QList<int> &outVars, std::function<void(bool)> done)
{
    auto run = std::make_shared<StatementRun>();
    run->statement = statement;
    run->vertical = vertical;
    run->done = std::move(done);
    run->timer.start();
    m_running = true;
    const int pageSize = m_maxRows > 0 ? qMin(m_maxRows, 500) : 500;
    QCborMap params = map({{QStringLiteral("sessionId"), m_sessionId},
                           {QStringLiteral("sql"), statement.text},
                           {QStringLiteral("pageSize"), pageSize},
                           {QStringLiteral("queryTimeoutSec"), m_profile.queryTimeoutSec}});
    if (!binds.isEmpty())
        params.insert(QStringLiteral("binds"), binds);
    request(QStringLiteral("execute"), params, [this, run, outVars](const QCborMap &result, const EngineError &error) {
                if (error.isError()) {
                    m_running = false;
                    QString where = error.line > 0 ? tr(" (line %1)").arg(error.line) : QString();
                    if (!m_location.isEmpty())
                        where = tr(" at %1").arg(m_location);
                    err(tr("ERROR%1: %2\n").arg(where, error.message));
                    recordHistory(run->statement, false, run->timer.elapsed(), -1, error.message);
                    run->done(false);
                    return;
                }
                // PL/SQL 블록이 바꾼 바인드 변수 값 (EXEC :v := ...)
                const QCborArray outs = result.value(QStringLiteral("outBinds")).toArray();
                for (qsizetype i = 0; i < outs.size() && i < outVars.size(); ++i) {
                    if (outVars[i] >= 0 && outVars[i] < m_bindVars.size())
                        m_bindVars[outVars[i]].value = outs.at(i);
                }
                for (const QCborValue &warning : result.value(QStringLiteral("warnings")).toArray())
                    err(tr("Warning: %1\n").arg(warning.toString()));
                for (const QCborValue &line : result.value(QStringLiteral("output")).toArray())
                    run->serverOutput << line.toString();

                if (result.value(QStringLiteral("kind")).toString() == u"resultSet") {
                    run->columns = result.value(QStringLiteral("columns")).toArray();
                    for (const QCborValue &row : result.value(QStringLiteral("rows")).toArray())
                        run->rows << row.toArray();
                    run->hasMore = result.value(QStringLiteral("hasMore")).toBool();
                    run->cursorId = result.value(QStringLiteral("cursorId")).toString();
                    fetchRest(run);
                    return;
                }

                m_running = false;
                const qint64 updateCount = result.value(QStringLiteral("updateCount")).toInteger(-1);
                if (m_feedback)
                    info(updateCount >= 0
                             ? tr("%1 affected (%2)\n").arg(count(updateCount, "row", "rows"), elapsedText(run->timer.elapsed()))
                             : tr("Done (%1)\n").arg(elapsedText(run->timer.elapsed())));
                for (const QString &line : std::as_const(run->serverOutput))
                    info(line + u'\n');
                // 수동 커밋: 바꾼 문장이 있으면 커밋할 때까지 표시 (COMMIT·ROLLBACK을 직접 실행하면 지운다)
                const QString keyword = SqlSafety::leadingKeyword(run->statement.text);
                if (keyword == u"COMMIT" || keyword == u"ROLLBACK")
                    m_pendingTransaction = false;
                else if (!m_autoCommit && SqlSafety::mayModify(run->statement.text))
                    m_pendingTransaction = true;
                emit stateChanged();
                recordHistory(run->statement, true, run->timer.elapsed(), updateCount, {});
                run->done(true);
            });
}

void CliApp::fetchRest(const std::shared_ptr<StatementRun> &run)
{
    if (!run->hasMore || m_interrupted || (m_maxRows > 0 && run->rows.size() >= m_maxRows)) {
        finishResult(run);
        return;
    }
    const int pageCount = m_maxRows == 0 ? 5000 : int(qMin<qsizetype>(5000, m_maxRows - run->rows.size()));
    request(QStringLiteral("fetch"),
            map({{QStringLiteral("sessionId"), m_sessionId},
                 {QStringLiteral("cursorId"), run->cursorId},
                 {QStringLiteral("count"), pageCount}}),
            [this, run](const QCborMap &page, const EngineError &error) {
                if (error.isError()) {
                    err(tr("ERROR while fetching rows: %1\n").arg(error.message));
                    run->ok = false;
                    finishResult(run);
                    return;
                }
                for (const QCborValue &row : page.value(QStringLiteral("rows")).toArray())
                    run->rows << row.toArray();
                run->hasMore = page.value(QStringLiteral("hasMore")).toBool();
                fetchRest(run);
            });
}

void CliApp::finishResult(const std::shared_ptr<StatementRun> &run)
{
    if (m_maxRows > 0 && run->rows.size() > m_maxRows) {
        run->rows.resize(m_maxRows);
        run->hasMore = true;
    }
    if (run->hasMore && !run->cursorId.isEmpty())
        request(QStringLiteral("closeCursor"),
                map({{QStringLiteral("sessionId"), m_sessionId}, {QStringLiteral("cursorId"), run->cursorId}}),
                [](const QCborMap &, const EngineError &) {});
    m_running = false;

    printResult(run->columns, run->rows, run->vertical);
    QString footer = count(run->rows.size(), "row", "rows");
    if (run->hasMore)
        footer += m_interrupted ? tr(" (cancelled)") : tr(" (more rows not fetched; --max-rows 0 for all)");
    if (m_feedback || run->hasMore)
        info(QStringLiteral("%1 (%2)\n").arg(footer, elapsedText(run->timer.elapsed())));
    for (const QString &line : std::as_const(run->serverOutput))
        info(line + u'\n');
    recordHistory(run->statement, run->ok, run->timer.elapsed(), run->rows.size(), {});
    run->done(run->ok);
}

void CliApp::printRecords(const QCborArray &columns, const QList<QCborArray> &rows)
{
    // MySQL의 \G 모양: 행마다 별 줄, 컬럼 이름은 오른쪽 맞춤
    QStringList names;
    int width = 0;
    for (const QCborValue &value : columns) {
        names << value.toMap().value(QStringLiteral("name")).toString();
        width = qMax(width, TextTable::displayWidth(names.last()));
    }
    QString text;
    for (qsizetype r = 0; r < rows.size(); ++r) {
        text += QString(27, u'*') + QStringLiteral(" %1. row ").arg(r + 1) + QString(27, u'*') + u'\n';
        for (qsizetype c = 0; c < names.size(); ++c) {
            const QCborValue value = rows[r].at(c);
            const QString shown = value.isNull() || value.isUndefined() ? QStringLiteral("NULL") : ResultExport::valueText(value);
            text += QString(qMax(0, width - TextTable::displayWidth(names[c])), u' ') + names[c] + QStringLiteral(": ") + shown
                    + u'\n';
        }
    }
    out(text);
    ++m_resultCount;
}

void CliApp::printResult(const QCborArray &columns, const QList<QCborArray> &rows, bool vertical)
{
    if (vertical) {
        printRecords(columns, rows);
        return;
    }
    QList<ExportColumn> exportColumns;
    for (const QCborValue &value : columns) {
        const QCborMap column = value.toMap();
        exportColumns.append({column.value(QStringLiteral("name")).toString(), column.value(QStringLiteral("typeName")).toString(),
                              column.value(QStringLiteral("numeric")).toBool()});
    }

    if (m_format == Format::Table) {
        QStringList headers;
        QList<bool> right;
        for (const ExportColumn &column : std::as_const(exportColumns)) {
            headers << column.name;
            right << column.numeric;
        }
        QList<QStringList> cells;
        cells.reserve(rows.size());
        for (const QCborArray &row : rows) {
            QStringList line;
            for (qsizetype c = 0; c < exportColumns.size(); ++c) {
                const QCborValue value = row.at(c);
                line << (value.isNull() || value.isUndefined() ? QStringLiteral("NULL") : ResultExport::valueText(value));
            }
            cells << line;
        }
        out(TextTable::render(headers, cells, right, m_maxCellWidth, m_heading && !m_pageSizeZero));
        ++m_resultCount;
        return;
    }

    ExportOptions options;
    options.format = m_format == Format::Csv   ? ExportOptions::Format::Csv
                     : m_format == Format::Tsv ? ExportOptions::Format::Tsv
                                               : ExportOptions::Format::Json;
    options.bom = false;
    options.header = m_heading && !m_pageSizeZero;
    options.dialect = m_dialect;
    QString text = QString::fromUtf8(ResultExport::build(exportColumns, rows, options));
    if (m_format == Format::Json)
        text = QString::fromUtf8(QJsonDocument::fromJson(text.toUtf8()).toJson(QJsonDocument::Compact));
    if (!text.endsWith(u'\n'))
        text += u'\n';
    if (m_resultCount > 0 && m_format != Format::Json && !m_embedded)
        out(QStringLiteral("\n")); // 결과 집합 사이 빈 줄
    out(text);
    ++m_resultCount;
}

void CliApp::recordHistory(const SqlStatement &statement, bool ok, qint64 elapsedMs, qint64 rows, const QString &error)
{
    if (!m_historySink && !m_history)
        return;
    HistoryEntry entry;
    entry.connectionId = m_profile.id;
    entry.connectionName = m_profile.name;
    entry.sql = statement.originalText();
    entry.ok = ok;
    entry.elapsedMs = elapsedMs;
    entry.rows = rows;
    entry.error = error;
    if (m_historySink)
        m_historySink(entry);
    else
        m_history->add(entry);
}

void CliApp::refreshSchemaAsync(std::function<void()> done)
{
    request(QStringLiteral("currentSchema"), map({{QStringLiteral("sessionId"), m_sessionId}}),
            [this, done](const QCborMap &result, const EngineError &error) {
                if (!error.isError()) {
                    m_currentSchema = result.value(QStringLiteral("schema")).toString();
                    emit stateChanged();
                }
                done();
            });
}

void CliApp::printConnections()
{
    const QList<ConnectionProfile> &profiles = m_store->profiles();
    if (m_format == Format::Json) {
        QJsonArray array;
        for (const ConnectionProfile &p : profiles) {
            QJsonObject object;
            object.insert(QStringLiteral("id"), p.id);
            object.insert(QStringLiteral("name"), p.name);
            object.insert(QStringLiteral("type"), Db::typeKey(p.type));
            object.insert(QStringLiteral("url"), Db::jdbcUrl(p));
            object.insert(QStringLiteral("user"), p.user);
            object.insert(QStringLiteral("environment"), Env::key(p.environment));
            array.append(object);
        }
        out(QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) + u'\n');
        return;
    }
    if (profiles.isEmpty()) {
        out(tr("No saved connections. Add one in DBaro (Database > New Connection).\n"));
        return;
    }
    QList<QStringList> rows;
    for (const ConnectionProfile &p : profiles)
        rows << QStringList{p.name, Db::typeName(p.type), Db::jdbcUrl(p), p.user, Env::name(p.environment), p.id};
    out(TextTable::render({tr("Name"), tr("Type"), tr("URL"), tr("User"), tr("Environment"), tr("Id")}, rows, {}, 70));
}

void CliApp::openGui(const ConnectionProfile &profile)
{
#ifdef Q_OS_WIN
    const QString program = QCoreApplication::applicationDirPath() + QStringLiteral("/dbaro.exe");
#else
    const QString program = QCoreApplication::applicationDirPath() + QStringLiteral("/dbaro");
#endif
    // DBaro가 이미 떠 있으면 그 프로세스에 넘겨 창만 연다 (단일 실행)
    if (!QFileInfo::exists(program) || !QProcess::startDetached(program, {QStringLiteral("--connection"), profile.id})) {
        err(tr("Could not start %1\n").arg(QDir::toNativeSeparators(program)));
        return;
    }
    info(tr("Opening \"%1\" in DBaro.\n").arg(profile.name));
}

// ---------------------------------------------------------------------------
// 대화형 (명령줄 프로그램과 GUI 탭이 함께 쓴다)

QString CliApp::prompt() const
{
    const QString first = m_currentSchema.isEmpty() ? QStringLiteral("%1> ").arg(m_profile.name)
                                                    : QStringLiteral("%1@%2> ").arg(m_currentSchema, m_profile.name);
    if (m_buffer.isEmpty())
        return first;
    return QString(qMax(0, TextTable::displayWidth(first) - 3), u' ') + QStringLiteral("-> ");
}

bool CliApp::isComplete(const QString &buffer) const
{
    // DESC는 SQL*Plus·psql처럼 ; 없이 줄 끝에서 끝난다
    static const QRegularExpression describe(QStringLiteral("^\\s*DESC(RIBE)?\\s+\\S+\\s*;?\\s*$"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (m_dialect != SqlDialect::MySql && describe.match(buffer).hasMatch())
        return true;
    const QList<SqlStatement> statements = SqlSplitter::split(buffer, m_dialect);
    if (statements.isEmpty())
        return true; // 주석뿐
    const SqlStatement &last = statements.last();
    // 구분자(; 나 / 줄, DELIMITER 문자열)가 아직 없으면 다음 줄을 더 받는다
    if (buffer.mid(last.end, last.terminatorEnd - last.end).trimmed().isEmpty())
        return false;
    return buffer.mid(last.terminatorEnd).trimmed().isEmpty();
}

bool CliApp::isCommand(const QString &trimmed) const
{
    static const QRegularExpression words(QStringLiteral("^(exit|quit|help)\\s*;?$"), QRegularExpression::CaseInsensitiveOption);
    return trimmed.startsWith(u'\\') || words.match(trimmed).hasMatch();
}

void CliApp::discardInput()
{
    m_buffer.clear();
    emit stateChanged();
}

void CliApp::submitLine(const QString &line, std::function<void()> done)
{
    if (m_busy) {
        done();
        return;
    }
    m_interrupted = false;
    setBusy(true);
    handleLine(line, &m_buffer, 0, {}, [this, done](bool ok) {
        if (!ok)
            m_anyFailed = true;
        setBusy(false);
        emit stateChanged(); // 프롬프트 (이어 쓰는 줄이면 ->)
        done();
    });
}

QString CliApp::withDelimiter(const QString &text) const
{
    if (m_dialect == SqlDialect::MySql && m_delimiter != u";")
        return QStringLiteral("DELIMITER %1\n").arg(m_delimiter) + text;
    return text;
}

bool CliApp::takeStatement(const QString &buffer, QString *script, bool *vertical) const
{
    // 끝이 \G(세로로) 또는 \g면 그 자리에서 끝난 문장 (문자열·주석 밖일 때만)
    const QString trimmed = buffer.trimmed();
    if (trimmed.endsWith(QStringLiteral("\\G")) || trimmed.endsWith(QStringLiteral("\\g"))) {
        const QString candidate = trimmed.left(trimmed.size() - 2);
        if (isComplete(withDelimiter(candidate + (m_dialect == SqlDialect::MySql ? m_delimiter : QStringLiteral(";"))))) {
            *script = withDelimiter(candidate);
            *vertical = trimmed.endsWith(QStringLiteral("\\G"));
            return true;
        }
    }
    const QString text = withDelimiter(buffer);
    if (!isComplete(text))
        return false;
    *script = text;
    *vertical = false;
    return true;
}

void CliApp::handleLine(const QString &rawLine, QString *buffer, int lineNumber, const FramePtr &frame,
                        std::function<void(bool)> done)
{
    QString line = rawLine;
    line.remove(u'\r');
    const QString trimmed = line.trimmed();

    // 문장 첫머리(비었거나 주석뿐)에서만 명령으로 본다
    if (SqlSplitter::split(withDelimiter(*buffer), m_dialect).isEmpty()) {
        if (trimmed.isEmpty() || (trimmed == u"/" && m_dialect == SqlDialect::Oracle)) {
            done(true); // 빈 줄, 문장 뒤의 남는 / 는 건너뛴다
            return;
        }
        const bool quit = QRegularExpression(QStringLiteral("^(exit|quit)\\b.*$"), QRegularExpression::CaseInsensitiveOption)
                              .match(trimmed)
                              .hasMatch();
        if (frame && quit) {
            // 스크립트 안의 EXIT: 스크립트를 끝낸다 (창이나 프로그램은 그대로)
            buffer->clear();
            abortScripts();
            done(true);
            return;
        }
        if (isScriptCommand(trimmed)) {
            buffer->clear();
            runScriptCommand(trimmed, frame, done);
            return;
        }
        if (isClientCommand(trimmed)) {
            buffer->clear();
            handleClientCommand(trimmed, frame, done);
            return;
        }
        if (isCommand(trimmed)) {
            buffer->clear();
            handleCommand(trimmed, done);
            return;
        }
    }

    if (frame && buffer->isEmpty())
        frame->bufferLine = lineNumber;
    *buffer += line + u'\n';
    QString script;
    bool vertical = false;
    if (!takeStatement(*buffer, &script, &vertical)) {
        done(true); // 다음 줄을 기다린다
        return;
    }
    const int firstLine = frame ? frame->bufferLine : 0;
    buffer->clear();
    if (m_sessionId.isEmpty()) {
        err(tr("Not connected.\n"));
        done(false);
        return;
    }
    runScriptAsync(script, done, frame, firstLine, vertical);
}

// ---------------------------------------------------------------------------
// 스크립트 파일 (@, @@, START)

QString CliApp::scriptFolder() const
{
    return ScriptFolders::folder(m_profile.type);
}

bool CliApp::isScriptCommand(const QString &trimmed) const
{
    if (trimmed.startsWith(u'@'))
        return true;
    if (m_dialect != SqlDialect::Oracle)
        return false; // PostgreSQL·MySQL의 START TRANSACTION
    static const QRegularExpression start(QStringLiteral("^STA(RT?)?\\s+(?!TRANSACTION\\b)\\S"), QRegularExpression::CaseInsensitiveOption);
    return start.match(trimmed).hasMatch();
}

namespace {

// 공백으로 나눈 인자 (따옴표로 감싼 것은 하나로)
QStringList splitArguments(const QString &text)
{
    QStringList args;
    QString current;
    QChar quote;
    bool inToken = false;
    for (const QChar ch : text) {
        if (!quote.isNull()) {
            if (ch == quote)
                quote = QChar();
            else
                current += ch;
            continue;
        }
        if (ch == u'"' || ch == u'\'') {
            quote = ch;
            inToken = true;
        } else if (ch.isSpace()) {
            if (inToken)
                args << current;
            current.clear();
            inToken = false;
        } else {
            current += ch;
            inToken = true;
        }
    }
    if (inToken)
        args << current;
    return args;
}

} // namespace

void CliApp::runScriptCommand(const QString &trimmed, const FramePtr &parent, std::function<void(bool)> done)
{
    QString rest;
    bool nested = false;
    if (trimmed.startsWith(QStringLiteral("@@"))) {
        nested = true;
        rest = trimmed.mid(2);
    } else if (trimmed.startsWith(u'@')) {
        rest = trimmed.mid(1);
    } else {
        rest = trimmed.section(QRegularExpression(QStringLiteral("\\s+")), 1);
    }
    if (rest.endsWith(u';'))
        rest.chop(1);
    QStringList args = splitArguments(rest.trimmed());
    if (args.isEmpty()) {
        err(tr("Usage: @file [arguments]  (\\scripts lists the files)\n"));
        done(false);
        return;
    }
    const QString name = args.takeFirst();
    if (m_frames.size() >= 20) {
        err(tr("Scripts are nested too deeply: %1\n").arg(name));
        done(false);
        return;
    }
    // @@: 부른 스크립트 옆 → 스크립트 폴더, @: 스크립트 폴더 → 부른 스크립트 옆 → 지금 폴더
    QStringList folders;
    const QString base = scriptFolder();
    if (nested && parent)
        folders << parent->dir << base;
    else
        folders << base << (parent ? parent->dir : QString());
    folders << QDir::currentPath();
    const QString path = ScriptFolders::resolve(name, folders);
    if (path.isEmpty()) {
        QStringList looked;
        for (const QString &folder : std::as_const(folders)) {
            if (!folder.isEmpty() && !looked.contains(QDir::toNativeSeparators(folder)))
                looked << QDir::toNativeSeparators(folder);
        }
        err(tr("Script not found: %1 (looked in %2)%3\n")
                .arg(name, looked.join(QStringLiteral(", ")),
                     base.isEmpty() ? tr(". Set the %1 script folder with \\scripts <folder>").arg(Db::typeName(m_profile.type))
                                    : QString()));
        done(false);
        return;
    }
    QString text;
    TextFileFormat format;
    QString readError;
    if (!TextFile::read(path, &text, &format, &readError)) {
        err(tr("Could not read %1: %2\n").arg(QDir::toNativeSeparators(path), readError));
        done(false);
        return;
    }
    runFileText(text, path, args, done);
}

void CliApp::runFileText(const QString &text, const QString &path, const QStringList &args, std::function<void(bool)> done)
{
    auto frame = std::make_shared<ScriptFrame>();
    frame->path = path;
    const QFileInfo info(path);
    frame->dir = info.exists() ? info.absolutePath() : QDir::currentPath();
    QString normalized = text;
    normalized.remove(u'\r');
    frame->lines = normalized.split(u'\n');
    frame->args = args;
    frame->done = std::move(done);
    frame->timer.start();
    m_frames << frame;
    runFrameNext(frame);
}

void CliApp::runFrameNext(const FramePtr &frame)
{
    // 한 줄씩 넘긴다. 바로 끝나는 줄(주석, PROMPT ...)은 이 자리에서 이어 가고, 실행을 기다리는 줄은 끝난 뒤 이어 간다.
    for (;;) {
        if (m_interrupted)
            abortScripts();
        if (frame->aborted || frame->index >= frame->lines.size()) {
            finishFrame(frame);
            return;
        }
        const int lineNumber = frame->index + 1;
        const QString line = frame->lines[frame->index++];
        auto state = std::make_shared<int>(0); // 0: 처리 중, 1: 바로 끝남, 2: 나중에 끝남
        handleLine(line, &frame->buffer, lineNumber, frame, [this, frame, state](bool ok) {
            if (!ok && m_scriptStopOnError)
                abortScripts(); // WHENEVER SQLERROR EXIT (기본은 SQL*Plus처럼 계속)
            if (*state == 0) {
                *state = 1;
                return;
            }
            runFrameNext(frame);
        });
        if (*state == 1)
            continue;
        *state = 2;
        return;
    }
}

void CliApp::finishFrame(const FramePtr &frame)
{
    // ; 없이 끝난 마지막 문장도 실행한다
    if (!frame->aborted && !SqlSplitter::split(withDelimiter(frame->buffer), m_dialect).isEmpty()) {
        const QString script = withDelimiter(std::exchange(frame->buffer, {}));
        runScriptAsync(script, [this, frame](bool ok) {
            if (!ok && m_scriptStopOnError)
                abortScripts();
            finishFrame(frame);
        }, frame, frame->bufferLine, false);
        return;
    }
    m_frames.removeOne(frame);
    if (m_feedback && frame->path != u"stdin") {
        const QString name = QFileInfo(frame->path).fileName();
        info(frame->failed > 0 ? tr("@%1: %2, %3 failed (%4)\n")
                                     .arg(name, count(frame->statements, "statement", "statements"))
                                     .arg(frame->failed)
                                     .arg(elapsedText(frame->timer.elapsed()))
                               : tr("@%1: %2 (%3)\n")
                                     .arg(name, count(frame->statements, "statement", "statements"),
                                          elapsedText(frame->timer.elapsed())));
    }
    m_location.clear();
    frame->done(frame->failed == 0);
}

void CliApp::abortScripts()
{
    for (const FramePtr &frame : std::as_const(m_frames))
        frame->aborted = true;
}

// ---------------------------------------------------------------------------
// SQL*Plus 명령 (Oracle·Tibero), PROMPT·REM (모든 DB), DELIMITER (MySQL·MariaDB)

namespace {

struct ClientCommand
{
    const char *name;
    int minimum; // 줄여 쓸 수 있는 가장 짧은 길이
};

const ClientCommand kSqlPlusCommands[] = {
    {"PROMPT", 3}, {"REMARK", 3}, {"DEFINE", 3},   {"UNDEFINE", 5}, {"WHENEVER", 8}, {"EXECUTE", 4},
    {"SPOOL", 3},  {"COLUMN", 3}, {"TTITLE", 3},   {"BTITLE", 3},   {"BREAK", 3},    {"COMPUTE", 4},
    {"CLEAR", 2},  {"TIMING", 4}, {"PAUSE", 3},    {"SHOW", 3},     {"HOST", 2},     {"CONNECT", 4},
    {"DISCONNECT", 4}, {"VARIABLE", 3}, {"PRINT", 3}, {"ACCEPT", 3}, {"PASSWORD", 5},
};

// SQL*Plus의 SET 항목과 줄여 쓸 수 있는 가장 짧은 길이 (HEA[DING], FEED[BACK] ...).
// 이 목록에 없는 SET(SET TRANSACTION, SET ROLE, SET CONSTRAINTS)은 SQL이다.
const ClientCommand kSetOptions[] = {
    {"APPINFO", 4},      {"ARRAYSIZE", 5},    {"AUTOCOMMIT", 4},  {"AUTOPRINT", 5},    {"AUTORECOVERY", 12}, {"AUTOTRACE", 5},
    {"BLOCKTERMINATOR", 3}, {"CMDSEP", 4},    {"COLINVISIBLE", 7}, {"COLSEP", 6},      {"CONCAT", 3},       {"COPYCOMMIT", 5},
    {"COPYTYPECHECK", 13}, {"DEFINE", 3},     {"DESCRIBE", 8},    {"ECHO", 4},         {"EDITFILE", 5},     {"EMBEDDED", 3},
    {"ERRORLOGGING", 6}, {"ESCAPE", 3},       {"ESCCHAR", 7},     {"EXITCOMMIT", 5},   {"FEEDBACK", 4},     {"FLAGGER", 7},
    {"FLUSH", 3},        {"HEADING", 3},      {"HEADSEP", 5},     {"INSTANCE", 8},     {"LINESIZE", 3},     {"LOBOFFSET", 5},
    {"LOBPREFETCH", 11}, {"LOGSOURCE", 9},    {"LONG", 4},        {"LONGCHUNKSIZE", 5}, {"MARKUP", 4},      {"NEWPAGE", 4},
    {"NULL", 4},         {"NUMFORMAT", 4},    {"NUMWIDTH", 3},    {"PAGESIZE", 5},     {"PAUSE", 3},        {"RECSEP", 6},
    {"RECSEPCHAR", 10},  {"ROWLIMIT", 8},     {"ROWPREFETCH", 11}, {"SECUREDCOL", 10}, {"SERVEROUTPUT", 9}, {"SHIFTINOUT", 5},
    {"SHOWMODE", 4},     {"SQLBLANKLINES", 5}, {"SQLCASE", 4},    {"SQLCONTINUE", 5},  {"SQLNUMBER", 4},    {"SQLPLUSCOMPATIBILITY", 13},
    {"SQLPREFIX", 6},    {"SQLPROMPT", 4},    {"SQLTERMINATOR", 4}, {"STATEMENTCACHE", 10}, {"SUFFIX", 3},  {"TAB", 3},
    {"TERMOUT", 4},      {"TIME", 2},         {"TIMING", 4},      {"TRIMOUT", 4},      {"TRIMSPOOL", 5},    {"UNDERLINE", 3},
    {"VERIFY", 3},       {"WRAP", 3},         {"XQUERY", 6},
};

QString setOptionName(const QString &word)
{
    const QString upper = word.toUpper();
    for (const ClientCommand &option : kSetOptions) {
        const QString name = QLatin1String(option.name);
        if (upper.size() >= option.minimum && name.startsWith(upper))
            return name;
    }
    return {};
}

// 따옴표로 감싼 글은 하나로 (따옴표는 뺀다)
QStringList commandTokens(const QString &text)
{
    QStringList tokens;
    QString current;
    QChar quote;
    bool inToken = false;
    for (const QChar ch : text) {
        if (!quote.isNull()) {
            if (ch == quote)
                quote = QChar();
            else
                current += ch;
            continue;
        }
        if (ch == u'"' || ch == u'\'') {
            quote = ch;
            inToken = true;
        } else if (ch.isSpace()) {
            if (inToken)
                tokens << current;
            current.clear();
            inToken = false;
        } else {
            current += ch;
            inToken = true;
        }
    }
    if (inToken)
        tokens << current;
    return tokens;
}

bool isWord(const QString &token, const char *full, int minimum)
{
    const QString upper = token.toUpper();
    return upper.size() >= minimum && QLatin1String(full).startsWith(upper);
}

QString commandName(const QString &word)
{
    const QString upper = word.toUpper();
    for (const ClientCommand &command : kSqlPlusCommands) {
        const QString name = QLatin1String(command.name);
        if (upper.size() >= command.minimum && name.startsWith(upper))
            return name;
    }
    return {};
}

} // namespace

bool CliApp::isClientCommand(const QString &trimmed) const
{
    const QString first = trimmed.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0).toUpper();
    if (m_dialect == SqlDialect::MySql)
        return first == u"DELIMITER";
    const QString name = commandName(first);
    if (name == u"PROMPT" || name == u"REMARK")
        return true; // 모든 DB (SQL 문장은 이 낱말로 시작하지 않는다)
    if (m_dialect != SqlDialect::Oracle)
        return false;
    if (first == u"SET")
        return !setOptionName(trimmed.section(QRegularExpression(QStringLiteral("\\s+")), 1, 1)).isEmpty();
    return !name.isEmpty();
}

void CliApp::handleClientCommand(const QString &trimmed, const FramePtr &frame, std::function<void(bool)> done)
{
    const QStringList words = trimmed.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const QString first = words.value(0).toUpper();
    const QString rest = trimmed.mid(words.value(0).size()).trimmed();

    if (first == u"DELIMITER") {
        QString delimiter = rest;
        if (delimiter.isEmpty()) {
            err(tr("Usage: DELIMITER <text>\n"));
            done(false);
            return;
        }
        m_delimiter = delimiter;
        done(true);
        return;
    }
    if (first == u"SET") {
        const QString option = setOptionName(words.value(1));
        const QString value = words.value(2).toUpper();
        if (option == u"ECHO")
            m_echo = value == u"ON";
        else if (option == u"FEEDBACK")
            m_feedback = value != u"OFF" && value != u"0";
        else if (option == u"DEFINE")
            m_substitution = value != u"OFF";
        else if (option == u"HEADING")
            m_heading = value != u"OFF";
        else if (option == u"PAGESIZE")
            m_pageSizeZero = value == u"0"; // SQL*Plus: 0이면 머리글도 없다
        else if (option == u"AUTOCOMMIT") {
            handleCommand(QStringLiteral("\\autocommit %1").arg(value == u"OFF" ? QStringLiteral("off") : QStringLiteral("on")),
                          done);
            return;
        }
        done(true); // 화면 모양 설정(LINESIZE, PAGESIZE ...)은 여기서 쓸 일이 없다
        return;
    }

    const QString name = commandName(first);
    if (name == u"PROMPT") {
        out(substituteText(rest, frame) + u'\n');
        done(true);
        return;
    }
    if (name == u"REMARK" || name == u"COLUMN" || name == u"TTITLE" || name == u"BTITLE" || name == u"BREAK"
        || name == u"COMPUTE" || name == u"CLEAR" || name == u"TIMING" || name == u"PAUSE") {
        done(true); // 화면 꾸미기 명령은 무시한다
        return;
    }
    if (name == u"WHENEVER") {
        if (words.value(1).toUpper() == u"SQLERROR")
            m_scriptStopOnError = words.value(2).toUpper() == u"EXIT";
        done(true);
        return;
    }
    if (name == u"VARIABLE") {
        handleVariable(rest, done);
        return;
    }
    if (name == u"PRINT") {
        handlePrint(rest, done);
        return;
    }
    if (name == u"ACCEPT") {
        handleAccept(rest, done);
        return;
    }
    if (name == u"DEFINE") {
        if (rest.isEmpty()) {
            QStringList names = m_defines.keys();
            names.sort();
            for (const QString &key : std::as_const(names))
                out(QStringLiteral("DEFINE %1 = \"%2\"\n").arg(key, m_defines.value(key)));
            done(true);
            return;
        }
        const int equals = int(rest.indexOf(u'='));
        if (equals < 0) {
            const QString key = rest.toUpper();
            if (m_defines.contains(key))
                out(QStringLiteral("DEFINE %1 = \"%2\"\n").arg(key, m_defines.value(key)));
            else
                err(tr("Symbol %1 is UNDEFINED\n").arg(rest));
            done(m_defines.contains(key));
            return;
        }
        const QString key = rest.left(equals).trimmed().toUpper();
        QString value = rest.mid(equals + 1).trimmed();
        if (value.size() >= 2 && (value.startsWith(u'\'') || value.startsWith(u'"')) && value.endsWith(value.at(0)))
            value = value.mid(1, value.size() - 2);
        m_defines.insert(key, value);
        done(true);
        return;
    }
    if (name == u"UNDEFINE") {
        for (const QString &word : words.mid(1))
            m_defines.remove(word.toUpper());
        done(true);
        return;
    }
    if (name == u"EXECUTE") {
        // EXEC proc(...) = BEGIN proc(...); END;
        QString call = rest;
        while (call.endsWith(u';'))
            call.chop(1);
        if (m_sessionId.isEmpty()) {
            err(tr("Not connected.\n"));
            done(false);
            return;
        }
        runScriptAsync(QStringLiteral("BEGIN %1; END;\n/\n").arg(call), done, frame, frame ? frame->index : 0);
        return;
    }
    // SPOOL, HOST, CONNECT, VARIABLE ...: 여기서는 할 수 없다
    err(tr("SQL*Plus command not supported here, skipped: %1\n").arg(name));
    done(true);
}

// ---------------------------------------------------------------------------
// 치환 변수 (&1, &이름, &&이름): Oracle·Tibero

bool CliApp::substitute(SqlStatement *statement, const FramePtr &frame)
{
    // SQL*Plus처럼 문자열 안('&name')도 바꾼다 (WHERE ename = '&name'). 주석 안은 그대로 둔다.
    if (m_dialect != SqlDialect::Oracle || !m_substitution || !statement->text.contains(u'&'))
        return true;
    const QString text = statement->text;
    const qsizetype size = text.size();
    QString result;
    bool inString = false;
    qsizetype i = 0;
    while (i < size) {
        const QChar ch = text.at(i);
        if (!inString && ch == u'-' && i + 1 < size && text.at(i + 1) == u'-') {
            qsizetype end = text.indexOf(u'\n', i);
            end = end < 0 ? size : end;
            result += text.mid(i, end - i);
            i = end;
            continue;
        }
        if (!inString && ch == u'/' && i + 1 < size && text.at(i + 1) == u'*') {
            qsizetype end = text.indexOf(QStringLiteral("*/"), i + 2);
            end = end < 0 ? size : end + 2;
            result += text.mid(i, end - i);
            i = end;
            continue;
        }
        if (ch == u'\'')
            inString = !inString; // ''는 두 번 바뀌어 제자리
        if (ch != u'&') {
            result += ch;
            ++i;
            continue;
        }
        qsizetype j = i + 1;
        const bool define = j < size && text.at(j) == u'&';
        if (define)
            ++j;
        const qsizetype nameStart = j;
        while (j < size && (text.at(j).isLetterOrNumber() || text.at(j) == u'_' || text.at(j) == u'$' || text.at(j) == u'#'))
            ++j;
        if (j == nameStart) { // & 하나뿐
            result += text.mid(i, j - i);
            i = j;
            continue;
        }
        const QString name = text.mid(nameStart, j - nameStart);
        if (j < size && text.at(j) == u'.')
            ++j; // &name. 의 점은 이름이 끝났다는 표시
        const QString key = name.toUpper();
        bool isNumber = false;
        const int number = name.toInt(&isNumber);
        QString value;
        if (isNumber && frame && number >= 1 && number <= frame->args.size()) {
            value = frame->args[number - 1];
        } else if (m_defines.contains(key)) {
            value = m_defines.value(key);
        } else if (m_askValue && m_askValue(tr("Enter value for %1: ").arg(name), false, &value)) {
            if (define)
                m_defines.insert(key, value); // &&는 한 번 정하면 다시 묻지 않는다
        } else {
            err(tr("Substitution variable &%1 is not defined (DEFINE %1 = value, or SET DEFINE OFF)\n").arg(name));
            return false;
        }
        result += value;
        i = j;
    }
    statement->sourceText = text;
    statement->text = result;
    return true;
}

// ---------------------------------------------------------------------------
// ACCEPT, VARIABLE, PRINT, :바인드 변수 (Oracle·Tibero)

void CliApp::handleAccept(const QString &rest, std::function<void(bool)> done)
{
    // ACC[EPT] 변수 [NUM[BER]|CHAR|DATE|BINARY_FLOAT|BINARY_DOUBLE] [FOR[MAT] f] [DEF[AULT] d] [PROMPT 글|NOPR[OMPT]] [HIDE]
    const QStringList tokens = commandTokens(rest);
    if (tokens.isEmpty()) {
        err(tr("Usage: ACCEPT variable [DEFAULT value] [PROMPT text] [HIDE]\n"));
        done(false);
        return;
    }
    const QString variable = tokens.first();
    QString prompt;
    QString defaultValue;
    bool hasDefault = false;
    bool hide = false;
    bool noPrompt = false;
    const auto isKeyword = [](const QString &token) {
        return isWord(token, "FORMAT", 3) || isWord(token, "DEFAULT", 3) || isWord(token, "PROMPT", 5)
               || isWord(token, "NOPROMPT", 4) || token.compare(QStringLiteral("HIDE"), Qt::CaseInsensitive) == 0;
    };
    for (int i = 1; i < tokens.size(); ++i) {
        const QString &token = tokens[i];
        if (isWord(token, "FORMAT", 3)) {
            ++i; // 입력 모양 검사는 하지 않는다
        } else if (isWord(token, "DEFAULT", 3)) {
            defaultValue = tokens.value(++i);
            hasDefault = true;
        } else if (isWord(token, "PROMPT", 5)) {
            QStringList words;
            while (i + 1 < tokens.size() && !isKeyword(tokens[i + 1]))
                words << tokens[++i];
            prompt = words.join(u' ');
        } else if (isWord(token, "NOPROMPT", 4)) {
            noPrompt = true;
        } else if (token.compare(QStringLiteral("HIDE"), Qt::CaseInsensitive) == 0) {
            hide = true;
        }
    }
    if (prompt.isEmpty() && !noPrompt)
        prompt = tr("Enter value for %1: ").arg(variable);
    QString value;
    if (!m_askValue) {
        if (!hasDefault) {
            err(tr("ACCEPT %1: no way to ask for a value here (use DEFINE %1 = value)\n").arg(variable));
            done(false);
            return;
        }
        value = defaultValue;
    } else if (!m_askValue(prompt, hide, &value)) {
        err(tr("Cancelled.\n"));
        abortScripts(); // SQL*Plus에서 입력을 끊은 것과 같다
        done(false);
        return;
    }
    if (value.isEmpty() && hasDefault)
        value = defaultValue;
    m_defines.insert(variable.toUpper(), value);
    done(true);
}

int CliApp::findBindVar(const QString &name) const
{
    const QString key = name.toUpper();
    for (int i = 0; i < m_bindVars.size(); ++i) {
        if (m_bindVars[i].name == key)
            return i;
    }
    return -1;
}

void CliApp::handleVariable(const QString &rest, std::function<void(bool)> done)
{
    // VAR[IABLE] [이름 [타입] [= 값]]
    const auto describe = [this](const BindVar &variable) {
        out(QStringLiteral("VARIABLE %1 %2\n").arg(variable.name, variable.type));
    };
    if (rest.isEmpty()) {
        if (m_bindVars.isEmpty())
            out(tr("No bind variables declared.\n"));
        for (const BindVar &variable : std::as_const(m_bindVars))
            describe(variable);
        done(true);
        return;
    }
    QString declaration = rest;
    QString initial;
    bool hasInitial = false;
    const qsizetype equals = declaration.indexOf(u'=');
    if (equals >= 0) {
        initial = declaration.mid(equals + 1).trimmed();
        if (initial.size() >= 2 && (initial.startsWith(u'\'') || initial.startsWith(u'"')) && initial.endsWith(initial.at(0)))
            initial = initial.mid(1, initial.size() - 2);
        declaration = declaration.left(equals).trimmed();
        hasInitial = true;
    }
    const QString name = declaration.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
    const QString type = declaration.section(QRegularExpression(QStringLiteral("\\s+")), 1).simplified().toUpper();
    if (type.isEmpty()) {
        const int index = findBindVar(name);
        if (index < 0) {
            err(tr("Bind variable \"%1\" not declared.\n").arg(name.toUpper()));
            done(false);
            return;
        }
        describe(m_bindVars[index]);
        done(true);
        return;
    }
    BindVar variable;
    variable.name = name.toUpper();
    variable.type = type;
    const QString base = type.section(u'(', 0, 0).trimmed();
    if (base == u"NUMBER" || base == u"BINARY_FLOAT" || base == u"BINARY_DOUBLE")
        variable.kind = QStringLiteral("NUMERIC");
    else if (base == u"CLOB" || base == u"NCLOB")
        variable.kind = QStringLiteral("CLOB");
    else if (base == u"VARCHAR2" || base == u"VARCHAR" || base == u"NVARCHAR2" || base == u"CHAR" || base == u"NCHAR")
        variable.kind = QStringLiteral("VARCHAR");
    else {
        err(tr("VARIABLE %1: type %2 is not supported here (NUMBER, VARCHAR2, CHAR, CLOB)\n").arg(variable.name, type));
        done(false);
        return;
    }
    variable.value = hasInitial ? QCborValue(initial) : QCborValue(QCborValue::Null);
    const int existing = findBindVar(variable.name);
    if (existing >= 0)
        m_bindVars[existing] = variable;
    else
        m_bindVars << variable;
    done(true);
}

void CliApp::handlePrint(const QString &rest, std::function<void(bool)> done)
{
    // PRI[NT] [이름 ...]: 이름이 없으면 모두
    QList<int> indexes;
    const QStringList names = rest.split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
    if (names.isEmpty()) {
        for (int i = 0; i < m_bindVars.size(); ++i)
            indexes << i;
    }
    for (QString name : names) {
        if (name.startsWith(u':'))
            name.remove(0, 1);
        const int index = findBindVar(name);
        if (index < 0) {
            err(tr("Bind variable \"%1\" not declared.\n").arg(name.toUpper()));
            done(false);
            return;
        }
        indexes << index;
    }
    if (indexes.isEmpty()) {
        out(tr("No bind variables declared.\n"));
        done(true);
        return;
    }
    for (const int index : std::as_const(indexes)) {
        const BindVar &variable = m_bindVars[index];
        const QString value = variable.value.isNull() || variable.value.isUndefined() ? QString()
                                                                                       : ResultExport::valueText(variable.value);
        out(TextTable::render({variable.name}, {{value}}, {variable.kind == u"NUMERIC"}, m_maxCellWidth, m_heading));
    }
    done(true);
}

bool CliApp::prepareBinds(SqlStatement *statement, QCborArray *binds, QList<int> *outVars)
{
    if (m_dialect != SqlDialect::Oracle || !statement->text.contains(u':'))
        return true;
    QList<BindVariable> variables;
    int positional = 0;
    const QList<BindOccurrence> occurrences = SqlBinds::scan(statement->text, m_dialect, &variables, &positional);
    QList<BindOccurrence> named;
    for (const BindOccurrence &occurrence : occurrences) {
        if (variables[occurrence.variable].kind == BindVariable::Kind::Named)
            named << occurrence;
    }
    if (named.isEmpty())
        return true;
    // 정하지 않은 바인드 변수는 SQL*Plus처럼 실행 전에 알린다
    QList<int> varOf(variables.size(), -1);
    for (int i = 0; i < variables.size(); ++i) {
        if (variables[i].kind != BindVariable::Kind::Named)
            continue;
        varOf[i] = findBindVar(variables[i].name);
        if (varOf[i] < 0) {
            err(tr("Bind variable \"%1\" not declared. (VARIABLE %1 VARCHAR2(4000))\n").arg(variables[i].name.toUpper()));
            return false;
        }
    }
    const auto inBind = [this](int var) {
        QCborMap bind;
        bind.insert(QStringLiteral("value"), m_bindVars[var].value);
        bind.insert(QStringLiteral("type"), m_bindVars[var].kind);
        bind.insert(QStringLiteral("out"), false);
        return bind;
    };
    QStringList replacements;
    for (int i = 0; i < variables.size(); ++i)
        replacements << QString();
    if (statement->sourceText.isEmpty())
        statement->sourceText = statement->text;

    const QString keyword = SqlSafety::leadingKeyword(statement->text);
    if (keyword == u"BEGIN" || keyword == u"DECLARE") {
        // PL/SQL 블록: 바인드 변수마다 지역 변수 하나로 받고, 블록이 끝나면 바뀐 값을 OUT으로 돌려받는다.
        //   DECLARE dbaro_b1 VARCHAR2(32767) := ?; BEGIN <원래 블록>  ? := dbaro_b1; END;
        QList<int> distinct;
        for (const BindOccurrence &occurrence : std::as_const(named)) {
            const int var = varOf[occurrence.variable];
            if (!distinct.contains(var))
                distinct << var;
        }
        for (int i = 0; i < variables.size(); ++i) {
            if (varOf[i] >= 0)
                replacements[i] = QStringLiteral("dbaro_b%1").arg(distinct.indexOf(varOf[i]) + 1);
        }
        QList<SqlEdit> edits;
        const QString body = SqlBinds::substitute(statement->text, named, replacements, &edits);
        QString wrapped = QStringLiteral("DECLARE\n");
        for (int k = 0; k < distinct.size(); ++k) {
            const BindVar &variable = m_bindVars[distinct[k]];
            const QString plsqlType = variable.kind == u"NUMERIC" ? QStringLiteral("NUMBER")
                                      : variable.kind == u"CLOB" ? QStringLiteral("CLOB")
                                                                 : QStringLiteral("VARCHAR2(32767)");
            wrapped += QStringLiteral("  dbaro_b%1 %2 := ?;\n").arg(k + 1).arg(plsqlType);
            binds->append(inBind(distinct[k]));
            outVars->append(-1);
        }
        wrapped += QStringLiteral("BEGIN\n") + body.trimmed() + u'\n';
        for (int k = 0; k < distinct.size(); ++k) {
            wrapped += QStringLiteral("  ? := dbaro_b%1;\n").arg(k + 1);
            QCborMap out;
            out.insert(QStringLiteral("value"), QCborValue(QCborValue::Null));
            out.insert(QStringLiteral("type"), m_bindVars[distinct[k]].kind);
            out.insert(QStringLiteral("out"), true);
            binds->append(out);
            outVars->append(distinct[k]);
        }
        wrapped += QStringLiteral("END;");
        statement->text = wrapped;
        return true;
    }
    // SQL 문장: 나온 자리마다 ? 하나와 그 값
    for (int i = 0; i < variables.size(); ++i) {
        if (varOf[i] >= 0)
            replacements[i] = QStringLiteral("?");
    }
    QList<BindOccurrence> ordered = named;
    std::sort(ordered.begin(), ordered.end(), [](const BindOccurrence &a, const BindOccurrence &b) { return a.start < b.start; });
    for (const BindOccurrence &occurrence : std::as_const(ordered)) {
        binds->append(inBind(varOf[occurrence.variable]));
        outVars->append(-1);
    }
    QList<SqlEdit> edits;
    statement->text = SqlBinds::substitute(statement->text, named, replacements, &edits);
    return true;
}

QString CliApp::substituteText(const QString &text, const FramePtr &frame) const
{
    // PROMPT 글 안의 &이름, &1 (정하지 않은 것은 그대로)
    if (m_dialect != SqlDialect::Oracle || !m_substitution || !text.contains(u'&'))
        return text;
    static const QRegularExpression pattern(QStringLiteral("&&?([A-Za-z0-9_$#]+)\\.?"));
    QString result;
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = pattern.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString name = match.captured(1);
        bool isNumber = false;
        const int number = name.toInt(&isNumber);
        QString value = match.captured(0);
        if (isNumber && frame && number >= 1 && number <= frame->args.size())
            value = frame->args[number - 1];
        else if (m_defines.contains(name.toUpper()))
            value = m_defines.value(name.toUpper());
        result += text.mid(last, match.capturedStart() - last) + value;
        last = match.capturedEnd();
    }
    return result + text.mid(last);
}

int CliApp::repl()
{
    if (!m_quiet)
        printBanner();
    while (!m_quit) {
        m_out(prompt());
        QString line;
        if (!m_readLine(&line))
            break; // EOF (Ctrl+Z, Ctrl+D)
        if (m_clearInput.exchange(false)) {
            discardInput(); // Ctrl+C: 쓰던 문장을 버린다
            m_out(QStringLiteral("\n"));
            continue;
        }
        await([&](std::function<void()> finish) { submitLine(line, finish); });
    }
    m_out(QStringLiteral("\n"));
    return m_anyFailed && !m_interactiveInput ? 1 : 0;
}

void CliApp::handleCommand(const QString &line, std::function<void(bool)> done)
{
    QString text = line;
    if (text.endsWith(u';'))
        text.chop(1);
    const QString command = text.section(u' ', 0, 0, QString::SectionSkipEmpty).toLower();
    const QString argument = text.section(u' ', 1, -1, QString::SectionSkipEmpty).trimmed();
    const auto fail = [this, done](const QString &message) {
        err(message);
        done(false);
    };

    if (command == u"\\q" || command == u"\\quit" || command == u"exit" || command == u"quit") {
        m_quit = true;
        emit quitRequested();
        done(true);
        return;
    }
    if (command == u"\\help" || command == u"\\?" || command == u"help") {
        m_out(tr("Commands:\n"
                 "  \\q                     quit (also exit, quit)\n"
                 "  \\format [table|csv|tsv|json]  show or change the output format\n"
                 "  \\max [n]               rows to fetch per result (0 = all)\n"
                 "  \\width [n]             widest table cell in columns (0 = never shorten)\n"
                 "  \\schema [name]         show or change the current schema\n"
                 "  \\schemas               list schemas\n"
                 "  \\autocommit [on|off]   show or change auto-commit\n"
                 "  \\commit, \\rollback     end the transaction\n"
                 "  \\gui                   open this connection in DBaro\n"
                 "  \\scripts [folder]      show or set the script folder of this DBMS, list its .sql files\n"
                 "  @file [args]          run a SQL file from the script folder (.sql may be left out; &1, &2 = args)\n"
                 "  @@file                run a file next to the calling script\n"
                 "  statement\\G           show the result vertically, one row at a time\n"
                 "Oracle/Tibero scripts may use PROMPT, REM, SET ECHO/DEFINE/FEEDBACK/HEADING, ACCEPT, DEFINE, UNDEFINE,\n"
                 "VARIABLE, PRINT, EXEC, :bind variables, &substitution variables and WHENEVER SQLERROR EXIT|CONTINUE\n"
                 "(scripts go on after an error unless WHENEVER SQLERROR EXIT). MySQL/MariaDB scripts may use DELIMITER.\n"
                 "End SQL statements with ';'. Oracle PL/SQL blocks end with a line holding only '/'.\n"
                 "Ctrl+C cancels the running statement or the statement being typed.\n"));
        done(true);
        return;
    }
    if (command == u"\\format") {
        if (!argument.isEmpty() && !parseFormat(argument, &m_format)) {
            fail(tr("Unknown format: %1\n").arg(argument));
            return;
        }
        m_out(tr("Format: %1\n").arg(formatName(m_format)));
        done(true);
        return;
    }
    if (command == u"\\max") {
        if (!argument.isEmpty()) {
            bool ok = false;
            const int value = argument.toInt(&ok);
            if (!ok || value < 0) {
                fail(tr("\\max needs a number (0 = all rows)\n"));
                return;
            }
            m_maxRows = value;
        }
        m_out(m_maxRows == 0 ? tr("Fetching all rows\n") : tr("Fetching up to %1 rows\n").arg(m_maxRows));
        done(true);
        return;
    }
    if (command == u"\\scripts") {
        if (!argument.isEmpty()) {
            QString folder = argument;
            if (folder.size() >= 2 && (folder.startsWith(u'"') || folder.startsWith(u'\'')) && folder.endsWith(folder.at(0)))
                folder = folder.mid(1, folder.size() - 2);
            if (!QFileInfo(folder).isDir()) {
                fail(tr("Not a folder: %1\n").arg(folder));
                return;
            }
            ScriptFolders::setFolder(m_profile.type, QFileInfo(folder).absoluteFilePath());
        }
        const QString folder = scriptFolder();
        if (folder.isEmpty()) {
            m_out(tr("No %1 script folder yet. Set it with \\scripts <folder>.\n").arg(Db::typeName(m_profile.type)));
            done(true);
            return;
        }
        const QStringList files = ScriptFolders::list(folder);
        m_out(tr("%1 scripts: %2 (%3)\n")
                  .arg(Db::typeName(m_profile.type), QDir::toNativeSeparators(folder), count(files.size(), "file", "files")));
        for (const QString &file : files)
            m_out(QStringLiteral("  @%1\n").arg(QString(file).replace(QStringLiteral(".sql"), QString(), Qt::CaseInsensitive)));
        done(true);
        return;
    }
    if (command == u"\\width") {
        if (!argument.isEmpty()) {
            bool ok = false;
            const int value = argument.toInt(&ok);
            if (!ok || value < 0) {
                fail(tr("\\width needs a number (0 = never shorten)\n"));
                return;
            }
            m_maxCellWidth = value;
        }
        m_out(m_maxCellWidth == 0 ? tr("Table cells are never shortened\n")
                                  : tr("Table cells are shortened past %1 columns\n").arg(m_maxCellWidth));
        done(true);
        return;
    }
    if (command == u"\\gui") {
        if (m_embedded)
            m_out(tr("This console is already in DBaro.\n"));
        else
            openGui(m_profile);
        done(true);
        return;
    }
    if (m_sessionId.isEmpty()) {
        fail(tr("Not connected.\n"));
        return;
    }
    const QCborMap session = map({{QStringLiteral("sessionId"), m_sessionId}});
    if (command == u"\\schema") {
        if (argument.isEmpty()) {
            refreshSchemaAsync([this, done] {
                m_out(tr("Current schema: %1\n").arg(m_currentSchema));
                done(true);
            });
            return;
        }
        QCborMap params = session;
        params.insert(QStringLiteral("schema"), argument);
        request(QStringLiteral("setSchema"), params, [this, done, fail](const QCborMap &result, const EngineError &error) {
            if (error.isError()) {
                fail(tr("ERROR: %1\n").arg(error.message));
                return;
            }
            m_currentSchema = result.value(QStringLiteral("schema")).toString();
            const QString sql = result.value(QStringLiteral("sql")).toString();
            info(sql.isEmpty() ? tr("Current schema: %1\n").arg(m_currentSchema)
                               : tr("%1\nCurrent schema: %2\n").arg(sql, m_currentSchema));
            done(true);
        });
        return;
    }
    if (command == u"\\schemas") {
        request(QStringLiteral("metaSchemas"), session, [this, done, fail](const QCborMap &result, const EngineError &error) {
            if (error.isError()) {
                fail(tr("ERROR: %1\n").arg(error.message));
                return;
            }
            const QString current = result.value(QStringLiteral("current")).toString();
            for (const QCborValue &value : result.value(QStringLiteral("names")).toArray()) {
                const QString name = value.toString();
                m_out((name.compare(m_currentSchema.isEmpty() ? current : m_currentSchema, Qt::CaseInsensitive) == 0
                           ? QStringLiteral("* ")
                           : QStringLiteral("  "))
                      + name + u'\n');
            }
            done(true);
        });
        return;
    }
    if (command == u"\\commit" || command == u"\\rollback") {
        const bool commit = command == u"\\commit";
        request(command.mid(1), session, [this, done, fail, commit](const QCborMap &, const EngineError &error) {
            if (error.isError()) {
                fail(tr("ERROR: %1\n").arg(error.message));
                return;
            }
            m_pendingTransaction = false;
            info(commit ? tr("Committed.\n") : tr("Rolled back.\n"));
            if (commit) {
                done(true);
                return;
            }
            refreshSchemaAsync([done] { done(true); }); // PostgreSQL은 롤백하면 SET search_path도 되돌린다
        });
        return;
    }
    if (command == u"\\autocommit") {
        if (argument.isEmpty()) {
            m_out(tr("Auto-commit is %1\n").arg(m_autoCommit ? tr("on") : tr("off")));
            done(true);
            return;
        }
        const QString value = argument.toLower();
        if (value != u"on" && value != u"off") {
            fail(tr("\\autocommit needs on or off\n"));
            return;
        }
        const bool on = value == u"on";
        QCborMap params = session;
        params.insert(QStringLiteral("autoCommit"), on);
        request(QStringLiteral("setAutoCommit"), params, [this, done, fail, on](const QCborMap &, const EngineError &error) {
            if (error.isError()) {
                fail(tr("ERROR: %1\n").arg(error.message));
                return;
            }
            m_autoCommit = on;
            if (on)
                m_pendingTransaction = false; // JDBC는 자동 커밋을 켜면 커밋한다
            info(tr("Auto-commit is %1\n").arg(on ? tr("on") : tr("off")));
            done(true);
        });
        return;
    }
    fail(tr("Unknown command: %1 (\\help lists the commands)\n").arg(command));
}

// ---------------------------------------------------------------------------
// 출력

void CliApp::out(const QString &text)
{
    if (m_outputFile.isOpen()) {
        m_outputFile.write(text.toUtf8());
        return;
    }
    m_out(text);
}

void CliApp::err(const QString &text)
{
    m_err(text);
}

void CliApp::info(const QString &text)
{
    if (m_quiet)
        return;
    if (m_embedded || (m_format == Format::Table && !m_outputFile.isOpen()))
        m_out(text);
    else
        m_err(text);
}
