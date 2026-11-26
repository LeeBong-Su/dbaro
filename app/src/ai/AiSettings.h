#pragma once

#include "ClaudeClient.h"

#include <QList>
#include <QPair>
#include <QString>

// AI 도우미 설정. 값은 QSettings("ai/..."), API 키는 OS 보안 저장소(자격 증명 관리자)에 둔다.
// API 키가 저장돼 있지 않으면 환경 변수 ANTHROPIC_API_KEY를 쓴다.
struct AiSettings
{
    QString model = QStringLiteral("claude-opus-5-5");
    QString effort = QStringLiteral("medium");
    QString baseUrl = QStringLiteral("https://api.anthropic.com");
    bool sendRows = true;        // 조회 결과 값을 모델에 보낸다 (끄면 컬럼 이름과 행 수만)
    int maxRows = 100;           // AI가 실행한 조회에서 가져올 최대 행 수
    bool askBeforeQuery = false; // 조회를 실행할 때마다 묻는다 (운영 DB는 늘 묻는다)
    bool consented = false;      // 무엇이 전송되는지 안내를 보고 동의했다

    static AiSettings load();
    void save() const;

    static QString apiKey(); // 저장된 키 → ANTHROPIC_API_KEY → 빈 값
    static bool hasStoredApiKey();
    static bool saveApiKey(const QString &key, QString *error = nullptr);
    static void removeApiKey();

    static QList<QPair<QString, QString>> models(); // (id, 이름)
    static QString modelName(const QString &id); // 화면에 보일 이름
    static QStringList efforts();

    ClaudeConfig config() const; // 키 포함
};
