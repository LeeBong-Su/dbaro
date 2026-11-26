#include "AiAgent.h"

#include <QJsonDocument>
#include <QPointer>
#include <QSet>

namespace {

QJsonObject toolResult(const QString &id, const QString &text, bool isError)
{
    QJsonObject result{{QStringLiteral("type"), QStringLiteral("tool_result")},
                       {QStringLiteral("tool_use_id"), id},
                       {QStringLiteral("content"), text.isEmpty() ? QStringLiteral("(empty)") : text}};
    if (isError)
        result.insert(QStringLiteral("is_error"), true);
    return result;
}

bool hasQuestion(const QJsonObject &message)
{
    if (message.value(QStringLiteral("role")).toString() != u"user")
        return false;
    for (const QJsonValue &block : message.value(QStringLiteral("content")).toArray()) {
        if (block.toObject().value(QStringLiteral("type")).toString() == u"text")
            return true;
    }
    return false;
}

} // namespace

AiAgent::AiAgent(QObject *parent)
    : QObject(parent)
{
    m_client = new ClaudeClient(this);
    connect(m_client, &ClaudeClient::blockStarted, this, [this](int, const QString &type, const QJsonObject &block) {
        emit blockStarted(type);
        if (type == u"fallback") {
            emit fallbackUsed(block.value(QStringLiteral("from")).toObject().value(QStringLiteral("model")).toString(),
                              block.value(QStringLiteral("to")).toObject().value(QStringLiteral("model")).toString());
        } else if (type == u"tool_use") {
            emit status(tr("Preparing %1...").arg(block.value(QStringLiteral("name")).toString()));
        } else if (type == u"thinking") {
            emit status(tr("Thinking..."));
        } else if (type == u"text") {
            emit status(tr("Writing..."));
        }
    });
    connect(m_client, &ClaudeClient::textDelta, this, [this](int, const QString &text) { emit textDelta(text); });
    connect(m_client, &ClaudeClient::thinkingDelta, this, [this](int, const QString &text) { emit thinkingDelta(text); });
    connect(m_client, &ClaudeClient::finished, this, &AiAgent::onMessage);
    connect(m_client, &ClaudeClient::failed, this, [this](const QString &message, int) {
        if (!m_running)
            return;
        m_running = false;
        emit failed(message);
    });
    connect(m_client, &ClaudeClient::retrying, this, [this](int attempt, int delayMs, const QString &reason) {
        emit status(tr("Retrying in %1 s (%2): %3").arg(qMax(1, delayMs / 1000)).arg(attempt).arg(reason.left(120)));
    });
}

void AiAgent::ask(const QString &text)
{
    if (m_running || text.trimmed().isEmpty())
        return;
    QJsonArray content = m_pendingResults; // 답하지 못한 도구 호출의 결과가 먼저 와야 한다
    m_pendingResults = QJsonArray();
    content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}});
    m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), content}});
    m_rounds = 0;
    m_running = true;
    send();
}

void AiAgent::stop()
{
    if (!m_running)
        return;
    ++m_generation;
    m_client->abort();
    // 실행하던 도구의 결과는 버리고, 답하지 못한 호출은 다음 질문 앞에 오류로 둔다
    QList<QJsonObject> unanswered;
    QSet<QString> answered;
    for (const QJsonValue &result : std::as_const(m_toolResults))
        answered.insert(result.toObject().value(QStringLiteral("tool_use_id")).toString());
    const QJsonObject last = m_messages.isEmpty() ? QJsonObject() : m_messages.last().toObject();
    if (last.value(QStringLiteral("role")).toString() == u"assistant") {
        // 결과를 아직 붙이지 않은 tool_use 전부
        unanswered.clear();
        for (const QJsonValue &block : last.value(QStringLiteral("content")).toArray()) {
            const QJsonObject b = block.toObject();
            if (b.value(QStringLiteral("type")).toString() == u"tool_use")
                unanswered.append(b);
        }
        QJsonArray kept;
        for (const QJsonValue &result : std::as_const(m_toolResults))
            kept.append(result);
        m_pendingResults = kept;
        for (const QJsonObject &block : std::as_const(unanswered)) {
            const QString id = block.value(QStringLiteral("id")).toString();
            if (!answered.contains(id))
                m_pendingResults.append(toolResult(id, tr("Not run: the user stopped the response."), true));
        }
    }
    m_toolQueue.clear();
    m_toolResults = QJsonArray();
    finish(QStringLiteral("stopped"));
}

void AiAgent::reset()
{
    stop();
    m_messages = QJsonArray();
    m_pendingResults = QJsonArray();
}

QJsonObject AiAgent::buildRequest(QStringList *betas) const
{
    const bool official = m_config.isOfficialEndpoint();
    QJsonArray tools;
    for (const QJsonValue &value : m_tools) {
        QJsonObject tool = value.toObject();
        // 스트리밍 + 직접 만든 도구: 입력을 버퍼링 없이 받는다 (게이트웨이는 이 필드를 거부할 수 있어 공식 API에서만)
        if (official)
            tool.insert(QStringLiteral("eager_input_streaming"), true);
        tools.append(tool);
    }
    QJsonObject body{
        {QStringLiteral("model"), m_config.model},
        {QStringLiteral("max_tokens"), m_config.maxTokens},
        {QStringLiteral("system"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), m_system}}}},
        {QStringLiteral("messages"), m_messages},
        {QStringLiteral("thinking"), QJsonObject{{QStringLiteral("type"), QStringLiteral("adaptive")}, {QStringLiteral("display"), QStringLiteral("summarized")}}},
        {QStringLiteral("output_config"), QJsonObject{{QStringLiteral("effort"), m_config.effort}}},
        // 도구 루프는 같은 앞부분을 계속 보내므로 자동 캐시 (마지막 캐시 가능 블록에 붙는다)
        {QStringLiteral("cache_control"), QJsonObject{{QStringLiteral("type"), QStringLiteral("ephemeral")}}},
    };
    if (!tools.isEmpty())
        body.insert(QStringLiteral("tools"), tools);
    // 안전 분류기가 거절하면 서버가 알맞은 다른 모델로 이어서 답한다 (Opus 5.5·Sonnet 5.5, 공식 API)
    if (official && (m_config.model == u"claude-opus-5-5" || m_config.model == u"claude-sonnet-5-5")) {
        body.insert(QStringLiteral("fallbacks"), QStringLiteral("default"));
        betas->append(QStringLiteral("server-side-fallback-2026-07-01"));
    }
    return body;
}

void AiAgent::send()
{
    QStringList betas;
    m_lastRequest = buildRequest(&betas);
    emit status(tr("Waiting for %1...").arg(m_config.model));
    m_client->send(m_config, m_lastRequest, betas);
}

void AiAgent::onMessage(const QJsonObject &message)
{
    if (!m_running)
        return;
    emit usage(message.value(QStringLiteral("usage")).toObject(), message.value(QStringLiteral("model")).toString());
    const QString stopReason = message.value(QStringLiteral("stop_reason")).toString();
    const QJsonArray content = message.value(QStringLiteral("content")).toArray();

    if (stopReason == u"refusal") {
        const QJsonObject details = message.value(QStringLiteral("stop_details")).toObject();
        const QString category = details.value(QStringLiteral("category")).toString();
        dropTurn();
        finish(stopReason, category.isEmpty() ? tr("The model declined to answer.")
                                              : tr("The model declined to answer (%1).").arg(category));
        return;
    }

    // 대체 모델로 넘어갔으면 마지막 fallback 블록 앞의 thinking·tool_use는 돌려보내지 않는다 (블록 자체는 그 자리에)
    int lastFallback = -1;
    for (int i = 0; i < content.size(); ++i) {
        if (content.at(i).toObject().value(QStringLiteral("type")).toString() == u"fallback")
            lastFallback = i;
    }
    QJsonArray echo;
    QList<QJsonObject> toolUses;
    for (int i = 0; i < content.size(); ++i) {
        QJsonObject block = content.at(i).toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        if (i < lastFallback && (type == u"thinking" || type == u"redacted_thinking" || type == u"tool_use"))
            continue;
        if (type == u"text" && block.value(QStringLiteral("text")).toString().isEmpty())
            continue; // 빈 text 블록은 요청에 넣을 수 없다
        if (type == u"tool_use") {
            toolUses.append(block); // _invalidInput은 도구 실행 때 본다
            block.remove(QStringLiteral("_invalidInput"));
        }
        echo.append(block);
    }
    if (!echo.isEmpty())
        m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), echo}});

    if (stopReason == u"tool_use" && !toolUses.isEmpty()) {
        if (m_rounds >= m_maxRounds) {
            leaveUnanswered(toolUses, tr("Not run: the assistant used too many tools in one answer."));
            finish(QStringLiteral("max_rounds"), tr("Stopped after %n tool round(s). Ask again to continue.", nullptr, m_maxRounds));
            return;
        }
        ++m_rounds;
        m_toolQueue = toolUses;
        m_toolResults = QJsonArray();
        runNextTool();
        return;
    }
    if (!toolUses.isEmpty())
        leaveUnanswered(toolUses, tr("Not run: the response ended (%1).").arg(stopReason));
    if (stopReason == u"max_tokens")
        finish(stopReason, tr("The answer reached the length limit and was cut off."));
    else
        finish(stopReason);
}

void AiAgent::runNextTool()
{
    if (m_toolQueue.isEmpty()) {
        m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), m_toolResults}});
        m_toolResults = QJsonArray();
        send();
        return;
    }
    const QJsonObject block = m_toolQueue.takeFirst();
    const QString id = block.value(QStringLiteral("id")).toString();
    const QString name = block.value(QStringLiteral("name")).toString();
    const QJsonObject input = block.value(QStringLiteral("input")).toObject();
    emit toolStarted(id, name, input);
    if (block.contains(QStringLiteral("_invalidInput"))) {
        // 입력을 끝까지 받지 못했거나 JSON이 아니다: 실행하지 않고 그대로 알려 다시 쓰게 한다
        const QString text = QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("INVALID_JSON"), block.value(QStringLiteral("_invalidInput")).toString()}})
                                                   .toJson(QJsonDocument::Compact));
        m_toolResults.append(toolResult(id, text, true));
        emit toolFinished(id, name, text, true);
        QMetaObject::invokeMethod(this, &AiAgent::runNextTool, Qt::QueuedConnection);
        return;
    }
    emit status(tr("Running %1...").arg(name));
    const quint64 generation = m_generation;
    QPointer<AiAgent> self(this);
    auto done = [self, generation, id, name](const QString &result, bool isError) {
        if (!self || generation != self->m_generation || !self->m_running)
            return;
        self->m_toolResults.append(toolResult(id, result, isError));
        emit self->toolFinished(id, name, result, isError);
        QMetaObject::invokeMethod(self, &AiAgent::runNextTool, Qt::QueuedConnection);
    };
    if (m_handler)
        m_handler(name, input, done);
    else
        done(tr("No tools are available."), true);
}

void AiAgent::dropTurn()
{
    // 마지막 질문(글이 있는 user 메시지)부터 끝까지 지운다: 거절된 차례를 남기지 않는다
    for (qsizetype i = m_messages.size() - 1; i >= 0; --i) {
        const QJsonObject question = m_messages.at(i).toObject();
        if (hasQuestion(question)) {
            // 그 질문 앞에 붙였던 이전 도구 결과는 다음 질문에 다시 붙인다 (앞 차례의 tool_use에 답해야 한다)
            QJsonArray carried;
            for (const QJsonValue &block : question.value(QStringLiteral("content")).toArray()) {
                if (block.toObject().value(QStringLiteral("type")).toString() == u"tool_result")
                    carried.append(block);
            }
            for (const QJsonValue &block : std::as_const(m_pendingResults))
                carried.append(block);
            m_pendingResults = carried;
            while (m_messages.size() > i)
                m_messages.removeLast();
            return;
        }
    }
}

void AiAgent::leaveUnanswered(const QList<QJsonObject> &toolUses, const QString &reason)
{
    for (const QJsonObject &block : toolUses)
        m_pendingResults.append(toolResult(block.value(QStringLiteral("id")).toString(), reason, true));
}

void AiAgent::finish(const QString &stopReason, const QString &note)
{
    m_running = false;
    emit status(QString());
    emit finished(stopReason, note);
}
