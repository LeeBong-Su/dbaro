#pragma once

#include "ai/AiAgent.h"
#include "core/ConnectSpec.h"
#include "core/ConnectionProfile.h"
#include "core/SqlDialect.h"

#include <QCborMap>
#include <QJsonArray>
#include <QObject>
#include <QPointer>

#include <functional>

class EngineClient;
class MetadataService;
class QuerySession;
class QWidget;

// AI 도우미가 쓰는 DB 도구. 모두 DBaro가 이 PC에서 실행하고 결과 글만 모델에 돌려준다.
// GUI의 AI 도우미 창과 MCP 서버(dbaro-cli mcp, 사용자의 Claude Code·Claude Desktop이 부른다)가 함께 쓴다.
// - list_schemas, list_tables, describe_table, get_ddl: 메타데이터 세션 (탐색기·자동완성과 같은 캐시)
// - run_query: 문장 하나, 읽기 전용(SELECT·WITH·SHOW·DESCRIBE·EXPLAIN)만. 자기 세션(읽기 전용 연결, 자동 커밋)에서
//   최대 maxRows행. 운영 DB이거나 설정에서 켜면 실행 전에 사용자에게 묻는다. 결과 값을 보내지 않게 설정할 수 있다
// - explain_query: 예상 실행 계획 (문장은 실행하지 않는다)
// - propose_sql: 실행하지 않고 사용자에게 보여 준다 (쿼리 탭에서 열기). 데이터·구조를 바꾸는 SQL은 이것으로만
class AiDbTools : public QObject
{
    Q_OBJECT

public:
    struct Context
    {
        MetadataService *metadata = nullptr;
        EngineClient *engine = nullptr;
        std::function<QString()> metadataId;    // 지금 보고 있는 DB의 메타데이터 id
        std::function<QString()> currentSchema; // 지금 쿼리 탭의 스키마
        std::function<bool(ConnectSpec *spec)> connectSpec; // 비밀번호를 물어 접속 정보를 만든다
        std::function<ConnectionProfile()> profile;
        QWidget *dialogParent = nullptr;

        // run_query의 행 수와 결과 값 보내기. 없으면 AI 도우미 설정(AiSettings)
        struct QueryPolicy
        {
            int maxRows = 100;
            bool sendRows = true;
        };
        std::function<QueryPolicy()> policy;
        // run_query 실행 전 확인. false면 실행하지 않고 reason을 돌려준다.
        // 없으면 설정(조회마다 묻기)·운영 DB에 따라 확인 창을 띄운다 (GUI). MCP 서버처럼 창이 없으면 꼭 정한다
        std::function<bool(const QString &sql, QString *reason)> approve;
    };

    explicit AiDbTools(const Context &context, QObject *parent = nullptr);
    ~AiDbTools() override;

    static QJsonArray definitions();
    void run(const QString &name, const QJsonObject &input, AiAgent::ToolDone done);
    void disconnectSession();
    QuerySession *session() const { return m_session; }

    // 읽기 전용으로 실행해도 되는 문장인지. 안 되면 이유를 돌려준다
    static QString readOnlyProblem(const QString &sql, SqlDialect dialect);
    // 실행 계획을 볼 수 있는 문장인지 (문장 하나, SELECT·DML). 안 되면 이유
    static QString explainProblem(const QString &sql, SqlDialect dialect);

signals:
    void sqlProposed(const QString &sql, const QString &title, const QString &explanation);

private:
    void metadataCall(const QString &method, const QCborMap &params, bool refresh,
                      std::function<void(const QCborMap &result, const QString &error)> done);
    void withSession(std::function<void()> work, std::function<void(const QString &)> fail);
    void withSchemaSession(std::function<void()> work, std::function<void(const QString &)> fail); // 지금 스키마로 맞춘 뒤
    void runQuery(const QJsonObject &input, AiAgent::ToolDone done);
    void explain(const QJsonObject &input, AiAgent::ToolDone done);
    QString schemaOr(const QJsonObject &input) const;

    Context m_context;
    QuerySession *m_session = nullptr;
    QString m_sessionKey; // 접속한 메타데이터 id (DB가 바뀌면 다시 접속)
};
