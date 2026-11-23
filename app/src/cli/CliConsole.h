#pragma once

#include <QString>

#include <functional>

// dbaro-cli의 콘솔 입출력. Windows 콘솔에서는 UTF-16 API(WriteConsoleW·ReadConsoleW)로 한글이 깨지지 않게 하고,
// 파이프·파일로 돌린 출력과 입력은 UTF-8로 주고받는다.
namespace CliConsole {

void init();

bool stdinIsConsole();
bool stdoutIsConsole();

void writeOut(const QString &text);
void writeErr(const QString &text);

// 한 줄 읽기 (줄 끝 제외). 입력이 끝나면(EOF, Ctrl+Z) false.
bool readLine(QString *line);
// 화면에 보이지 않게 한 줄 읽기 (비밀번호)
bool readHiddenLine(QString *line);
// 표준 입력 전체 (파이프로 넘긴 스크립트)
QString readAll();

// Ctrl+C: 콘솔 스레드에서 불린다. 프로그램을 끝내지 않고 handler를 부른다 (실행 중인 문장 취소).
void setInterruptHandler(std::function<void()> handler);

} // namespace CliConsole
