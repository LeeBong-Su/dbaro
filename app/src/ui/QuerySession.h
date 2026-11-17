#pragma once

#include "core/ConnectSpec.h"
#include "core/SqlSplitter.h"
#include "ipc/EngineClient.h"

#include <QCborArray>
#include <QObject>
#include <QPointer>

#include <functional>

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

    void connectTo(const ConnectSpec &spec, const QString &profileId, std::function<void(bool)> done = {});
    void disconnectFromServer();
    void run(const QList<SqlStatement> &statements, int pageSize = 500, int queryTimeoutSec = 0);
    void fetchMore(int count = 500);
    void cancel();
    void commit();
    void rollback();
    void setAutoCommit(bool autoCommit);

signals:
    void stateChanged(); // 연결·실행·트랜잭션 상태가 바뀜
    void message(const QString &line, bool isError);
    void serverOutput(const QStringList &lines);
    void resultStarted(const QCborArray &columns);
    void rowsReceived(const QCborArray &rows, bool hasMore);
    void statementFailed(const SqlStatement &statement, const EngineError &error);

private:
    void runNext();
    void onExecuted(const SqlStatement &statement, const QCborMap &result, const EngineError &error);
    void finishRun();
    void closeCursor();
    void markDisconnected(const QString &reason);
    void setBusy(bool busy);
    void setPendingTransaction(bool pending);
    QString progress() const;

    QPointer<EngineClient> m_engine;
    QString m_sessionId;
    QString m_profileId;
    QString m_serverInfo;
    bool m_connecting = false;
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
};
