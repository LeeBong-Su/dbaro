#pragma once

#include <QDialog>
#include <QStringList>

#include <functional>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

// Database → Use with Claude Code...: DBaro의 DB 도구를 사용자의 Claude Code·Claude Desktop에 MCP 서버로 등록하는 안내.
// API 키 없이 사용자의 Claude 로그인(구독)으로 쓴다. DBaro는 Claude를 부르지 않고 도구만 낸다 (dbaro-cli mcp).
// - Claude Code: 등록 명령(복사), claude가 있으면 바로 등록(이미 있으면 바꿀지 묻는다)
// - Claude Desktop: claude_desktop_config.json에 넣을 내용(복사)
// - 옵션(운영 DB 조회 허용, 값 보내지 않기, 행 수)은 명령 인자가 된다. 설정 "claudeCode/..."에 기억한다
class ClaudeCodeDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ClaudeCodeDialog(QWidget *parent = nullptr);

    QStringList serverArguments() const; // mcp [옵션...]
    static QStringList serverArgumentsFor(bool allowProduction, bool noRows, int maxRows);
    static QStringList savedServerArguments(); // 이 창에서 마지막으로 고른 옵션
    QString claudeCodeCommand() const;
    QString desktopConfig() const;
    static QString cliPath();

private:
    void updateTexts();
    void addToClaudeCode();
    void runClaude(const QStringList &arguments, std::function<void(int exitCode, const QString &output)> done);

    QCheckBox *m_production = nullptr;
    QCheckBox *m_noRows = nullptr;
    QSpinBox *m_maxRows = nullptr;
    QPlainTextEdit *m_command = nullptr;
    QPlainTextEdit *m_desktop = nullptr;
    QPushButton *m_add = nullptr;
    QLabel *m_status = nullptr;
    QString m_claude; // claude 실행 파일 (없으면 빈 값)
};
