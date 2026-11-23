#pragma once

#include "core/ConnectSpec.h"
#include "core/SqlSplitter.h"
#include "ipc/EngineClient.h"

#include <QCborArray>
#include <QObject>
#include <QPointer>

#include <functional>

// 마지막으로 실행한 문장의 결과 요약 (결과 영역 오른쪽 위에 보인다).
struct ExecutionInfo
{
    enum class Kind { None, ResultSet, Update, Other, Plan };

    Kind kind = Kind::None;
    qint64 rows = 0;       // ResultSet: 지금까지 받은 행 수, Update: 영향 받은 행 수
    bool hasMore = false;  // 아직 가져오지 않은 행이 남아 있음
    qint64 executeMs = 0;  // 문장 실행 시간
    qint64 fetchMs = 0;    // 행을 읽어 온 시간 (스크롤로 더 가져온 시간 포함)

    qint64 totalMs() const { return executeMs + fetchMs; }
};

// 쿼리 탭 하나의 DB 세션. 엔진의 세션 하나와 짝을 이룬다 (탭마다 별도 연결).
// 여러 문장은 차례로 실행하고, 오류가 나면 거기서 멈춘다.
class QuerySession : public QObject
{
    Q_OBJECT

public:
    explicit QuerySession(EngineClient *engine, QObject *parent = nullptr);
    ~QuerySession() override;

    bool isConnected() const { return !m_sessionId.isEmpty(); }
    bool isConnecting() const { return m_connecting; }
    bool isBusy() const { return m_busy; }
    bool autoCommit() const { return m_autoCommit; }
    bool hasPendingTransaction() const { return m_pendingTransaction; }
    QString profileId() const { return m_profileId; }
    QString serverInfo() const { return m_serverInfo; }
    // 이 세션의 지금 스키마 (MySQL·MariaDB는 데이터베이스). 모르면 빈 값
    QString currentSchema() const { return m_currentSchema; }

    void connectTo(const ConnectSpec &spec, const QString &profileId, std::function<void(bool)> done = {});
    // 접속 중이면 접속이 끝난 뒤 callback(성공 여부)을 부른다. 이미 접속했으면 바로 true, 접속 중이 아니면 바로 false.
    void whenConnected(std::function<void(bool)> callback);
    // 기본 스키마를 바꾼다: Oracle·Tibero ALTER SESSION SET CURRENT_SCHEMA, PostgreSQL search_path, MySQL·MariaDB USE
    void setSchema(const QString &schema, std::function<void(bool ok)> done = {});
    // 서버에 지금 스키마를 다시 묻는다 (스키마를 바꾸는 문장을 직접 실행했거나 롤백한 뒤). done은 알아낸 스키마로 불린다.
    void refreshCurrentSchema(std::function<void(const QString &schema)> done = {});
    void disconnectFromServer();
    void run(const QList<SqlStatement> &statements, int pageSize = 500, int queryTimeoutSec = 0);
    void fetchMore(int count = 500);
    void cancel();
    void commit();
    void rollback();
    void setAutoCommit(bool autoCommit);

    // 예상 실행 계획 (문장은 실행하지 않는다). 결과는 planReceived, 실패는 statementFailed.
    void explain(const SqlStatement &statement);
    // 문장 여러 개를 한 번에 엔진으로 보내 차례로 실행한다 (데이터 불러오기, 결과 편집 반영).
    // 결과: executed, failed, skipped, affected, stoppedAt, cancelled, errors[{index, message}], elapsedMs
    void executeBatch(const QStringList &statements, bool stopOnError,
                      std::function<void(const QCborMap &result, const EngineError &error)> done);

    ExecutionInfo executionInfo() const { return m_info; }

signals:
    void stateChanged(); // 연결·실행·트랜잭션 상태가 바뀜
    void message(const QString &line, bool isError);
    void serverOutput(const QStringList &lines);
    void resultStarted(const QCborArray &columns);
    void rowsReceived(const QCborArray &rows, bool hasMore);
    void statementFailed(const SqlStatement &statement, const EngineError &error);
    void statementSucceeded(const SqlStatement &statement);
    // 실행한 문장 하나가 끝남 (실행 기록용). rows는 가져온 행 수나 영향 받은 행 수, 모르면 -1
    void statementFinished(const SqlStatement &statement, bool ok, qint64 elapsedMs, qint64 rows, const QString &error);
    void executionInfoChanged(const ExecutionInfo &info);
    void planReceived(const QCborMap &plan);
    void schemaChanged(const QString &schema); // 지금 스키마가 바뀜 (끊기면 빈 값)

private:
    void runNext();
    void onExecuted(const SqlStatement &statement, const QCborMap &result, const EngineError &error);
    void finishRun();
    void closeCursor();
    void markDisconnected(const QString &reason);
    void setBusy(bool busy);
    void setPendingTransaction(bool pending);
    void setCurrentSchemaValue(const QString &schema);
    QString progress() const;

    QPointer<EngineClient> m_engine;
    QString m_sessionId;
    QString m_profileId;
    QString m_serverInfo;
    QString m_currentSchema;
    bool m_connecting = false;
    QList<std::function<void(bool)>> m_connectWaiters;
    bool m_busy = false;
    bool m_autoCommit = false;
    bool m_pendingTransaction = false;

    QList<SqlStatement> m_queue;
    int m_total = 0;
    int m_done = 0;
    int m_pageSize = 500;
    int m_queryTimeoutSec = 0;
    QString m_cursorId;
    bool m_fetching = false;
    ExecutionInfo m_info;
};
