#include "ClaudeCodeLauncher.h"

#include "ClaudeCodeDialog.h"
#include "core/ScriptFolders.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace ClaudeCodeLauncher {

namespace {
[[maybe_unused]] constexpr int kMaxPromptChars = 20000; // 맥락 글과 합쳐 Windows 명령줄(32767자) 안에 들게
}

QString claudePath()
{
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("claude"));
    if (!onPath.isEmpty())
        return onPath;
    // macOS에서 Finder로 연 앱은 셸의 PATH를 모른다: 설치 프로그램이 두는 곳을 직접 본다
    const QStringList folders{QDir::home().filePath(QStringLiteral(".local/bin")),
                              QDir::home().filePath(QStringLiteral(".claude/local")),
                              QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")};
    return QStandardPaths::findExecutable(QStringLiteral("claude"), folders);
}

bool isDbaroRegistered()
{
    // 사용자 범위(--scope user)로 등록한 서버는 ~/.claude.json의 mcpServers에 있다
    QFile file(QDir::home().filePath(QStringLiteral(".claude.json")));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    return root.value(QStringLiteral("mcpServers")).toObject().contains(QStringLiteral("dbaro"));
}

QString workingFolder(const Context &context)
{
    const QString scripts = ScriptFolders::folder(context.profile.type);
    if (!scripts.isEmpty() && QFileInfo(scripts).isDir())
        return scripts;
    // 문서 폴더는 OneDrive와 동기화되는 경우가 많아 쓰지 않는다 (DB에서 읽은 SQL·데이터가 클라우드로 올라가지 않게)
    const QString folder = QDir::home().filePath(QStringLiteral("DBaro"));
    QDir().mkpath(folder);
    return folder;
}

QString contextPrompt(const Context &context, const QString &folder)
{
    const ConnectionProfile &p = context.profile;
    QStringList lines;
    QString connection = QStringLiteral("This session was opened from DBaro (a SQL IDE) for the database connection '%1' (%2")
                             .arg(p.name, Db::typeName(p.type));
    if (!context.serverInfo.isEmpty())
        connection += QStringLiteral(", %1").arg(context.serverInfo.section(u'\n', 0, 0));
    connection += QStringLiteral(").");
    if (!context.schema.isEmpty())
        connection += QStringLiteral(" The current schema of the user's DBaro query tab is %1.").arg(context.schema);
    lines << connection;
    if (!context.database.isEmpty())
        lines << QStringLiteral("The DBaro tab uses the database %1, but the dbaro tools connect to the connection's default "
                                "database (%2); qualify names or tell the user when that matters.")
                     .arg(context.database, p.database);
    if (p.environment == Environment::Production)
        lines << QStringLiteral("This is a PRODUCTION database. Be careful: never suggest running changes without the user's "
                                "review, and keep queries light.");
    lines << QStringLiteral("- Use the dbaro MCP tools with connection '%1' unless the user names another connection "
                            "(list_connections shows them).")
                 .arg(p.name);
    lines << QStringLiteral("- Look before you write SQL: list_tables, describe_table and get_ddl give the real names, keys and "
                            "comments (comments are often in Korean). run_query and explain_query are read-only.");
    lines << QStringLiteral("- For SQL that changes data or structure, or a finished query the user should keep, call open_in_dbaro: "
                            "it opens in a new DBaro query tab, where the user reviews and runs it.");
    const QString scripts = ScriptFolders::folder(p.type);
    if (!scripts.isEmpty() && QDir(scripts) == QDir(folder))
        lines << QStringLiteral("- The working folder (%1) is the user's SQL script folder for this kind of database; read or edit "
                                ".sql files there when the user asks.")
                     .arg(QDir::toNativeSeparators(folder));
    else
        lines << QStringLiteral("- The working folder (%1) is DBaro's default folder; save .sql files there when the user asks.")
                     .arg(QDir::toNativeSeparators(folder));
    lines << QStringLiteral("- Write SQL for %1 and answer in the language the user writes in.").arg(Db::typeName(p.type));
    return lines.join(u'\n');
}

QStringList arguments(const Context &context, const QString &folder, const QString &mcpConfigPath, const QString &prompt)
{
    QStringList arguments{QStringLiteral("--append-system-prompt"), contextPrompt(context, folder)};
    if (!mcpConfigPath.isEmpty())
        arguments << QStringLiteral("--mcp-config") << mcpConfigPath;
    // 첫 질문 (claude "질문": 대화형으로 열고 바로 보낸다). -로 시작하면 옵션으로 읽히므로 앞에 글을 붙인다
    const QString question = prompt.trimmed();
    if (!question.isEmpty())
        arguments << (question.startsWith(u'-') ? QStringLiteral("Question: ") + question : question);
    return arguments;
}

namespace {

// 등록하지 않았을 때 이 세션에만 붙일 dbaro MCP 서버 (Use with Claude Code 창에서 고른 옵션으로)
QString writeSessionMcpConfig(QString *error)
{
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(folder);
    const QString path = QDir(folder).filePath(QStringLiteral("claude-mcp.json"));
    const QJsonObject server{{QStringLiteral("command"), ClaudeCodeDialog::cliPath()},
                             {QStringLiteral("args"), QJsonArray::fromStringList(ClaudeCodeDialog::savedServerArguments())}};
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(QJsonObject{{QStringLiteral("mcpServers"), QJsonObject{{QStringLiteral("dbaro"), server}}}})
                          .toJson(QJsonDocument::Indented))
               < 0) {
        *error = file.errorString();
        return {};
    }
    return path;
}

// DBaro를 Claude Code 세션(터미널 도구, 데스크톱 앱)에서 띄웠으면 그 세션의 환경 변수(세션 id, 메시지 토큰, 로컬 프록시
// 주소 등)를 물려받는다. 새 Claude Code가 그것을 보면 하위 세션처럼 굴어 기록이 남지 않으므로 뺀다.
// 사용자가 시스템·사용자 환경 변수로 직접 정한 것(예: CLAUDE_CODE_USE_BEDROCK)은 그대로 둔다
#ifdef Q_OS_WIN
QProcessEnvironment launchEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QSet<QString> persistent;
    for (const QString &key : {QStringLiteral("HKEY_CURRENT_USER\\Environment"),
                               QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment")}) {
        const QSettings registry(key, QSettings::NativeFormat);
        for (const QString &name : registry.childKeys())
            persistent.insert(name.toUpper());
    }
    static const QStringList sessionPrefixes{
        QStringLiteral("CLAUDECODE"),      QStringLiteral("CLAUDE_CODE_"),     QStringLiteral("CLAUDE_AGENT_SDK_"),
        QStringLiteral("CLAUDE_PID"),      QStringLiteral("CLAUDE_PREVIEW_"),  QStringLiteral("ANTHROPIC_BASE_URL"),
        QStringLiteral("MCP_CONNECTION_"), QStringLiteral("MCP_SERVER_CONNECTION_"),
    };
    for (const QString &name : environment.keys()) {
        const QString upper = name.toUpper();
        if (persistent.contains(upper))
            continue;
        for (const QString &prefix : sessionPrefixes) {
            if (upper.startsWith(prefix)) {
                environment.remove(name);
                break;
            }
        }
    }
    return environment;
}
#endif

#ifndef Q_OS_WIN
QString shellQuoted(const QString &text)
{
    QString quoted = text;
    quoted.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
    return QStringLiteral("'") + quoted + QStringLiteral("'");
}

// macOS·Linux: 실행할 셸 스크립트를 만들어 터미널 앱으로 연다 (터미널이 새 로그인 셸에서 돌리므로 사용자의 환경을 쓴다)
bool startInTerminalApp(const QString &program, const QStringList &arguments, const QString &folder, QString *error)
{
    const QString dataFolder = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataFolder);
    const QString path = QDir(dataFolder).filePath(QStringLiteral("open-claude-code.command"));
    QStringList command{shellQuoted(program)};
    for (const QString &argument : arguments)
        command << shellQuoted(argument);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = file.errorString();
        return false;
    }
    file.write(QStringLiteral("#!/bin/bash\ncd %1 || exit 1\nclear\nexec %2\n").arg(shellQuoted(folder), command.join(u' ')).toUtf8());
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#ifdef Q_OS_MACOS
    if (!QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-a"), QStringLiteral("Terminal"), path})) {
        *error = QObject::tr("Could not open Terminal.");
        return false;
    }
    return true;
#else
    for (const QString &terminal : {QStringLiteral("x-terminal-emulator"), QStringLiteral("gnome-terminal"), QStringLiteral("konsole")}) {
        const QString found = QStandardPaths::findExecutable(terminal);
        if (found.isEmpty())
            continue;
        const QStringList args = terminal == u"gnome-terminal" ? QStringList{QStringLiteral("--"), path}
                                                                : QStringList{QStringLiteral("-e"), path};
        if (QProcess::startDetached(found, args))
            return true;
    }
    *error = QObject::tr("No terminal program was found. Open a terminal in %1 and run claude.").arg(folder);
    return false;
#endif
}
#endif

bool startInNewTerminal(const QString &program, const QStringList &arguments, const QString &folder, QString *error)
{
#ifndef Q_OS_WIN
    return startInTerminalApp(program, arguments, folder, error);
#else
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(folder);
    process.setProcessEnvironment(launchEnvironment());
    // GUI 앱이 콘솔 프로그램을 띄우면 창이 없다 (CREATE_NO_WINDOW). 새 콘솔을 만들어 보이게 한다
    // Qt는 부모의 표준 핸들을 넘긴다(STARTF_USESTDHANDLES). GUI 앱은 핸들이 없어 claude가 터미널을 못 찾고 바로 끝나므로
    // 넘기지 않고 새 콘솔의 것을 쓰게 한다
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags &= ~DWORD(CREATE_NO_WINDOW | DETACHED_PROCESS);
        args->flags |= CREATE_NEW_CONSOLE;
        args->inheritHandles = false;
        args->startupInfo->dwFlags &= ~DWORD(STARTF_USESTDHANDLES);
        args->startupInfo->hStdInput = nullptr;
        args->startupInfo->hStdOutput = nullptr;
        args->startupInfo->hStdError = nullptr;
    });
    if (!process.startDetached()) {
        *error = process.errorString();
        return false;
    }
    return true;
#endif
}

} // namespace

bool launch(QWidget *parent, const Context &context, QString *note, const QString &prompt)
{
    const QString claude = claudePath();
    if (claude.isEmpty()) {
        QMessageBox::information(parent, QObject::tr("Open Claude Code"),
                                 QObject::tr("Claude Code is not installed: the claude command was not found.\n\n"
                                             "Install it (https://claude.com/claude-code), sign in once in a terminal, "
                                             "then try again."));
        return false;
    }
    QString error;
    QString mcpConfig;
    if (!isDbaroRegistered()) {
        mcpConfig = writeSessionMcpConfig(&error);
        if (mcpConfig.isEmpty()) {
            QMessageBox::warning(parent, QObject::tr("Open Claude Code"), QObject::tr("Could not write the MCP settings: %1").arg(error));
            return false;
        }
        if (note)
            *note = QObject::tr("The dbaro tools were added for this session only. Database → Use with Claude Code... adds them "
                                "to every Claude Code session.");
    }
    const QString folder = workingFolder(context);
    QString program = claude;
    QString question = prompt.trimmed();
#ifdef Q_OS_WIN
    // Windows 명령줄은 32K자까지라 긴 질문(큰 스크립트)은 클립보드로 넘긴다
    if (question.size() > kMaxPromptChars) {
        QGuiApplication::clipboard()->setText(question);
        question.clear();
        if (note)
            *note = QObject::tr("The question was too long to pass, so it was copied: paste it into Claude Code (Ctrl+V).");
    }
#endif
    QStringList args = arguments(context, folder, mcpConfig, question);
#ifdef Q_OS_WIN
    // npm으로 설치했으면 claude.cmd: cmd가 실행해야 한다. cmd는 여러 줄·따옴표 인자를 망가뜨리므로 맥락 글은 넘기지 않는다
    if (claude.endsWith(QLatin1String(".cmd"), Qt::CaseInsensitive) || claude.endsWith(QLatin1String(".bat"), Qt::CaseInsensitive)) {
        args = mcpConfig.isEmpty() ? QStringList() : QStringList{QStringLiteral("--mcp-config"), mcpConfig};
        args.prepend(claude);
        args.prepend(QStringLiteral("/c"));
        program = QStringLiteral("cmd.exe");
        if (!question.isEmpty())
            QGuiApplication::clipboard()->setText(question);
        if (note)
            *note = question.isEmpty()
                        ? QObject::tr("Claude Code was installed with npm, so DBaro could not pass the current connection; "
                                      "tell Claude which connection to use.")
                        : QObject::tr("Claude Code was installed with npm, so the question was copied: paste it into Claude Code "
                                      "(Ctrl+V) and say which connection to use.");
    }
#endif
    if (!startInNewTerminal(program, args, folder, &error)) {
        QMessageBox::warning(parent, QObject::tr("Open Claude Code"),
                             QObject::tr("Could not start %1: %2").arg(QDir::toNativeSeparators(claude), error));
        return false;
    }
    return true;
}

} // namespace ClaudeCodeLauncher
