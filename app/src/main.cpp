#include "ui/MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("DBaro");
    QApplication::setApplicationName("DBaro");
    QApplication::setApplicationVersion("0.1.0");

    // TODO: 엔진 프로세스를 QProcess로 띄우고 연결한다 (protocol/README.md 참고).
    MainWindow window;
    window.show();

    return app.exec();
}
