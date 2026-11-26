#include "QuerySession.h"

#include "core/SqlSafety.h"

#include <utility>

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

// 실행 로그에 남길 문장: "[1/2] SQL> SELECT ..." 이어지는 줄은 첫 줄의 SQL 위치에 맞춘다.
// 아주 긴 문장(큰 INSERT 스크립트 등)은 앞부분만 남긴다.
QString statementForLog(const QString &head, const QString &sql)
{
    constexpr int maxLines = 200;
    constexpr int maxChars = 20000;
    QString text = sql.trimmed();
    text.remove(u'\r');
    QStringList lines = text.split(u'\n');
    const qsizetype total = lines.size();
    if (total > maxLines)
        lines.resize(maxLines);
    for (QString &line : lines) {
        while (!line.isEmpty() && line.back().isSpace())
            line.chop(1);
    }
    QString body = lines.join(u'\n' + QString(head.size(), u' '));
    QString note;
    if (body.size() > maxChars) {
        body.truncate(maxChars);
        note = QuerySession::tr("(statement shortened: %n characters shown)", nullptr, maxChars);
    } else if (total > maxLines) {
        note = QuerySession::tr("(%n more line(s) not shown)", nullptr, int(total - maxLines));
    }
    if (!note.isEmpty())
        body += u'\n' + QString(head.size(), u' ') + note;
    return head + body;
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
    const QString requestedSchema = spec.schema;
    m_engine->request(QStringLiteral("connect"), spec.toCbor(),
                      [self, profileId, done, requestedSchema](const QCborMap &result, const EngineError &error) {
                          if (!self)
                              return;
                          self->m_connecting = false;
                          const QList<std::function<void(bool)>> waiters = std::exchange(self->m_connectWaiters, {});
                          if (error.isError()) {
                              emit self->message(tr("Connection failed: %1").arg(error.message), true);
                              emit self->stateChanged();
                              if (done)
                                  done(false);
                              for (const auto &waiter : waiters)
                                  waiter(false);
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
                          const QString schemaError = result.value(QStringLiteral("schemaError")).toString();
                          if (!schemaError.isEmpty())
                              emit self->message(tr("Could not switch to schema %1: %2").arg(requestedSchema, schemaError), true);
                          self->setCurrentSchemaValue(result.value(QStringLiteral("schema")).toString());
                          emit self->stateChanged();
                          if (done)
                              done(true);
                          for (const auto &waiter : waiters) {
                              if (!self)
                                  return;
                              waiter(self->isConnected());
                          }
                      });
}

void QuerySession::setSchema(const QString &schema, std::function<void(bool)> done)
{
    if (!isConnected() || m_busy || !m_engine || schema.isEmpty()) {
        if (done)
            done(false);
        return;
    }
    setActivity(tr("Switching the schema to %1").arg(schema));
    setBusy(true);
    QPointer<QuerySession> self(this);
    m_engine->request(QStringLiteral("setSchema"),
                      params({{QStringLiteral("sessionId"), m_sessionId}, {QStringLiteral("schema"), schema}}),
                      [self, schema, done](const QCborMap &result, const EngineError &error) {
                          if (!self)
                              return;
                          self->setBusy(false);
                          if (error.isError()) {
                              emit self->message(tr("Could not switch to schema %1: %2").arg(schema, error.message), true);
                              if (done)
                                  done(false);
                              return;
                          }
                          // 실행한 문장도 로그에 남긴다 (ALTER SESSION SET CURRENT_SCHEMA = ...)
                          const QString sql = result.value(QStringLiteral("sql")).toString();
                          const QString current = result.value(QStringLiteral("schema")).toString();
                          emit self->message(sql.isEmpty() ? tr("Current schema: %1").arg(current)
                                                           : tr("%1\nCurrent schema: %2").arg(sql, current),
                                             false);
                          self->setCurrentSchemaValue(current);
                          if (done)
                              done(true);
                      });
}

void QuerySession::refreshCurrentSchema(std::function<void(const QString &)> done)
{
    if (!isConnected() || !m_engine)
        return;
    QPointer<QuerySession> self(this);
    m_engine->request(QStringLiteral("currentSchema"), params({{QStringLiteral("sessionId"), m_sessionId}}),
                      [self, done](const QCborMap &result, const EngineError &error) {
                          if (!self || error.isError() || !self->isConnected())
                              return;
                          const QString current = result.value(QStringLiteral("schema")).toString();
                          self->setCurrentSchemaValue(current);
                          if (done)
                              done(current);
                      });
}

void QuerySession::setCurrentSchemaValue(const QString &schema)
{
    if (m_currentSchema == schema)
        return;
    m_currentSchema = schema;
    emit schemaChanged(schema);
}

void QuerySession::whenConnected(std::function<void(bool)> callback)
{
    if (m_connecting)
        m_connectWaiters.append(std::move(callback));
    else
        callback(isConnected());
}

void QuerySession::call(const QString &method, QCborMap params,
                        std::function<void(const QCborMap &, const EngineError &)> done)
{
    if (!isConnected() || !m_engine) {
        if (done)
            done({}, EngineError::of(tr("Not connected.")));
        return;
    }
    params.insert(QStringLiteral("sessionId"), m_sessionId);
    QPointer<QuerySession> self(this);
    m_engine->request(method, params, [self, done](const QCborMap &result, const EngineError &error) {
        if (self && done)
            done(result, error);
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
    setCurrentSchemaValue({});
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
    setActivity(m_total > 1 ? tr("Running statement 1 of %1").arg(m_total) : tr("Running the query"), statements.first().text);
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
    setActivity(m_total > 1 ? tr("Running statement %1 of %2").arg(m_done + 1).arg(m_total) : tr("Running the query"),
                statement.text);
    // 실행하는 문장을 먼저 로그에 남긴다 (오래 걸리는 문장도 무엇이 실행 중인지 보인다).
    emit message(statementForLog(QStringLiteral("[%1/%2] SQL> ").arg(m_done + 1).arg(m_total), statement.text), false);
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
        emit statementFinished(statement, false, 0, -1, error.message);
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
    const qint64 executeMs = result.value(QStringLiteral("executeMs")).toInteger(elapsed);
    m_info = {};
    m_info.executeMs = executeMs;
    m_info.fetchMs = qMax<qint64>(0, elapsed - executeMs);
    if (result.value(QStringLiteral("kind")).toString() == QStringLiteral("resultSet")) {
        closeCursor();
        const QCborArray rows = result.value(QStringLiteral("rows")).toArray();
        const bool hasMore = result.value(QStringLiteral("hasMore")).toBool();
        m_cursorId = result.value(QStringLiteral("cursorId")).toString();
        emit resultStarted(result.value(QStringLiteral("columns")).toArray());
        emit rowsReceived(rows, hasMore);
        m_info.kind = ExecutionInfo::Kind::ResultSet;
        m_info.rows = rows.size();
        m_info.hasMore = hasMore;
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
        m_info.kind = dml && count >= 0 ? ExecutionInfo::Kind::Update : ExecutionInfo::Kind::Other;
        m_info.rows = qMax<qint64>(0, count);
    }
    emit executionInfoChanged(m_info);
    emit statementFinished(statement, true, elapsed,
                           m_info.kind == ExecutionInfo::Kind::Other ? -1 : m_info.rows, QString());
    emit statementSucceeded(statement);
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
                          const QCborArray rows = result.value(QStringLiteral("rows")).toArray();
                          emit self->rowsReceived(rows, hasMore);
                          self->m_info.rows += rows.size();
                          self->m_info.hasMore = hasMore;
                          self->m_info.fetchMs += result.value(QStringLiteral("elapsedMs")).toInteger();
                          emit self->executionInfoChanged(self->m_info);
                      });
}

void QuerySession::interrupt()
{
    if (isConnected() && m_engine)
        m_engine->request(QStringLiteral("cancel"), params({{QStringLiteral("sessionId"), m_sessionId}}));
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
                          self->refreshCurrentSchema(); // PostgreSQL은 트랜잭션 안의 SET search_path도 되돌린다
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

void QuerySession::executeBatch(const QStringList &statements, bool stopOnError,
                                std::function<void(const QCborMap &, const EngineError &)> done)
{
    if (!isConnected() || m_busy || !m_engine) {
        done({}, EngineError::of(tr("The tab is not connected or is busy.")));
        return;
    }
    setActivity(tr("Running %n statement(s)", nullptr, int(statements.size())), statements.value(0));
    setBusy(true);
    QCborArray list;
    for (const QString &sql : statements)
        list.append(sql);
    QPointer<QuerySession> self(this);
    m_engine->request(QStringLiteral("executeBatch"),
                      params({{QStringLiteral("sessionId"), m_sessionId},
                              {QStringLiteral("statements"), list},
                              {QStringLiteral("stopOnError"), stopOnError}}),
                      [self, done](const QCborMap &result, const EngineError &error) {
                          if (!self)
                              return;
                          self->setBusy(false);
                          // 수동 커밋이면 커밋하기 전까지 남는다
                          if (!error.isError() && result.value(QStringLiteral("executed")).toInteger() > 0 && !self->m_autoCommit)
                              self->setPendingTransaction(true);
                          done(result, error);
                      });
}

void QuerySession::explain(const SqlStatement &statement, bool actual)
{
    if (!isConnected() || m_busy || !m_engine)
        return;
    setActivity(actual ? tr("Running the statement for its actual plan") : tr("Getting the estimated plan"), statement.text);
    setBusy(true);
    emit message(statementForLog(actual ? QStringLiteral("EXPLAIN ANALYZE> ") : QStringLiteral("EXPLAIN> "), statement.text), false);
    QPointer<QuerySession> self(this);
    QCborMap request = params({{QStringLiteral("sessionId"), m_sessionId}, {QStringLiteral("sql"), statement.text}});
    if (actual)
        request.insert(QStringLiteral("actual"), true);
    m_engine->request(QStringLiteral("explain"), request,
                      [self, statement, actual](const QCborMap &result, const EngineError &error) {
                          if (!self)
                              return;
                          self->setBusy(false);
                          if (error.isError()) {
                              emit self->message(tr("Explain failed: %1").arg(error.message), true);
                              // 오류 위치는 엔진이 앞에 붙인 EXPLAIN 구문 기준이라 에디터 위치와 맞지 않는다.
                              EngineError withoutPosition = error;
                              withoutPosition.position = -1;
                              withoutPosition.line = -1;
                              emit self->statementFailed(statement, withoutPosition);
                              return;
                          }
                          const qint64 elapsed = result.value(QStringLiteral("elapsedMs")).toInteger();
                          if (!actual)
                              emit self->message(tr("Estimated plan in %1 ms").arg(elapsed), false);
                          else if (result.value(QStringLiteral("rolledBack")).toBool())
                              emit self->message(tr("Actual plan in %1 ms. The statement ran and its changes were rolled back.").arg(elapsed), false);
                          else
                              emit self->message(tr("Actual plan in %1 ms. The statement ran.").arg(elapsed), false);
                          emit self->planReceived(result);
                          ExecutionInfo info;
                          info.kind = ExecutionInfo::Kind::Plan;
                          info.actualPlan = actual;
                          info.executeMs = elapsed;
                          emit self->executionInfoChanged(info);
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
    setCurrentSchemaValue({});
    emit message(reason, true);
    emit stateChanged();
}

void QuerySession::setActivity(const QString &title, const QString &detail)
{
    if (m_activityTitle == title && m_activityDetail == detail)
        return;
    m_activityTitle = title;
    m_activityDetail = detail;
    emit activityChanged(title, detail);
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
