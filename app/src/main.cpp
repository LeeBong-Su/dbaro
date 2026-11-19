#include "ui/AppController.h"
#include "ui/Icons.h"
#include "ui/SingleInstance.h"
#include "ui/Theme.h"
#include "ui/WindowsIntegration.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("DBaro");
    QApplication::setApplicationName("DBaro");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setWindowIcon(Icons::get(QStringLiteral("dbaro")));

    // 작업 표시줄이 모든 DBaro 창과 점프 목록을 한 앱으로 묶게 한다 (창을 만들기 전에).
    WindowsIntegration::setAppUserModelId();

    // DBaro는 사용자마다 하나만 띄운다. 이미 떠 있으면 인자(--connection <id> 등)를 넘기고 끝낸다.
    const QStringList arguments = QApplication::arguments().mid(1);
    SingleInstance instance(QStringLiteral("DBaro"));
    if (instance.forwardToRunning(arguments))
        return 0;
    instance.listen();

    // 창을 만들기 전에 테마를 적용한다 (기본: 다크 그레이).
    Theme::apply(Theme::load());

    // 연결 창을 모두 닫아도 연결 선택 창으로 돌아가므로, 끝내는 때는 AppController가 정한다.
    QApplication::setQuitOnLastWindowClosed(false);
    AppController controller;
    QObject::connect(&instance, &SingleInstance::argumentsReceived, &controller, &AppController::handleArguments);
    controller.start(arguments);

    return app.exec();
}
