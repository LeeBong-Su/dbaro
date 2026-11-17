#include "QuerySession.h"

#include "core/SqlSafety.h"

namespace {

QStringList toStringList(const QCborValue &value)
{
    QStringList lines;
    for (const QCborValue &item : value.toArray())
        lines << item.toString();
    return lines;
}

QCborMap params(std::initializer_list<std::pair<QString, QCborValue>> items)
{
    QCborMap map;
    for (const auto &[key, value] : items)
        map.insert(key, value);
    return map;
}

} // namespace

QuerySession::QuerySession(EngineClient *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
    connect(engine, &EngineClient::engineStopped, this, [this](const QString &reason) {
        if (isConnected() || m_connecting)
            markDisconnected(reason);
    });
}

QuerySession::~QuerySession()
{
    disconnectFromServer();
}

void QuerySession::connectTo(const ConnectSpec &spec, const QString &profileId, std::function<void(bool)> done)
{
    if (!m_engine)
        return;
    if (isConnected())
        disconnectFromServer();

    m_connecting = true;
    m_autoCommit = spec.autoCommit;
    emit stateChanged();
    emit message(tr("Connecting to %1 ...").arg(spec.url), false);

    QPointer<QuerySession> self(this);
    m_engine->request(QStringLiteral("connect"), spec.toCbor(),
                      [self, profileId, done](const QCborMap &result, const EngineError &error) {
                          if (!self)
                              return;
                          self->m_connecting = false;
                          if (error.isError()) {
                              emit self->message(tr("Connection failed: %1").arg(error.message), true);
                              emit self->stateChanged();
                              if (done)
                                  done(false);
                              return;
                          }
                          self->m_sessionId = result.value(QStringLiteral("sessionId")).toString();
                          self->m_profileId = profileId;
                          self->m_serverInfo = QStringLiteral("%1 %2").arg(
                              result.value(QStringLiteral("product")).toString(),
                              result.value(QStringLiteral("productVersion")).toString().section(u'\n', 0, 0));
                          emit self->message(tr("Connected: %1 (driver %2)")
                                                 .arg(self->m_serverInfo,
                                                      result.value(QStringLiteral("driverVersion")).toString()),
                                             false);
                          emit self->stateChanged();
                          if (done)
                              done(true);
                      });
}

void QuerySession::disconnectFromServer()
{
    if (!isConnected())
        return;
    closeCursor();
    if (m_engine)
        m_engine->request(QStringLiteral("disconnect"), params({{QStringLiteral("sessionId"), m_sessionId}}));
    m_sessionId.clear();
    m_profileId.clear();
    m_serverInfo.clear();
    m_queue.clear();
    m_busy = false;
    setPendingTransaction(false);
    emit message(tr("Disconnected."), false);
    emit stateChanged();
}

void QuerySession::run(const QList<SqlStatement> &statements, int pageSize, int queryTimeoutSec)
{
    if (!isConnected() || m_busy || statements.isEmpty())
        return;

    m_queue = statements;
    m_total = int(statements.size());
    m_done = 0;
    m_pageSize = pageSize;
    m_queryTimeoutSec = queryTimeoutSec;
    setBusy(true);
    runNext();
}

void QuerySession::runNext()
{
    if (m_queue.isEmpty() || !m_engine || !isConnected()) {
        finishRun();
        return;
    }

    const SqlStatement statement = m_queue.takeFirst();
    QPointer<QuerySession> self(this);
    m_engine->request(QStringLiteral("execute"),
                      params({{QStringLiteral("sessionId"), m_sessionId},
                              {QStringLiteral("sql"), statement.text},
                              {QStringLiteral("pageSize"), m_pageSize},
                              {QStringLiteral("queryTimeoutSec"), m_queryTimeoutSec}}),
                      [self, statement](const QCborMap &result, const EngineError &error) {
                          if (self)
                              self->onExecuted(statement, result, error);
                      });
}

void QuerySession::onExecuted(const SqlStatement &statement, const QCborMap &result, const EngineError &error)
{
    ++m_done;
    if (error.isError()) {
        emit message(tr("%1 Error: %2").arg(progress(), error.message), true);
        emit statementFailed(statement, error);
        m_queue.clear(); // 스크립트는 오류가 난 곳에서 멈춘다
        finishRun();
        return;
    }

    const QStringList output = toStringList(result.value(QStringLiteral("output")))
                               + toStringList(result.value(QStringLiteral("warnings")));
    if (!output.isEmpty())
        emit serverOutput(output);

    const qint64 elapsed = result.value(QStringLiteral("elapsedMs")).toInteger();
    if (result.value(QStringLiteral("kind")).toString() == QStringLiteral("resultSet")) {
        closeCursor();
        const QCborArray rows = result.value(QStringLiteral("rows")).toArray();
        const bool hasMore = result.value(QStringLiteral("hasMore")).toBool();
        m_cursorId = result.value(QStringLiteral("cursorId")).toString();
        emit resultStarted(result.value(QStringLiteral("columns")).toArray());
        emit rowsReceived(rows, hasMore);
        emit message(tr("%1 %n row(s) fetched in %2 ms%3", nullptr, int(rows.size()))
                         .arg(progress())
                         .arg(elapsed)
                         .arg(hasMore ? tr(" (scroll to load more)") : QString()),
                     false);
    } else {
        const qint64 count = result.value(QStringLiteral("updateCount")).toInteger(-1);
        const bool dml = SqlSafety::isDml(statement.text);
        const QString what = dml && count >= 0 ? tr("%n row(s) affected", nullptr, int(count)) : tr("Done");
        emit message(tr("%1 %2 in %3 ms").arg(progress(), what).arg(elapsed), false);
        if (dml && !m_autoCommit)
            setPendingTransaction(true);
    }
    runNext();
}

void QuerySession::finishRun()
{
    m_queue.clear();
    setBusy(false);
}

void QuerySession::fetchMore(int count)
{
    if (m_cursorId.isEmpty() || m_fetching || !m_engine)
        return;
    m_fetching = true;
    QPointer<QuerySession> self(this);
    const QString cursorId = m_cursorId;
    m_engine->request(QStringLiteral("fetch"),
                      params({{QStringLiteral("sessionId"), m_sessionId},
                              {QStringLiteral("cursorId"), cursorId},
                              {QStringLiteral("count"), count}}),
                      [self, cursorId](const QCborMap &result, const EngineError &error) {
                          if (!self || self->m_cursorId != cursorId)
                              return;
                          self->m_fetching = false;
                          if (error.isError()) {
                              self->m_cursorId.clear();
                              emit self->message(tr("Could not load more rows: %1").arg(error.message), true);
                              emit self->rowsReceived({}, false);
                              return;
                          }
                          const bool hasMore = result.value(QStringLiteral("hasMore")).toBool();
                          if (!hasMore)
                              self->m_cursorId.clear();
                          emit self->rowsReceived(result.value(QStringLiteral("rows")).toArray(), hasMore);
                      });
}

void QuerySession::cancel()
{
    if (!m_busy || !m_engine)
        return;
    m_queue.clear();
    m_engine->request(QStringLiteral("cancel"), params({{QStringLiteral("sessionId"), m_sessionId}}));
    emit message(tr("Cancel requested."), false);
}

void QuerySession::commit()
{
    if (!isConnected() || m_busy || !m_engine)
        return;
    QPointer<QuerySession> self(this);
    m_engine->request(QStringLiteral("commit"), params({{QStringLiteral("sessionId"), m_sessionId}}),
                      [self](const QCborMap &, const EngineError &error) {
                          if (!self)
                              return;
                          if (error.isError()) {
                              emit self->message(tr("Commit failed: %1").arg(error.message), true);
                              return;
                          }
                          self->setPendingTransaction(false);
                          emit self->message(tr("Committed."), false);
                      });
}

void QuerySession::rollback()
{
    if (!isConnected() || m_busy || !m_engine)
        return;
    QPointer<QuerySession> self(this);
    m_engine->request(QStringLiteral("rollback"), params({{QStringLiteral("sessionId"), m_sessionId}}),
                      [self](const QCborMap &, const EngineError &error) {
                          if (!self)
                              return;
                          if (error.isError()) {
                              emit self->message(tr("Rollback failed: %1").arg(error.message), true);
                              return;
                          }
                          self->setPendingTransaction(false);
                          emit self->message(tr("Rolled back."), false);
                      });
}

void QuerySession::setAutoCommit(bool autoCommit)
{
    if (m_autoCommit == autoCommit)
        return;
    m_autoCommit = autoCommit;
    if (!isConnected() || !m_engine) {
        emit stateChanged();
        return;
    }
    // JDBC는 자동 커밋으로 바꿀 때 진행 중인 트랜잭션을 커밋한다.
    QPointer<QuerySession> self(this);
    m_engine->request(QStringLiteral("setAutoCommit"),
                      params({{QStringLiteral("sessionId"), m_sessionId}, {QStringLiteral("autoCommit"), autoCommit}}),
                      [self, autoCommit](const QCborMap &, const EngineError &error) {
                          if (!self)
                              return;
                          if (error.isError()) {
                              emit self->message(tr("Could not change the commit mode: %1").arg(error.message), true);
                              return;
                          }
                          if (autoCommit && self->m_pendingTransaction) {
                              self->setPendingTransaction(false);
                              emit self->message(tr("Pending changes were committed (auto-commit on)."), false);
                          }
                          emit self->stateChanged();
                      });
}

void QuerySession::closeCursor()
{
    if (m_cursorId.isEmpty())
        return;
    if (m_engine && isConnected()) {
        m_engine->request(QStringLiteral("closeCursor"),
                          params({{QStringLiteral("sessionId"), m_sessionId}, {QStringLiteral("cursorId"), m_cursorId}}));
    }
    m_cursorId.clear();
    m_fetching = false;
}

void QuerySession::markDisconnected(const QString &reason)
{
    m_sessionId.clear();
    m_profileId.clear();
    m_serverInfo.clear();
    m_cursorId.clear();
    m_queue.clear();
    m_connecting = false;
    m_busy = false;
    m_fetching = false;
    m_pendingTransaction = false;
    emit message(reason, true);
    emit stateChanged();
}

void QuerySession::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit stateChanged();
}

void QuerySession::setPendingTransaction(bool pending)
{
    if (m_pendingTransaction == pending)
        return;
    m_pendingTransaction = pending;
    emit stateChanged();
}

QString QuerySession::progress() const
{
    return QStringLiteral("[%1/%2]").arg(m_done).arg(m_total);
}
