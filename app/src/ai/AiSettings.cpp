#include "AiSettings.h"

#include "core/SecretStore.h"

#include <QSettings>

namespace {
// 자격 증명 관리자 항목: "DBaro/connection/ai-anthropic" (연결 비밀번호와 같은 저장소 API를 쓴다)
const auto kSecretId = QStringLiteral("ai-anthropic");
} // namespace

AiSettings AiSettings::load()
{
    QSettings s;
    AiSettings a;
    a.model = s.value(QStringLiteral("ai/model"), a.model).toString();
    a.effort = s.value(QStringLiteral("ai/effort"), a.effort).toString();
    a.baseUrl = s.value(QStringLiteral("ai/baseUrl"), a.baseUrl).toString();
    a.sendRows = s.value(QStringLiteral("ai/sendRows"), a.sendRows).toBool();
    a.maxRows = qBound(1, s.value(QStringLiteral("ai/maxRows"), a.maxRows).toInt(), 1000);
    a.askBeforeQuery = s.value(QStringLiteral("ai/askBeforeQuery"), a.askBeforeQuery).toBool();
    a.consented = s.value(QStringLiteral("ai/consented"), false).toBool();
    return a;
}

void AiSettings::save() const
{
    QSettings s;
    s.setValue(QStringLiteral("ai/model"), model);
    s.setValue(QStringLiteral("ai/effort"), effort);
    s.setValue(QStringLiteral("ai/baseUrl"), baseUrl);
    s.setValue(QStringLiteral("ai/sendRows"), sendRows);
    s.setValue(QStringLiteral("ai/maxRows"), maxRows);
    s.setValue(QStringLiteral("ai/askBeforeQuery"), askBeforeQuery);
    s.setValue(QStringLiteral("ai/consented"), consented);
}

QString AiSettings::apiKey()
{
    if (SecretStore::isSupported()) {
        if (const std::optional<QString> stored = SecretStore::loadPassword(kSecretId); stored && !stored->isEmpty())
            return *stored;
    }
    return qEnvironmentVariable("ANTHROPIC_API_KEY");
}

bool AiSettings::hasStoredApiKey()
{
    return SecretStore::isSupported() && SecretStore::hasPassword(kSecretId);
}

bool AiSettings::saveApiKey(const QString &key, QString *error)
{
    if (!SecretStore::isSupported()) {
        if (error)
            *error = QStringLiteral("No secure storage on this system. Set ANTHROPIC_API_KEY instead.");
        return false;
    }
    return SecretStore::savePassword(kSecretId, QStringLiteral("anthropic-api-key"), key.trimmed(), error);
}

void AiSettings::removeApiKey()
{
    if (SecretStore::isSupported())
        SecretStore::removePassword(kSecretId);
}

QList<QPair<QString, QString>> AiSettings::models()
{
    return {
        {QStringLiteral("claude-opus-5-5"), QStringLiteral("Claude Opus 5.5 (recommended)")},
        {QStringLiteral("claude-sonnet-5-5"), QStringLiteral("Claude Sonnet 5.5 (faster, cheaper)")},
        {QStringLiteral("claude-haiku-5-5"), QStringLiteral("Claude Haiku 5.5 (fastest, cheapest)")},
    };
}

QString AiSettings::modelName(const QString &id)
{
    // "Claude Opus 5.5 (recommended)" → "Claude Opus 5.5". 모르는 id(게이트웨이 모델 등)는 그대로
    for (const auto &[model, name] : models()) {
        if (model == id)
            return name.section(QStringLiteral(" ("), 0, 0);
    }
    return id;
}

QStringList AiSettings::efforts()
{
    return {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh"), QStringLiteral("max")};
}

ClaudeConfig AiSettings::config() const
{
    ClaudeConfig c;
    c.apiKey = apiKey();
    c.baseUrl = baseUrl.trimmed().isEmpty() ? QStringLiteral("https://api.anthropic.com") : baseUrl.trimmed();
    c.model = model;
    c.effort = effort;
    return c;
}
