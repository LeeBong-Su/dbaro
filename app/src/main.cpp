#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("DBaro");
    QApplication::setApplicationName("DBaro");
    QApplication::setApplicationVersion("0.1.0");

    // 창을 만들기 전에 테마를 적용한다 (기본: 다크 그레이).
    Theme::apply(Theme::load());

    // TODO: 엔진 프로세스를 QProcess로 띄우고 연결한다 (protocol/README.md 참고).
    MainWindow window;
    window.show();

    return app.exec();
}
