#pragma once

#include "core/ConnectionProfile.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>

class AiDbTools;
class ConnectionStore;
class EngineClient;
class MetadataService;

// dbaro-cli mcp: DBaro의 DB 도구를 MCP(Model Context Protocol) 서버로 낸다 (표준 입출력, 줄마다 JSON-RPC 2.0).
// 사용자가 자기 Claude Code·Claude Desktop에 등록해 자기 로그인으로 쓴다. DBaro는 Claude를 부르지 않는다.
//
//   claude mcp add --transport stdio --scope user dbaro -- "C:\...\dbaro-cli.exe" mcp
//
// - 도구: list_connections, list_schemas, list_tables, describe_table, get_ddl, run_query, explain_query, open_in_dbaro.
//   GUI의 AI 도우미와 같은 코드(AiDbTools)라 읽기 전용 검사·행 수 제한·결과 형식이 같다
// - 연결은 DBaro에 저장한 것 (연결 목록 파일, 자격 증명 관리자의 비밀번호). 비밀번호를 저장하지 않은 연결은 못 쓴다
//   (표준 입력이 프로토콜이라 물을 수 없다. DBARO_PASSWORD가 있으면 그것)
// - 운영 연결은 run_query를 막는다 (--allow-production으로 연다). 구조 읽기와 예상 계획은 된다
// - 데이터를 바꾸는 SQL은 실행하지 않고 open_in_dbaro로 DBaro의 새 쿼리 탭에 넣는다
// - 표준 출력에는 프로토콜 메시지만 쓴다. 안내·오류는 표준 오류로
class McpServer : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        QStringList connections; // 쓸 연결 (이름 또는 id). 비면 저장된 연결 모두
        bool allowProduction = false;
        int maxRows = 100;
        bool sendRows = true;
        QString connectionsFile; // 비면 DBaro의 연결 목록
    };
    using Writer = std::function<void(const QByteArray &line)>;
    // open_in_dbaro: DBaro를 이 인자로 실행한다 (이미 떠 있으면 그 창이 받는다). 테스트 도구가 바꾼다
    using Launcher = std::function<bool(const QStringList &arguments, QString *error)>;

    explicit McpServer(QObject *parent = nullptr);
    ~McpServer() override;

    // 명령줄: dbaro-cli mcp [옵션]. 표준 입력이 닫힐 때까지 돈다
    int run(const QStringList &arguments);
    static QString usage();

    // 옵션과 연결을 읽는다 (run이 부른다. 테스트는 직접). 안 되면 false와 error
    bool setUp(const Options &options, QString *error);
    // 받은 줄 하나 처리. 답은 writer로 (알림이면 답이 없다)
    void handleLine(const QByteArray &line);
    void setWriter(Writer writer) { m_writer = std::move(writer); }
    void setLauncher(Launcher launcher) { m_launcher = std::move(launcher); }

    QJsonArray toolDefinitions() const;
    QString instructions() const;

private:
    struct Call
    {
        QJsonValue id;
        QString name;
        QJsonObject arguments;
    };
    using Done = std::function<void(const QString &text, bool isError)>;

    void reply(const QJsonValue &id, const QJsonObject &result);
    void replyError(const QJsonValue &id, int code, const QString &message);
    void startNext();
    void runTool(const Call &call, Done done);
    void listConnections(Done done);
    void openInDbaro(const ConnectionProfile &profile, const QJsonObject &arguments, Done done);
    const ConnectionProfile *findProfile(const QString &key, QString *error) const;
    bool passwordFor(const ConnectionProfile &profile, QString *password, QString *error) const;
    AiDbTools *toolsFor(const ConnectionProfile &profile);
    void withDefaultSchema(const ConnectionProfile &profile, std::function<void()> next);
    bool launchDbaro(const QStringList &arguments, QString *error) const;

    Options m_options;
    std::unique_ptr<ConnectionStore> m_store;
    QList<ConnectionProfile> m_profiles;
    EngineClient *m_engine = nullptr;
    MetadataService *m_metadata = nullptr;
    QHash<QString, AiDbTools *> m_tools;          // 연결 id → 도구 (자기 읽기 전용 세션)
    QHash<QString, QString> m_defaultSchemas;      // 연결 id → 접속했을 때의 스키마
    QString m_callSchema;                          // 지금 도구 호출의 schema 인자
    QList<Call> m_queue;                           // 도구 호출은 하나씩
    bool m_busy = false;
    Writer m_writer;
    Launcher m_launcher;
};
