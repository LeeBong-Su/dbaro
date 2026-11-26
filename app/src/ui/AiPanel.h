#pragma once

#include "AiDbTools.h"

#include <QJsonObject>
#include <QWidget>

#include <functional>

class AiAgent;
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QTimer;
class QToolButton;

// AI 도우미 패널 (연결 창 오른쪽 도크). Claude와 대화하며 DB 도구(AiDbTools)로 구조를 읽고 읽기 전용 조회를 한다.
// 데이터를 바꾸는 SQL은 제안만 하고, 사용자가 쿼리 탭에서 열어 실행한다.
// 빠른 작업: 지금 에디터의 SQL 설명·성능 검토, 마지막 오류 고치기.
class AiPanel : public QWidget
{
    Q_OBJECT

public:
    struct Hooks
    {
        std::function<QString()> editorSql;  // 지금 탭에서 고른 글, 없으면 전체 (없으면 빈 값)
        std::function<QString()> lastError;  // 지금 탭에서 마지막으로 실패한 문장과 오류
        std::function<QString()> serverInfo; // "Oracle Database 23ai ..." (모르면 빈 값)
        std::function<void(const QString &sql, const QString &title)> openSql; // 새 쿼리 탭에 연다
    };

    AiPanel(const AiDbTools::Context &context, const Hooks &hooks, QWidget *parent = nullptr);

    void ask(const QString &question); // 입력란과 같은 경로 (빠른 작업, 테스트)
    void newChat();
    AiAgent *agent() const { return m_agent; }
    AiDbTools *tools() const { return m_tools; }
    QTextBrowser *transcript() const { return m_transcript; }
    QString transcriptMarkdown() const;
    void refreshBanner();

signals:
    void settingsRequested();
    // 이 연결로 Claude Code를 새 터미널에서 연다. prompt가 있으면 그 질문을 첫 메시지로 (Ask: Claude Code)
    void claudeCodeRequested(const QString &prompt);

private:
    struct Entry
    {
        enum Kind { User, Assistant, Thinking, Tool, Proposal, Notice, Error };
        Kind kind = Notice;
        QString text;
        QString title;   // Tool: 도구 이름, Proposal: 제목
        QString detail;  // Tool: 결과 요약, Proposal: 설명
        QString id;      // Tool: tool_use id
        bool error = false;
    };

    void send();
    void quickAction(const QString &kind);
    bool ensureReady();
    QString systemPrompt() const;
    void scheduleRender();
    void render();
    void onLink(const QUrl &url);
    void setRunning(bool running);
    bool askClaudeCode() const; // 질문을 Claude Code로 보내는지
    void updateTarget();
    QString render(const Entry &entry, QStringList *snippets) const;

    AiDbTools::Context m_context;
    Hooks m_hooks;
    AiAgent *m_agent = nullptr;
    AiDbTools *m_tools = nullptr;
    QList<Entry> m_entries;
    QStringList m_snippets; // 그린 SQL 조각 (링크 번호)
    bool m_newConversation = true;
    qint64 m_inputTokens = 0;
    qint64 m_outputTokens = 0;
    qint64 m_cacheRead = 0;

    QLabel *m_model = nullptr;
    QLabel *m_banner = nullptr;
    QTextBrowser *m_transcript = nullptr;
    QPlainTextEdit *m_input = nullptr;
    QCheckBox *m_includeSql = nullptr;
    QComboBox *m_target = nullptr; // Ask: AI Assistant(API 키) / Claude Code(구독, 새 터미널)
    QPushButton *m_send = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_renderTimer = nullptr;
    QList<QPushButton *> m_quick;
};
