#include "CliConsole.h"

#include <QByteArray>

#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

std::function<void()> g_interrupt;

#ifdef Q_OS_WIN
bool isConsole(DWORD which)
{
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(which), &mode) != 0;
}

void write(DWORD which, const QString &text)
{
    HANDLE handle = GetStdHandle(which);
    if (isConsole(which)) {
        const std::wstring wide = text.toStdWString();
        DWORD written = 0;
        WriteConsoleW(handle, wide.data(), DWORD(wide.size()), &written, nullptr);
        return;
    }
    const QByteArray utf8 = text.toUtf8();
    DWORD written = 0;
    WriteFile(handle, utf8.constData(), DWORD(utf8.size()), &written, nullptr);
}

BOOL WINAPI onConsoleControl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (g_interrupt)
            g_interrupt();
        return TRUE; // 끝내지 않는다
    }
    return FALSE;
}

bool readConsoleLine(QString *line)
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring text;
    wchar_t buffer[1024];
    for (;;) {
        DWORD read = 0;
        if (!ReadConsoleW(input, buffer, DWORD(std::size(buffer)), &read, nullptr))
            return false;
        if (read == 0) {
            // Ctrl+C로 읽기가 끊겼다: 빈 줄로 돌려준다
            if (GetLastError() == ERROR_OPERATION_ABORTED)
                break;
            return false;
        }
        text.append(buffer, read);
        if (!text.empty() && text.back() == L'\n')
            break;
    }
    // Ctrl+Z (EOF)
    if (!text.empty() && text.front() == 0x1A)
        return false;
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r'))
        text.pop_back();
    *line = QString::fromStdWString(text);
    return true;
}
#endif

bool readUtf8Line(QString *line)
{
    std::string text;
    if (!std::getline(std::cin, text))
        return false;
    if (!text.empty() && text.back() == '\r')
        text.pop_back();
    *line = QString::fromUtf8(text.data(), qsizetype(text.size()));
    return true;
}

} // namespace

void CliConsole::init()
{
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
#else
    std::signal(SIGINT, [](int) {
        if (g_interrupt)
            g_interrupt();
    });
#endif
}

bool CliConsole::stdinIsConsole()
{
#ifdef Q_OS_WIN
    return isConsole(STD_INPUT_HANDLE);
#else
    return isatty(STDIN_FILENO);
#endif
}

bool CliConsole::stdoutIsConsole()
{
#ifdef Q_OS_WIN
    return isConsole(STD_OUTPUT_HANDLE);
#else
    return isatty(STDOUT_FILENO);
#endif
}

void CliConsole::writeOut(const QString &text)
{
#ifdef Q_OS_WIN
    write(STD_OUTPUT_HANDLE, text);
#else
    const QByteArray utf8 = text.toUtf8();
    std::fwrite(utf8.constData(), 1, size_t(utf8.size()), stdout);
    std::fflush(stdout);
#endif
}

void CliConsole::writeErr(const QString &text)
{
#ifdef Q_OS_WIN
    write(STD_ERROR_HANDLE, text);
#else
    const QByteArray utf8 = text.toUtf8();
    std::fwrite(utf8.constData(), 1, size_t(utf8.size()), stderr);
    std::fflush(stderr);
#endif
}

bool CliConsole::readLine(QString *line)
{
#ifdef Q_OS_WIN
    if (stdinIsConsole())
        return readConsoleLine(line);
#endif
    return readUtf8Line(line);
}

bool CliConsole::readHiddenLine(QString *line)
{
#ifdef Q_OS_WIN
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = GetConsoleMode(input, &mode) != 0;
    if (console)
        SetConsoleMode(input, mode & ~DWORD(ENABLE_ECHO_INPUT));
    const bool ok = readLine(line);
    if (console) {
        SetConsoleMode(input, mode);
        writeErr(QStringLiteral("\n"));
    }
    return ok;
#else
    termios old {};
    const bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        termios hidden = old;
        hidden.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &hidden);
    }
    const bool ok = readLine(line);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        writeErr(QStringLiteral("\n"));
    }
    return ok;
#endif
}

QString CliConsole::readAll()
{
    std::string all;
    char buffer[65536];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof buffer, stdin)) > 0)
        all.append(buffer, read);
    QString text = QString::fromUtf8(all.data(), qsizetype(all.size()));
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    return text;
}

void CliConsole::setInterruptHandler(std::function<void()> handler)
{
    g_interrupt = std::move(handler);
}
