// AI 도우미: Claude Messages API 스트리밍(SSE) 받기, 도구 루프, 대화 이어 붙이기 규칙, 읽기 전용 검사.
// 이 PC 안의 가짜 API 서버(QTcpServer)로 확인한다 (실제 API는 부르지 않는다).
#include "ai/AiAgent.h"
#include "ai/ClaudeClient.h"
#include "cli/McpServer.h"
#include "ui/AiDbTools.h"
#include "ui/ClaudeCodeLauncher.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

namespace {

// 가짜 /v1/messages. 받은 요청을 남기고, 넣어 둔 응답을 차례로 돌려준다.
class MockClaude : public QObject
{
public:
    struct Request
    {
        QByteArray path;
        QHash<QByteArray, QByteArray> headers; // 이름은 소문자
        QJsonObject body;
    };
    struct Response
    {
        int status = 200;
        QByteArray headers; // "name: value\r\n"...
        QByteArray body;
        int splitAt = -1; // 이 바이트에서 나눠 조금 뒤에 나머지를 보낸다 (SSE 조각 받기)
    };

    MockClaude()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    void enqueue(const Response &response) { m_responses.append(response); }
    void enqueueStream(const QByteArray &sse, int splitAt = -1) { m_responses.append({200, {}, sse, splitAt}); }

    QList<Request> requests;

private:
    void read(QTcpSocket *socket)
    {
        QByteArray &data = m_pending[socket];
        data += socket->readAll();
        const qsizetype headerEnd = data.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        Request request;
        const QList<QByteArray> lines = data.left(headerEnd).split('\n');
        request.path = lines.value(0).split(' ').value(1);
        qsizetype length = 0;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            request.headers.insert(name, line.mid(colon + 1).trimmed());
            if (name == "content-length")
                length = line.mid(colon + 1).trimmed().toLongLong();
        }
        if (data.size() < headerEnd + 4 + length)
            return;
        request.body = QJsonDocument::fromJson(data.mid(headerEnd + 4, length)).object();
        m_pending.remove(socket);
        requests.append(request);

        const Response response = m_responses.isEmpty()
                                      ? Response{500, {}, R"({"type":"error","error":{"type":"api_error","message":"no scripted response"}})"}
                                      : m_responses.takeFirst();
        QByteArray head = "HTTP/1.1 " + QByteArray::number(response.status) + " Scripted\r\n";
        if (response.status == 200) {
            head += "Content-Type: text/event-stream\r\n";
        } else {
            head += "Content-Type: application/json\r\n";
            head += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
        }
        head += response.headers + "Connection: close\r\n\r\n";
        socket->write(head);
        if (response.splitAt > 0 && response.splitAt < response.body.size()) {
            socket->write(response.body.left(response.splitAt));
            socket->flush();
            const QByteArray rest = response.body.mid(response.splitAt);
            QTimer::singleShot(40, socket, [socket, rest] {
                socket->write(rest);
                socket->disconnectFromHost();
            });
        } else {
            socket->write(response.body);
            socket->disconnectFromHost();
        }
    }

    QTcpServer m_server;
    QList<Response> m_responses;
    QHash<QTcpSocket *, QByteArray> m_pending;
};

QByteArray sse(const QJsonObject &event)
{
    return "event: " + event.value(QStringLiteral("type")).toString().toUtf8() + "\ndata: "
           + QJsonDocument(event).toJson(QJsonDocument::Compact) + "\n\n";
}

// 스트리밍 응답 한 개를 이벤트 순서대로 만든다
class Stream
{
public:
    explicit Stream(const QString &model = QStringLiteral("claude-opus-5-5"))
    {
        m_data += sse(QJsonObject{
            {"type", "message_start"},
            {"message", QJsonObject{{"id", "msg_test"}, {"type", "message"}, {"role", "assistant"}, {"model", model},
                                    {"content", QJsonArray()}, {"stop_reason", QJsonValue::Null},
                                    {"usage", QJsonObject{{"input_tokens", 120}, {"output_tokens", 1}}}}},
        });
        m_data += sse(QJsonObject{{"type", "ping"}});
    }

    Stream &thinking(const QString &text, const QString &signature)
    {
        start(QJsonObject{{"type", "thinking"}, {"thinking", ""}});
        delta(QJsonObject{{"type", "thinking_delta"}, {"thinking", text}});
        delta(QJsonObject{{"type", "signature_delta"}, {"signature", signature}});
        return stop();
    }
    Stream &text(const QString &text)
    {
        start(QJsonObject{{"type", "text"}, {"text", ""}});
        const qsizetype half = text.size() / 2;
        delta(QJsonObject{{"type", "text_delta"}, {"text", text.left(half)}});
        delta(QJsonObject{{"type", "text_delta"}, {"text", text.mid(half)}});
        return stop();
    }
    Stream &tool(const QString &id, const QString &name, const QStringList &jsonParts)
    {
        start(QJsonObject{{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", QJsonObject()}});
        for (const QString &part : jsonParts)
            delta(QJsonObject{{"type", "input_json_delta"}, {"partial_json", part}});
        return stop();
    }
    Stream &block(const QJsonObject &block)
    {
        start(block);
        return stop();
    }
    Stream &error(const QString &type)
    {
        m_data += sse(QJsonObject{{"type", "error"}, {"error", QJsonObject{{"type", type}, {"message", "Overloaded"}}}});
        return *this;
    }
    QByteArray end(const QString &stopReason, const QJsonObject &stopDetails = {})
    {
        QJsonObject delta{{"stop_reason", stopReason}};
        if (!stopDetails.isEmpty())
            delta.insert("stop_details", stopDetails);
        m_data += sse(QJsonObject{{"type", "message_delta"}, {"delta", delta}, {"usage", QJsonObject{{"output_tokens", 42}}}});
        m_data += sse(QJsonObject{{"type", "message_stop"}});
        return m_data;
    }
    QByteArray unfinished() const { return m_data; }

private:
    void start(const QJsonObject &block)
    {
        m_data += sse(QJsonObject{{"type", "content_block_start"}, {"index", m_index}, {"content_block", block}});
    }
    void delta(const QJsonObject &delta)
    {
        m_data += sse(QJsonObject{{"type", "content_block_delta"}, {"index", m_index}, {"delta", delta}});
    }
    Stream &stop()
    {
        m_data += sse(QJsonObject{{"type", "content_block_stop"}, {"index", m_index}});
        ++m_index;
        return *this;
    }

    QByteArray m_data;
    int m_index = 0;
};

ClaudeConfig configFor(const MockClaude &mock)
{
    ClaudeConfig config;
    config.apiKey = QStringLiteral("test-key");
    config.baseUrl = mock.url();
    return config;
}

QStringList types(const QJsonArray &content)
{
    QStringList list;
    for (const QJsonValue &block : content)
        list << block.toObject().value("type").toString();
    return list;
}

QJsonArray contentOf(const QJsonArray &messages, int index)
{
    return messages.at(index).toObject().value("content").toArray();
}

} // namespace

class AiAgentTest : public QObject
{
    Q_OBJECT

private slots:
    void runsToolsAndSendsResultsTogether()
    {
        MockClaude mock;
        Stream first;
        first.thinking(QStringLiteral("Need the tables."), QStringLiteral("sig-1"))
            .text(QStringLiteral("Checking the schema."))
            .tool(QStringLiteral("toolu_1"), QStringLiteral("list_tables"), {QStringLiteral("{\"schema\":"), QStringLiteral(" \"HR\"}")})
            .tool(QStringLiteral("toolu_2"), QStringLiteral("describe_table"), {QStringLiteral("{\"table\":\"EMP\"}")});
        const QByteArray firstBody = first.end(QStringLiteral("tool_use"));
        mock.enqueueStream(firstBody, int(firstBody.size() / 2)); // 이벤트 중간에서 끊어 보낸다
        mock.enqueueStream(Stream().text(QStringLiteral("HR has 2 tables.")).end(QStringLiteral("end_turn")));

        AiAgent agent;
        agent.setConfig(configFor(mock));
        agent.setSystemPrompt(QStringLiteral("You are a DB assistant."));
        agent.setTools(AiDbTools::definitions());
        QList<QPair<QString, QJsonObject>> calls;
        agent.setToolHandler([&calls](const QString &name, const QJsonObject &input, AiAgent::ToolDone done) {
            calls.append({name, input});
            if (name == u"list_tables") // 늦게 끝나는 도구도 순서를 지킨다
                QTimer::singleShot(20, [done] { done(QStringLiteral("EMP\nDEPT"), false); });
            else
                done(QStringLiteral("EMPNO NUMBER"), false);
        });
        QString streamed;
        connect(&agent, &AiAgent::textDelta, this, [&streamed](const QString &text) { streamed += text; });
        QSignalSpy finished(&agent, &AiAgent::finished);
        QSignalSpy failed(&agent, &AiAgent::failed);

        agent.ask(QStringLiteral("What tables are in HR?"));
        QVERIFY(finished.wait(10000));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("end_turn"));
        QCOMPARE(streamed, QStringLiteral("Checking the schema.HR has 2 tables."));

        QCOMPARE(calls.size(), 2);
        QCOMPARE(calls.at(0).first, QStringLiteral("list_tables"));
        QCOMPARE(calls.at(0).second.value("schema").toString(), QStringLiteral("HR"));
        QCOMPARE(calls.at(1).first, QStringLiteral("describe_table"));
        QCOMPARE(calls.at(1).second.value("table").toString(), QStringLiteral("EMP"));

        // 요청 헤더와 본문
        QCOMPARE(mock.requests.size(), 2);
        const MockClaude::Request &request = mock.requests.first();
        QCOMPARE(request.path, QByteArray("/v1/messages"));
        QCOMPARE(request.headers.value("x-api-key"), QByteArray("test-key"));
        QCOMPARE(request.headers.value("anthropic-version"), QByteArray("2026-10-21"));
        QVERIFY(!request.headers.contains("anthropic-beta")); // 공식 API가 아니면 베타 기능을 보내지 않는다
        const QJsonObject body = request.body;
        QCOMPARE(body.value("stream").toBool(), true);
        QCOMPARE(body.value("model").toString(), QStringLiteral("claude-opus-5-5"));
        QCOMPARE(body.value("thinking").toObject().value("type").toString(), QStringLiteral("adaptive"));
        QCOMPARE(body.value("output_config").toObject().value("effort").toString(), QStringLiteral("medium"));
        QCOMPARE(body.value("cache_control").toObject().value("type").toString(), QStringLiteral("ephemeral"));
        QVERIFY(!body.contains("fallbacks"));
        QVERIFY(!body.contains("tool_choice"));
        QCOMPARE(body.value("tools").toArray().size(), AiDbTools::definitions().size());
        QVERIFY(!body.value("tools").toArray().first().toObject().contains("eager_input_streaming"));
        QCOMPARE(body.value("system").toArray().first().toObject().value("text").toString(), QStringLiteral("You are a DB assistant."));

        // 두 번째 요청: 응답을 그대로 붙이고(thinking·signature 포함) 도구 결과 둘을 한 user 메시지로
        const QJsonArray messages = mock.requests.at(1).body.value("messages").toArray();
        QCOMPARE(messages.size(), 3);
        QCOMPARE(messages.at(1).toObject().value("role").toString(), QStringLiteral("assistant"));
        const QJsonArray assistant = contentOf(messages, 1);
        QCOMPARE(types(assistant), (QStringList{"thinking", "text", "tool_use", "tool_use"}));
        QCOMPARE(assistant.at(0).toObject().value("thinking").toString(), QStringLiteral("Need the tables."));
        QCOMPARE(assistant.at(0).toObject().value("signature").toString(), QStringLiteral("sig-1"));
        QCOMPARE(assistant.at(2).toObject().value("input").toObject().value("schema").toString(), QStringLiteral("HR"));
        QVERIFY(!assistant.at(2).toObject().contains("_invalidInput"));
        const QJsonArray results = contentOf(messages, 2);
        QCOMPARE(messages.at(2).toObject().value("role").toString(), QStringLiteral("user"));
        QCOMPARE(types(results), (QStringList{"tool_result", "tool_result"}));
        QCOMPARE(results.at(0).toObject().value("tool_use_id").toString(), QStringLiteral("toolu_1"));
        QCOMPARE(results.at(0).toObject().value("content").toString(), QStringLiteral("EMP\nDEPT"));
        QCOMPARE(results.at(1).toObject().value("tool_use_id").toString(), QStringLiteral("toolu_2"));
        QVERIFY(!results.at(0).toObject().contains("is_error"));

        QCOMPARE(agent.messages().size(), 4);
        QCOMPARE(contentOf(agent.messages(), 3).first().toObject().value("text").toString(), QStringLiteral("HR has 2 tables."));
    }

    void invalidToolInputIsNotRun()
    {
        MockClaude mock;
        mock.enqueueStream(Stream()
                               .tool(QStringLiteral("toolu_9"), QStringLiteral("run_query"), {QStringLiteral("{\"sql\": \"SELECT")})
                               .end(QStringLiteral("tool_use")));
        mock.enqueueStream(Stream().text(QStringLiteral("Retrying.")).end(QStringLiteral("end_turn")));
        AiAgent agent;
        agent.setConfig(configFor(mock));
        int called = 0;
        agent.setToolHandler([&called](const QString &, const QJsonObject &, AiAgent::ToolDone done) {
            ++called;
            done(QString(), false);
        });
        QSignalSpy finished(&agent, &AiAgent::finished);
        agent.ask(QStringLiteral("Count rows"));
        QVERIFY(finished.wait(10000));
        QCOMPARE(called, 0);
        QCOMPARE(mock.requests.size(), 2);
        const QJsonArray messages = mock.requests.at(1).body.value("messages").toArray();
        QVERIFY(!contentOf(messages, 1).first().toObject().contains("_invalidInput"));
        const QJsonObject result = contentOf(messages, 2).first().toObject();
        QCOMPARE(result.value("tool_use_id").toString(), QStringLiteral("toolu_9"));
        QCOMPARE(result.value("is_error").toBool(), true);
        QVERIFY(result.value("content").toString().contains("INVALID_JSON"));
    }

    void refusalDropsTheTurn()
    {
        MockClaude mock;
        mock.enqueueStream(Stream().text(QStringLiteral("Hello.")).end(QStringLiteral("end_turn")));
        mock.enqueueStream(Stream().text(QStringLiteral("I can")).end(QStringLiteral("refusal"), QJsonObject{{"category", "cyber"}}));
        mock.enqueueStream(Stream().text(QStringLiteral("Sure.")).end(QStringLiteral("end_turn")));
        AiAgent agent;
        agent.setConfig(configFor(mock));
        QSignalSpy finished(&agent, &AiAgent::finished);

        agent.ask(QStringLiteral("q1"));
        QVERIFY(finished.wait(10000));
        agent.ask(QStringLiteral("q2"));
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.at(1).at(0).toString(), QStringLiteral("refusal"));
        QVERIFY(finished.at(1).at(1).toString().contains("cyber"));
        QCOMPARE(agent.messages().size(), 2); // q2와 거절된 답은 남지 않는다

        agent.ask(QStringLiteral("q3"));
        QVERIFY(finished.wait(10000));
        const QJsonArray messages = mock.requests.at(2).body.value("messages").toArray();
        QCOMPARE(messages.size(), 3);
        QCOMPARE(contentOf(messages, 2).first().toObject().value("text").toString(), QStringLiteral("q3"));
    }

    void unansweredToolUseGetsResultBeforeNextQuestion()
    {
        MockClaude mock;
        mock.enqueueStream(Stream()
                               .text(QStringLiteral("Let me look."))
                               .tool(QStringLiteral("toolu_a"), QStringLiteral("list_schemas"), {QStringLiteral("{}")})
                               .end(QStringLiteral("max_tokens")));
        mock.enqueueStream(Stream().text(QStringLiteral("OK.")).end(QStringLiteral("end_turn")));
        AiAgent agent;
        agent.setConfig(configFor(mock));
        int called = 0;
        agent.setToolHandler([&called](const QString &, const QJsonObject &, AiAgent::ToolDone done) {
            ++called;
            done(QString(), false);
        });
        QSignalSpy finished(&agent, &AiAgent::finished);
        agent.ask(QStringLiteral("q1"));
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("max_tokens"));
        QCOMPARE(called, 0);

        agent.ask(QStringLiteral("next"));
        QVERIFY(finished.wait(10000));
        const QJsonArray messages = mock.requests.at(1).body.value("messages").toArray();
        QCOMPARE(messages.size(), 3);
        const QJsonArray question = contentOf(messages, 2);
        QCOMPARE(types(question), (QStringList{"tool_result", "text"}));
        QCOMPARE(question.first().toObject().value("tool_use_id").toString(), QStringLiteral("toolu_a"));
        QCOMPARE(question.first().toObject().value("is_error").toBool(), true);
    }

    void stopWhileToolRuns()
    {
        MockClaude mock;
        mock.enqueueStream(Stream()
                               .tool(QStringLiteral("toolu_s"), QStringLiteral("run_query"), {QStringLiteral("{\"sql\":\"SELECT 1\"}")})
                               .end(QStringLiteral("tool_use")));
        mock.enqueueStream(Stream().text(QStringLiteral("OK.")).end(QStringLiteral("end_turn")));
        AiAgent agent;
        agent.setConfig(configFor(mock));
        AiAgent::ToolDone pending;
        agent.setToolHandler([&pending](const QString &, const QJsonObject &, AiAgent::ToolDone done) { pending = done; });
        QSignalSpy started(&agent, &AiAgent::toolStarted);
        QSignalSpy finished(&agent, &AiAgent::finished);
        agent.ask(QStringLiteral("q1"));
        QVERIFY(started.wait(10000));
        agent.stop();
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("stopped"));
        pending(QStringLiteral("late result"), false); // 멈춘 뒤 늦게 온 결과는 버린다
        QCOMPARE(mock.requests.size(), 1);

        agent.ask(QStringLiteral("again"));
        QVERIFY(finished.wait(10000));
        const QJsonArray messages = mock.requests.at(1).body.value("messages").toArray();
        QCOMPARE(messages.size(), 3);
        const QJsonArray question = contentOf(messages, 2);
        QCOMPARE(types(question), (QStringList{"tool_result", "text"}));
        QCOMPARE(question.first().toObject().value("is_error").toBool(), true);
        QVERIFY(!question.first().toObject().value("content").toString().contains("late result"));
    }

    void fallbackEchoRule()
    {
        MockClaude mock;
        mock.enqueueStream(Stream()
                               .thinking(QStringLiteral("t1"), QStringLiteral("sig-a"))
                               .block(QJsonObject{{"type", "fallback"},
                                                  {"from", QJsonObject{{"model", "claude-opus-5-5"}}},
                                                  {"to", QJsonObject{{"model", "claude-sonnet-5-5"}}}})
                               .thinking(QStringLiteral("t2"), QStringLiteral("sig-b"))
                               .text(QStringLiteral("Answer."))
                               .end(QStringLiteral("end_turn")));
        mock.enqueueStream(Stream().text(QStringLiteral("OK.")).end(QStringLiteral("end_turn")));
        AiAgent agent;
        agent.setConfig(configFor(mock));
        QSignalSpy fallback(&agent, &AiAgent::fallbackUsed);
        QSignalSpy finished(&agent, &AiAgent::finished);
        agent.ask(QStringLiteral("q1"));
        QVERIFY(finished.wait(10000));
        QCOMPARE(fallback.count(), 1);
        QCOMPARE(fallback.first().at(0).toString(), QStringLiteral("claude-opus-5-5"));
        QCOMPARE(fallback.first().at(1).toString(), QStringLiteral("claude-sonnet-5-5"));

        agent.ask(QStringLiteral("q2"));
        QVERIFY(finished.wait(10000));
        const QJsonArray assistant = contentOf(mock.requests.at(1).body.value("messages").toArray(), 1);
        // 마지막 fallback 앞의 thinking은 빼고, fallback 블록은 그 자리에 둔다
        QCOMPARE(types(assistant), (QStringList{"fallback", "thinking", "text"}));
        QCOMPARE(assistant.at(1).toObject().value("signature").toString(), QStringLiteral("sig-b"));
    }

    void retriesOverloadAndRetryAfter()
    {
        MockClaude mock;
        mock.enqueueStream(Stream().error(QStringLiteral("overloaded_error")).unfinished()); // 답을 시작하기 전 과부하
        mock.enqueue({529, "retry-after: 0.05\r\n", R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})"});
        mock.enqueueStream(Stream().text(QStringLiteral("Done.")).end(QStringLiteral("end_turn")));
        AiAgent agent;
        agent.setConfig(configFor(mock));
        QSignalSpy finished(&agent, &AiAgent::finished);
        QSignalSpy failed(&agent, &AiAgent::failed);
        agent.ask(QStringLiteral("q1"));
        QVERIFY(finished.wait(15000));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(mock.requests.size(), 3);
        QCOMPARE(mock.requests.at(2).body, mock.requests.at(0).body);
        QCOMPARE(agent.messages().size(), 2);
    }

    void authErrorIsNotRetried()
    {
        MockClaude mock;
        mock.enqueue({401, {}, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})"});
        AiAgent agent;
        agent.setConfig(configFor(mock));
        QSignalSpy failed(&agent, &AiAgent::failed);
        agent.ask(QStringLiteral("q1"));
        QVERIFY(failed.wait(10000));
        QVERIFY(failed.first().at(0).toString().contains("401"));
        QCOMPARE(mock.requests.size(), 1);
        QVERIFY(!agent.isRunning());
    }

    void betaHeader()
    {
        MockClaude mock;
        mock.enqueueStream(Stream().text(QStringLiteral("Hi.")).end(QStringLiteral("end_turn")));
        ClaudeClient client;
        QSignalSpy finished(&client, &ClaudeClient::finished);
        client.send(configFor(mock), QJsonObject{{"model", "claude-opus-5-5"}, {"max_tokens", 16}},
                    {QStringLiteral("beta-one"), QStringLiteral("beta-two")});
        QVERIFY(finished.wait(10000));
        QCOMPARE(mock.requests.first().headers.value("anthropic-beta"), QByteArray("beta-one,beta-two"));
        const QJsonObject message = finished.first().at(0).toJsonObject();
        QCOMPARE(message.value("stop_reason").toString(), QStringLiteral("end_turn"));
        QCOMPARE(message.value("usage").toObject().value("input_tokens").toInt(), 120);
        QCOMPARE(message.value("usage").toObject().value("output_tokens").toInt(), 42);
    }

    void requestForEndpoints()
    {
        AiAgent agent;
        agent.setTools(AiDbTools::definitions());
        ClaudeConfig config;

        // 공식 API + Opus 5.5: 서버 대체 모델, eager input streaming
        QStringList betas;
        agent.setConfig(config);
        QJsonObject body = agent.buildRequest(&betas);
        QCOMPARE(body.value("fallbacks").toString(), QStringLiteral("default"));
        QCOMPARE(betas, QStringList{QStringLiteral("server-side-fallback-2026-07-01")});
        QCOMPARE(body.value("tools").toArray().first().toObject().value("eager_input_streaming").toBool(), true);
        QCOMPARE(body.value("max_tokens").toInt(), 64000);

        // Haiku: 대체 모델 없음
        config.model = QStringLiteral("claude-haiku-5-5");
        agent.setConfig(config);
        betas.clear();
        body = agent.buildRequest(&betas);
        QVERIFY(!body.contains("fallbacks"));
        QVERIFY(betas.isEmpty());

        // 회사 게이트웨이: 베타 기능 없이
        config.model = QStringLiteral("claude-sonnet-5-5");
        config.baseUrl = QStringLiteral("https://llm-gateway.example.com/anthropic");
        agent.setConfig(config);
        betas.clear();
        body = agent.buildRequest(&betas);
        QVERIFY(!body.contains("fallbacks"));
        QVERIFY(betas.isEmpty());
        QVERIFY(!body.value("tools").toArray().first().toObject().contains("eager_input_streaming"));
    }

    void readOnlyGuard_data()
    {
        QTest::addColumn<QString>("sql");
        QTest::addColumn<int>("dialect");
        QTest::addColumn<bool>("allowed");
        const int oracle = int(SqlDialect::Oracle), pg = int(SqlDialect::PostgreSql), mysql = int(SqlDialect::MySql);
        QTest::newRow("select") << "SELECT * FROM emp" << oracle << true;
        QTest::newRow("with select") << "with x as (select 1 from dual) select * from x" << oracle << true;
        QTest::newRow("comment first") << "-- note\nselect 1 from dual" << oracle << true;
        QTest::newRow("words in string") << "select 'delete from x; drop table y' from dual" << oracle << true;
        QTest::newRow("column like keyword") << "SELECT update_date, created_by FROM t" << pg << true;
        QTest::newRow("show create") << "SHOW CREATE TABLE emp" << mysql << true;
        QTest::newRow("explain") << "EXPLAIN SELECT 1" << mysql << true;
        QTest::newRow("trailing semicolon") << "select 1;" << pg << true;
        QTest::newRow("delete") << "DELETE FROM emp" << oracle << false;
        QTest::newRow("cte delete") << "WITH d AS (DELETE FROM emp RETURNING *) SELECT * FROM d" << pg << false;
        QTest::newRow("select into table") << "SELECT * INTO emp_copy FROM emp" << pg << false;
        QTest::newRow("into outfile") << "SELECT * FROM emp INTO OUTFILE '/tmp/emp.csv'" << mysql << false;
        QTest::newRow("for update") << "SELECT * FROM emp FOR UPDATE" << oracle << false;
        QTest::newRow("lock in share mode") << "SELECT * FROM emp LOCK IN SHARE MODE" << mysql << false;
        QTest::newRow("two statements") << "SELECT 1; DROP TABLE emp" << pg << false;
        QTest::newRow("explain analyze") << "EXPLAIN ANALYZE SELECT * FROM emp" << pg << false;
        QTest::newRow("plsql block") << "BEGIN DELETE FROM emp; END;" << oracle << false;
        QTest::newRow("call") << "CALL refresh_stats()" << mysql << false;
        QTest::newRow("empty") << "  " << pg << false;
    }

    void readOnlyGuard()
    {
        QFETCH(QString, sql);
        QFETCH(int, dialect);
        QFETCH(bool, allowed);
        const QString problem = AiDbTools::readOnlyProblem(sql, SqlDialect(dialect));
        QVERIFY2(problem.isEmpty() == allowed, qPrintable(problem.isEmpty() ? QStringLiteral("allowed") : problem));
    }

    void explainGuard()
    {
        QVERIFY(AiDbTools::explainProblem(QStringLiteral("SELECT * FROM emp"), SqlDialect::PostgreSql).isEmpty());
        QVERIFY(AiDbTools::explainProblem(QStringLiteral("DELETE FROM emp WHERE id = 1"), SqlDialect::Oracle).isEmpty());
        QVERIFY(!AiDbTools::explainProblem(QStringLiteral("SELECT 1; DROP TABLE emp"), SqlDialect::PostgreSql).isEmpty());
        QVERIFY(!AiDbTools::explainProblem(QStringLiteral("DROP TABLE emp"), SqlDialect::MySql).isEmpty());
    }

    // ---- dbaro-cli mcp (MCP 서버). DB·엔진 없이 프로토콜과 막는 규칙만 본다

    void mcpProtocol()
    {
        McpHarness mcp;
        QVERIFY2(mcp.ok, qPrintable(mcp.error));

        QJsonObject init = mcp.request(QStringLiteral("initialize"),
                                       QJsonObject{{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject()},
                                                   {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}});
        QCOMPARE(init.value("id").toInt(), mcp.lastId);
        QCOMPARE(init.value("result").toObject().value("protocolVersion").toString(), QStringLiteral("2025-06-18"));
        QVERIFY(init.value("result").toObject().value("capabilities").toObject().contains("tools"));
        QVERIFY(init.value("result").toObject().value("instructions").toString().contains("open_in_dbaro"));
        // 모르는 버전이면 아는 최신 버전
        init = mcp.request(QStringLiteral("initialize"), QJsonObject{{"protocolVersion", "1999-01-01"}});
        QCOMPARE(init.value("result").toObject().value("protocolVersion").toString(), QStringLiteral("2025-11-25"));

        // 알림에는 답하지 않는다
        mcp.lines.clear();
        mcp.server.handleLine(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        QVERIFY(mcp.lines.isEmpty());

        QCOMPARE(mcp.request(QStringLiteral("ping")).value("result").toObject(), QJsonObject());
        QCOMPARE(mcp.request(QStringLiteral("resources/list")).value("error").toObject().value("code").toInt(), -32601);
        mcp.lines.clear();
        mcp.server.handleLine("{not json");
        QCOMPARE(mcp.lines.size(), 1);
        QCOMPARE(QJsonDocument::fromJson(mcp.lines.first()).object().value("error").toObject().value("code").toInt(), -32700);

        const QJsonArray tools = mcp.request(QStringLiteral("tools/list")).value("result").toObject().value("tools").toArray();
        QStringList names;
        for (const QJsonValue &tool : tools)
            names << tool.toObject().value("name").toString();
        QCOMPARE(names, (QStringList{"list_connections", "list_schemas", "list_tables", "describe_table", "get_ddl", "run_query",
                                     "explain_query", "open_in_dbaro"}));
        const QJsonObject runQuery = tools.at(5).toObject();
        const QJsonObject input = runQuery.value("inputSchema").toObject();
        QVERIFY(input.value("properties").toObject().contains("connection"));
        QVERIFY(input.value("properties").toObject().contains("schema"));
        QVERIFY(input.value("required").toArray().contains(QJsonValue("connection"))); // 연결이 둘이라 꼭 정한다
        QCOMPARE(runQuery.value("annotations").toObject().value("readOnlyHint").toBool(), true);
        QVERIFY(!runQuery.value("description").toString().contains("propose_sql"));

        QCOMPARE(mcp.request(QStringLiteral("tools/call"), QJsonObject{{"name", "drop_everything"}, {"arguments", QJsonObject()}})
                     .value("error").toObject().value("code").toInt(),
                 -32602);
    }

    void mcpTools()
    {
        McpHarness mcp;
        QVERIFY2(mcp.ok, qPrintable(mcp.error));

        QString text = mcp.tool(QStringLiteral("list_connections"), {});
        QVERIFY2(text.contains("Dev DB: PostgreSQL, database app"), qPrintable(text));
        QVERIFY2(text.contains("Prod DB: Oracle") && text.contains("run_query off"), qPrintable(text));
        QVERIFY(!text.contains("localhost")); // 주소는 보내지 않는다

        bool isError = false;
        text = mcp.tool(QStringLiteral("run_query"), QJsonObject{{"sql", "SELECT 1"}}, &isError);
        QVERIFY(isError && text.contains("Which connection"));
        text = mcp.tool(QStringLiteral("run_query"), QJsonObject{{"connection", "nope"}, {"sql", "SELECT 1"}}, &isError);
        QVERIFY(isError && text.contains("Dev DB, Prod DB"));
        // 읽기 전용 검사는 접속하기 전에
        text = mcp.tool(QStringLiteral("run_query"), QJsonObject{{"connection", "dev db"}, {"sql", "DELETE FROM emp"}}, &isError);
        QVERIFY2(isError && text.contains("open_in_dbaro") && !text.contains("propose_sql"), qPrintable(text));
        text = mcp.tool(QStringLiteral("run_query"),
                        QJsonObject{{"connection", "Dev DB"}, {"sql", "WITH d AS (DELETE FROM emp RETURNING *) SELECT * FROM d"}}, &isError);
        QVERIFY(isError);
        text = mcp.tool(QStringLiteral("explain_query"), QJsonObject{{"connection", "Dev DB"}, {"sql", "SELECT 1; DROP TABLE emp"}}, &isError);
        QVERIFY(isError);
        // 운영 연결은 조회하지 않는다
        text = mcp.tool(QStringLiteral("run_query"), QJsonObject{{"connection", "Prod DB"}, {"sql", "SELECT 1 FROM dual"}}, &isError);
        QVERIFY2(isError && text.contains("production"), qPrintable(text));

        // open_in_dbaro: SQL을 임시 파일로 넘겨 DBaro를 연다 (여기서는 실행하지 않고 인자만 본다)
        QStringList launched;
        mcp.server.setLauncher([&launched](const QStringList &arguments, QString *) {
            launched = arguments;
            return true;
        });
        text = mcp.tool(QStringLiteral("open_in_dbaro"),
                        QJsonObject{{"connection", "Prod DB"}, {"sql", "UPDATE emp SET sal = sal * 1.1;"}, {"title", "Raise"}}, &isError);
        QVERIFY2(!isError && text.contains("has not been run"), qPrintable(text));
        QCOMPARE(launched.value(0), QStringLiteral("--connection"));
        QCOMPARE(launched.value(1), QStringLiteral("prod-1"));
        QCOMPARE(launched.value(2), QStringLiteral("--open-sql"));
        QCOMPARE(launched.mid(4), (QStringList{"--title", "Raise"}));
        QFile sqlFile(launched.value(3));
        QVERIFY(QFileInfo(sqlFile).fileName().startsWith("dbaro-sql-"));
        QVERIFY(sqlFile.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(sqlFile.readAll()).trimmed(), QStringLiteral("UPDATE emp SET sal = sal * 1.1;"));
        sqlFile.close();
        sqlFile.remove();
    }

    // Open Claude Code: 새 터미널의 claude에 넘기는 맥락과 인자 (실제로 띄우지는 않는다)
    void claudeCodeLaunchArguments()
    {
        ClaudeCodeLauncher::Context context;
        context.profile.name = QStringLiteral("Sales DB");
        context.profile.type = DbType::PostgreSQL;
        context.profile.database = QStringLiteral("sales");
        context.serverInfo = QStringLiteral("PostgreSQL 17.2\nsecond line");
        context.schema = QStringLiteral("public");
        const QString folder = QDir::tempPath();

        QString prompt = ClaudeCodeLauncher::contextPrompt(context, folder);
        QVERIFY(prompt.contains("connection 'Sales DB' (PostgreSQL, PostgreSQL 17.2)"));
        QVERIFY(!prompt.contains("second line"));
        QVERIFY(prompt.contains("schema of the user's DBaro query tab is public"));
        QVERIFY(prompt.contains("open_in_dbaro"));
        QVERIFY(prompt.contains("DBaro's default folder")); // 스크립트 폴더가 아니면 그렇게 말한다
        QVERIFY(!prompt.contains("PRODUCTION"));
        QVERIFY(!prompt.contains('"')); // 명령줄로 넘기므로 큰따옴표를 쓰지 않는다

        context.profile.environment = Environment::Production;
        context.database = QStringLiteral("archive");
        prompt = ClaudeCodeLauncher::contextPrompt(context, folder);
        QVERIFY(prompt.contains("PRODUCTION"));
        QVERIFY(prompt.contains("uses the database archive") && prompt.contains("default database (sales)"));

        QStringList arguments = ClaudeCodeLauncher::arguments(context, folder, {});
        QCOMPARE(arguments.size(), 2);
        QCOMPARE(arguments.first(), QStringLiteral("--append-system-prompt"));
        arguments = ClaudeCodeLauncher::arguments(context, folder, QStringLiteral("C:/x/claude-mcp.json"));
        QCOMPARE(arguments.mid(2), (QStringList{"--mcp-config", "C:/x/claude-mcp.json"}));

        // 첫 질문은 맨 끝 인자 (claude "질문"). -로 시작하면 옵션으로 읽히지 않게 앞에 글을 붙인다
        const QString question = QStringLiteral("Explain this SQL.\n\n```sql\nSELECT \"Name\" FROM t WHERE a = 'x'\n```");
        arguments = ClaudeCodeLauncher::arguments(context, folder, {}, QStringLiteral("  ") + question + QStringLiteral("\n"));
        QCOMPARE(arguments.size(), 3);
        QCOMPARE(arguments.last(), question);
        arguments = ClaudeCodeLauncher::arguments(context, folder, {}, QStringLiteral("-- why does this fail?"));
        QCOMPARE(arguments.last(), QStringLiteral("Question: -- why does this fail?"));
    }

private:
    // 연결 둘(개발 PostgreSQL, 운영 Oracle)로 띄운 MCP 서버. 답은 lines에 모인다
    struct McpHarness
    {
        McpServer server;
        QTemporaryDir dir;
        QList<QByteArray> lines;
        int lastId = 0;
        bool ok = false;
        QString error;

        McpHarness()
        {
            const QJsonObject dev{{"id", "dev-1"}, {"name", "Dev DB"}, {"type", "postgres"}, {"host", "localhost"}, {"port", 1},
                                  {"database", "app"}, {"user", "u"}, {"savePassword", false}, {"environment", "development"}};
            const QJsonObject prod{{"id", "prod-1"}, {"name", "Prod DB"}, {"type", "oracle"}, {"host", "localhost"}, {"port", 1},
                                   {"database", "ORCL"}, {"user", "u"}, {"savePassword", false}, {"environment", "production"}};
            const QString path = dir.filePath("connections.json");
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) {
                error = file.errorString();
                return;
            }
            file.write(QJsonDocument(QJsonObject{{"version", 1}, {"connections", QJsonArray{dev, prod}}}).toJson());
            file.close();
            qputenv("DBARO_PASSWORD", "unused"); // 비밀번호가 있어야 도구가 진행한다 (접속은 하지 않는다)
            McpServer::Options options;
            options.connectionsFile = path;
            server.setWriter([this](const QByteArray &line) { lines.append(line); });
            ok = server.setUp(options, &error);
        }

        QJsonObject request(const QString &method, const QJsonObject &params = {})
        {
            lines.clear();
            QJsonObject message{{"jsonrpc", "2.0"}, {"id", ++lastId}, {"method", method}};
            if (!params.isEmpty())
                message.insert("params", params);
            server.handleLine(QJsonDocument(message).toJson(QJsonDocument::Compact));
            if (!QTest::qWaitFor([this] { return !lines.isEmpty(); }, 5000))
                return {};
            return QJsonDocument::fromJson(lines.first()).object();
        }

        QString tool(const QString &name, const QJsonObject &arguments, bool *isError = nullptr)
        {
            const QJsonObject result =
                request(QStringLiteral("tools/call"), QJsonObject{{"name", name}, {"arguments", arguments}}).value("result").toObject();
            if (isError)
                *isError = result.value("isError").toBool();
            return result.value("content").toArray().first().toObject().value("text").toString();
        }
    };
};

QTEST_GUILESS_MAIN(AiAgentTest)
#include "AiAgentTest.moc"
