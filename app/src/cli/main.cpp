#include "cli/CliApp.h"
#include "cli/CliConsole.h"
#include "cli/McpServer.h"

#include <QCoreApplication>

// dbaro-cli: 콘솔 프로그램. GUI(dbaro)와 같은 연결 목록·비밀번호·실행 기록을 쓴다.
// dbaro-cli mcp: Claude Code·Claude Desktop이 부르는 MCP 서버 (McpServer)
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // GUI와 같은 앱 데이터 폴더(%APPDATA%\DBaro\DBaro)를 쓰도록 이름을 맞춘다.
    QCoreApplication::setOrganizationName("DBaro");
    QCoreApplication::setApplicationName("DBaro");
    QCoreApplication::setApplicationVersion("0.1.0");

    const QStringList arguments = QCoreApplication::arguments().mid(1);
    if (arguments.value(0) == u"mcp") {
        // 표준 출력은 프로토콜 메시지만. 콘솔 설정(Ctrl+C 처리 등)은 하지 않는다
        McpServer server;
        return server.run(arguments.mid(1));
    }
    CliConsole::init();
    CliApp cli;
    return cli.run(arguments);
}
