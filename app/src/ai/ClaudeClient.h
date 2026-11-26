#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Claude API 연결 설정 (AI 도우미 설정 창에서 정한다)
struct ClaudeConfig
{
    QString apiKey;
    QString baseUrl = QStringLiteral("https://api.anthropic.com");
    QString model = QStringLiteral("claude-opus-5-5");
    QString effort = QStringLiteral("medium");
    int maxTokens = 64000;

    // Anthropic API 자체인지 (게이트웨이·프록시면 베타 기능을 보내지 않는다)
    bool isOfficialEndpoint() const;
};

// Claude Messages API를 스트리밍(SSE)으로 부른다. C++에는 공식 SDK가 없어 HTTP로 직접 부른다.
// - 받은 이벤트로 응답 메시지(content 블록들)를 그대로 조립해 finished로 넘긴다 (thinking 블록의 signature,
//   fallback 블록 포함: 다음 요청에 바뀌지 않게 돌려보내야 한다)
// - tool_use 입력은 조각을 모아 끝에 엄격하게 파싱한다. 파싱되지 않으면 블록에 "_invalidInput"(받은 글)을 남긴다
// - 429·5xx·529·연결 오류는 두 번까지 다시 시도한다 (retry-after를 따른다)
// - 네트워크는 시스템 프록시를 쓰고, TLS는 OS 인증서 저장소로 검증한다 (회사 HTTPS 검사 프록시 환경)
class ClaudeClient : public QObject
{
    Q_OBJECT

public:
    explicit ClaudeClient(QObject *parent = nullptr);
    ~ClaudeClient() override;

    // body: model·messages·tools 등 (stream은 넣어 준다). betas: anthropic-beta 헤더 값들
    void send(const ClaudeConfig &config, const QJsonObject &body, const QStringList &betas = {});
    void abort();
    bool isBusy() const { return m_reply != nullptr || m_retryPending; }

    static QString apiVersion() { return QStringLiteral("2026-10-21"); }

signals:
    void blockStarted(int index, const QString &type, const QJsonObject &block);
    void textDelta(int index, const QString &text);
    void thinkingDelta(int index, const QString &text);
    void toolInputDelta(int index, const QString &partialJson);
    void finished(const QJsonObject &message); // {id, model, content, stop_reason, stop_details, usage}
    void failed(const QString &message, int httpStatus);
    void retrying(int attempt, int delayMs, const QString &reason);

private:
    void start();
    void onReadyRead();
    void parseBuffer();
    void onFinished();
    void handleEvent(const QString &event, const QByteArray &data);
    void scheduleRetry(const QString &reason, int retryAfterMs);

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QTimer *m_retryTimer = nullptr;
    bool m_retryPending = false;
    ClaudeConfig m_config;
    QByteArray m_payload;
    QStringList m_betas;
    int m_attempt = 0;

    QByteArray m_buffer;
    QJsonObject m_message;
    QList<QJsonObject> m_blocks;
    QHash<int, QString> m_partialInput;
    bool m_gotMessageStop = false;
    bool m_streamError = false;
};
