#include "ClaudeClient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kMaxRetries = 2;
constexpr int kInactivityTimeoutMs = 5 * 60 * 1000; // 이만큼 아무것도 오지 않으면 끊는다

bool retryableStatus(int status)
{
    return status == 408 || status == 409 || status == 429 || status == 500 || status == 502 || status == 503
           || status == 504 || status == 529;
}
} // namespace

bool ClaudeConfig::isOfficialEndpoint() const
{
    return QUrl(baseUrl).host().compare(QLatin1String("api.anthropic.com"), Qt::CaseInsensitive) == 0;
}

ClaudeClient::ClaudeClient(QObject *parent)
    : QObject(parent)
{
    static const bool proxies = [] {
        QNetworkProxyFactory::setUseSystemConfiguration(true); // 회사 프록시 (PAC·시스템 설정)
        return true;
    }();
    Q_UNUSED(proxies);
    m_network = new QNetworkAccessManager(this);
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, [this] {
        m_retryPending = false;
        start();
    });
}

ClaudeClient::~ClaudeClient()
{
    abort();
}

void ClaudeClient::send(const ClaudeConfig &config, const QJsonObject &body, const QStringList &betas)
{
    abort();
    m_config = config;
    QJsonObject request = body;
    request.insert(QStringLiteral("stream"), true);
    m_payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    m_betas = betas;
    m_attempt = 0;
    start();
}

void ClaudeClient::abort()
{
    m_retryTimer->stop();
    m_retryPending = false;
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

void ClaudeClient::start()
{
    ++m_attempt;
    m_buffer.clear();
    m_message = QJsonObject();
    m_blocks.clear();
    m_partialInput.clear();
    m_gotMessageStop = false;
    m_streamError = false;

    QString base = m_config.baseUrl.trimmed();
    while (base.endsWith(u'/'))
        base.chop(1);
    QNetworkRequest request(QUrl(base + QStringLiteral("/v1/messages")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("x-api-key", m_config.apiKey.toUtf8());
    request.setRawHeader("anthropic-version", apiVersion().toUtf8());
    request.setRawHeader("accept", "text/event-stream");
    if (!m_betas.isEmpty())
        request.setRawHeader("anthropic-beta", m_betas.join(u',').toUtf8());
    request.setTransferTimeout(kInactivityTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_reply = m_network->post(request, m_payload);
    connect(m_reply, &QNetworkReply::readyRead, this, &ClaudeClient::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &ClaudeClient::onFinished);
}

void ClaudeClient::onReadyRead()
{
    if (!m_reply)
        return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200)
        return; // 오류 본문은 끝나고 한 번에 읽는다
    m_buffer += m_reply->readAll();
    parseBuffer();
}

void ClaudeClient::parseBuffer()
{
    // SSE: 빈 줄로 나뉜 이벤트. "event: 이름" + "data: JSON"
    while (!m_retryPending) {
        qsizetype end = m_buffer.indexOf("\n\n");
        qsizetype skip = 2;
        const qsizetype crlf = m_buffer.indexOf("\r\n\r\n");
        if (crlf >= 0 && (end < 0 || crlf < end)) {
            end = crlf;
            skip = 4;
        }
        if (end < 0)
            break;
        const QByteArray frame = m_buffer.left(end);
        m_buffer.remove(0, end + skip);
        QString event;
        QByteArray data;
        for (QByteArray line : frame.split('\n')) {
            if (line.endsWith('\r'))
                line.chop(1);
            if (line.startsWith("event:"))
                event = QString::fromUtf8(line.mid(6).trimmed());
            else if (line.startsWith("data:"))
                data += line.mid(5).trimmed();
        }
        if (!data.isEmpty())
            handleEvent(event, data);
    }
}

void ClaudeClient::handleEvent(const QString &eventName, const QByteArray &data)
{
    const QJsonObject event = QJsonDocument::fromJson(data).object();
    const QString type = event.value(QStringLiteral("type")).toString(eventName);
    if (type == u"message_start") {
        m_message = event.value(QStringLiteral("message")).toObject();
        m_message.remove(QStringLiteral("content"));
    } else if (type == u"content_block_start") {
        const int index = event.value(QStringLiteral("index")).toInt();
        QJsonObject block = event.value(QStringLiteral("content_block")).toObject();
        while (m_blocks.size() <= index)
            m_blocks.append(QJsonObject());
        m_blocks[index] = block;
        const QString blockType = block.value(QStringLiteral("type")).toString();
        if (blockType == u"tool_use")
            m_partialInput.insert(index, QString());
        emit blockStarted(index, blockType, block);
    } else if (type == u"content_block_delta") {
        const int index = event.value(QStringLiteral("index")).toInt();
        if (index < 0 || index >= m_blocks.size())
            return;
        const QJsonObject delta = event.value(QStringLiteral("delta")).toObject();
        const QString deltaType = delta.value(QStringLiteral("type")).toString();
        QJsonObject &block = m_blocks[index];
        if (deltaType == u"text_delta") {
            const QString text = delta.value(QStringLiteral("text")).toString();
            block.insert(QStringLiteral("text"), block.value(QStringLiteral("text")).toString() + text);
            emit textDelta(index, text);
        } else if (deltaType == u"thinking_delta") {
            const QString text = delta.value(QStringLiteral("thinking")).toString();
            block.insert(QStringLiteral("thinking"), block.value(QStringLiteral("thinking")).toString() + text);
            emit thinkingDelta(index, text);
        } else if (deltaType == u"signature_delta") {
            block.insert(QStringLiteral("signature"),
                         block.value(QStringLiteral("signature")).toString() + delta.value(QStringLiteral("signature")).toString());
        } else if (deltaType == u"input_json_delta") {
            const QString part = delta.value(QStringLiteral("partial_json")).toString();
            m_partialInput[index] += part;
            emit toolInputDelta(index, part);
        }
        // 그 밖의 델타(citations 등)는 쓰지 않는다
    } else if (type == u"content_block_stop") {
        const int index = event.value(QStringLiteral("index")).toInt();
        if (index < 0 || index >= m_blocks.size() || !m_partialInput.contains(index))
            return;
        // eager input streaming: 서버가 검사하지 않으니 엄격하게 파싱한다
        const QString raw = m_partialInput.take(index);
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(raw.isEmpty() ? QByteArrayLiteral("{}") : raw.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && doc.isObject()) {
            m_blocks[index].insert(QStringLiteral("input"), doc.object());
        } else {
            m_blocks[index].insert(QStringLiteral("input"), QJsonObject());
            m_blocks[index].insert(QStringLiteral("_invalidInput"), raw);
        }
    } else if (type == u"message_delta") {
        const QJsonObject delta = event.value(QStringLiteral("delta")).toObject();
        for (auto it = delta.begin(); it != delta.end(); ++it)
            m_message.insert(it.key(), it.value());
        QJsonObject usage = m_message.value(QStringLiteral("usage")).toObject();
        const QJsonObject more = event.value(QStringLiteral("usage")).toObject();
        for (auto it = more.begin(); it != more.end(); ++it)
            usage.insert(it.key(), it.value());
        m_message.insert(QStringLiteral("usage"), usage);
    } else if (type == u"message_stop") {
        m_gotMessageStop = true;
    } else if (type == u"error") {
        m_streamError = true;
        const QJsonObject error = event.value(QStringLiteral("error")).toObject();
        const QString errorType = error.value(QStringLiteral("type")).toString();
        const QString text = error.value(QStringLiteral("message")).toString(QString::fromUtf8(data));
        // 응답을 시작하기 전의 과부하는 다시 시도한다
        if ((errorType == u"overloaded_error" || errorType == u"api_error") && m_blocks.isEmpty() && m_attempt <= kMaxRetries) {
            scheduleRetry(text, 0);
            return;
        }
        emit failed(QStringLiteral("%1: %2").arg(errorType, text), 200);
    }
    // ping 등은 무시
}

void ClaudeClient::onFinished()
{
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    m_reply = nullptr;
    reply->deleteLater();
    if (m_retryPending)
        return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 200) {
        // 남은 조각 처리
        m_buffer += reply->readAll();
        if (!m_buffer.isEmpty()) {
            m_buffer += "\n\n";
            parseBuffer();
        }
        if (m_retryPending)
            return;
        if (m_streamError)
            return; // failed를 이미 보냈다
        if (!m_gotMessageStop) {
            if (m_blocks.isEmpty() && m_attempt <= kMaxRetries) {
                scheduleRetry(reply->errorString(), 0);
                return;
            }
            emit failed(tr("The response stopped before it was complete (%1).").arg(reply->errorString()), status);
            return;
        }
        QJsonArray content;
        for (const QJsonObject &block : std::as_const(m_blocks)) {
            if (!block.isEmpty())
                content.append(block);
        }
        QJsonObject message = m_message;
        message.insert(QStringLiteral("content"), content);
        emit finished(message);
        return;
    }

    const QByteArray body = reply->readAll();
    QString text = QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
    if (text.isEmpty())
        text = reply->errorString();
    const bool networkError = status == 0 && reply->error() != QNetworkReply::OperationCanceledError;
    if ((retryableStatus(status) || networkError) && m_attempt <= kMaxRetries) {
        int retryAfter = 0;
        const QByteArray header = reply->rawHeader("retry-after");
        if (!header.isEmpty())
            retryAfter = int(header.toDouble() * 1000);
        scheduleRetry(text, retryAfter);
        return;
    }
    if (status == 401)
        text = tr("The API key was not accepted (401). Check it in the AI Assistant settings.");
    emit failed(text, status);
}

void ClaudeClient::scheduleRetry(const QString &reason, int retryAfterMs)
{
    m_buffer.clear();
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    const int delay = retryAfterMs > 0 ? std::min(retryAfterMs, 60000) : 1000 * (1 << (m_attempt - 1));
    m_retryPending = true;
    emit retrying(m_attempt, delay, reason);
    m_retryTimer->start(delay);
}
