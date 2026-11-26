#pragma once

#include "core/ConnectionProfile.h"

#include <QString>
#include <QStringList>

class QWidget;

// Database → Open Claude Code (Ctrl+Alt+C): 사용자의 Claude Code(claude)를 새 터미널 창에서 연다.
// DBaro는 실행만 해 주고 그 뒤로는 관여하지 않는다 (입력·출력을 읽지 않는다. 로그인은 사용자의 것).
// - 지금 연결·스키마·DB 버전을 --append-system-prompt로 알려, 연결 이름을 말하지 않아도 dbaro 도구를 그 연결로 쓰게 한다
// - dbaro MCP 서버가 Claude Code에 등록돼 있지 않으면 이 세션에만 --mcp-config로 붙인다
// - 작업 폴더는 그 DBMS의 스크립트 폴더 (없으면 %USERPROFILE%\DBaro). Claude가 그 폴더의 SQL 파일을 읽고 고칠 수 있다
// - Windows는 새 콘솔로 실행한다 (Windows 11은 기본 터미널, 보통 Windows Terminal에 뜬다)
namespace ClaudeCodeLauncher {

struct Context
{
    ConnectionProfile profile;
    QString serverInfo; // "Oracle Database 23ai ..." (모르면 빈 값)
    QString schema;     // 지금 쿼리 탭의 스키마
    QString database;   // PostgreSQL에서 탭이 고른 DB (기본 DB면 빈 값)
};

QString claudePath();         // claude 실행 파일 (없으면 빈 값)
bool isDbaroRegistered();     // Claude Code 사용자 설정(~/.claude.json)에 dbaro MCP 서버가 있는지
QString workingFolder(const Context &context);
QString contextPrompt(const Context &context, const QString &folder);
// claude에 넘길 인자. mcpConfigPath가 있으면 --mcp-config로, prompt가 있으면 첫 질문으로 붙인다
QStringList arguments(const Context &context, const QString &folder, const QString &mcpConfigPath, const QString &prompt = {});

// 연다. prompt가 있으면 그 질문을 바로 보낸 채로 (너무 길면 클립보드에 넣고 note로 알린다). 안 되면 parent에 알리고 false
bool launch(QWidget *parent, const Context &context, QString *note = nullptr, const QString &prompt = {});

} // namespace ClaudeCodeLauncher
