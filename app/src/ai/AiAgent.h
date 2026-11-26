#pragma once

#include "ClaudeClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include <functional>

// 도구를 쓰는 대화 하나 (Messages API 수동 루프).
// ask → 응답을 받아 대화에 그대로 붙이고(thinking 블록·signature·fallback 블록을 바꾸지 않는다),
// stop_reason이 tool_use면 도구를 차례로 실행해 결과를 한 user 메시지로 돌려보내고 다시 묻는다.
// - 대체 모델로 넘어간 응답(fallback 블록)은 마지막 fallback 앞의 thinking·tool_use 블록을 빼고 붙인다 (API 규칙)
// - refusal이면 그 질문 차례를 대화에서 지운다. max_tokens·멈춤·도구 횟수 초과로 답하지 못한 tool_use에는
//   다음 질문 앞에 오류 결과를 붙여 대화가 어긋나지 않게 한다
// - 도구 입력이 올바른 JSON이 아니면 도구를 실행하지 않고 INVALID_JSON 오류를 돌려준다
class AiAgent : public QObject
{
    Q_OBJECT

public:
    using ToolDone = std::function<void(const QString &result, bool isError)>;
    using ToolHandler = std::function<void(const QString &name, const QJsonObject &input, ToolDone done)>;

    explicit AiAgent(QObject *parent = nullptr);

    void setConfig(const ClaudeConfig &config) { m_config = config; }
    ClaudeConfig config() const { return m_config; }
    void setSystemPrompt(const QString &prompt) { m_system = prompt; }
    QString systemPrompt() const { return m_system; }
    void setTools(const QJsonArray &tools) { m_tools = tools; }
    void setToolHandler(ToolHandler handler) { m_handler = std::move(handler); }
    void setMaxToolRounds(int rounds) { m_maxRounds = rounds; }

    void ask(const QString &text);
    void stop();
    void reset(); // 새 대화
    bool isRunning() const { return m_running; }
    QJsonArray messages() const { return m_messages; }
    QJsonObject lastRequest() const { return m_lastRequest; }
    // 지금 대화로 보낼 요청 본문과 anthropic-beta 값 (stream은 ClaudeClient가 넣는다)
    QJsonObject buildRequest(QStringList *betas) const;

signals:
    void blockStarted(const QString &type); // text, thinking, tool_use, fallback ...
    void textDelta(const QString &text);
    void thinkingDelta(const QString &text);
    void toolStarted(const QString &id, const QString &name, const QJsonObject &input);
    void toolFinished(const QString &id, const QString &name, const QString &result, bool isError);
    void finished(const QString &stopReason, const QString &note); // 이 질문 차례가 끝남
    void failed(const QString &message);
    void status(const QString &text);
    void usage(const QJsonObject &usage, const QString &model);
    void fallbackUsed(const QString &fromModel, const QString &toModel);

private:
    void send();
    void onMessage(const QJsonObject &message);
    void runNextTool();
    void dropTurn(); // 마지막 질문 차례를 지운다 (refusal)
    void leaveUnanswered(const QList<QJsonObject> &toolUses, const QString &reason);
    void finish(const QString &stopReason, const QString &note = {});

    ClaudeClient *m_client = nullptr;
    ClaudeConfig m_config;
    QString m_system;
    QJsonArray m_tools;
    ToolHandler m_handler;
    int m_maxRounds = 30;

    QJsonArray m_messages;
    QJsonArray m_pendingResults; // 다음 질문 앞에 붙일 tool_result (답하지 못한 tool_use)
    QList<QJsonObject> m_toolQueue;
    QJsonArray m_toolResults;
    int m_rounds = 0;
    bool m_running = false;
    quint64 m_generation = 0; // stop·reset 뒤에 늦게 온 도구 결과를 버린다
    QJsonObject m_lastRequest;
};
