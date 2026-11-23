#include "cli/CliApp.h"
#include "cli/CliConsole.h"

#include <QCoreApplication>

// dbaro-cli: 콘솔 프로그램. GUI(dbaro)와 같은 연결 목록·비밀번호·실행 기록을 쓴다.
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // GUI와 같은 앱 데이터 폴더(%APPDATA%\DBaro\DBaro)를 쓰도록 이름을 맞춘다.
    QCoreApplication::setOrganizationName("DBaro");
    QCoreApplication::setApplicationName("DBaro");
    QCoreApplication::setApplicationVersion("0.1.0");

    CliConsole::init();
    CliApp cli;
    return cli.run(QCoreApplication::arguments().mid(1));
}
