#include <QApplication>
#include <QMainWindow>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("DBaro");
    QApplication::setApplicationVersion("0.1.0");

    // TODO: 엔진 프로세스를 QProcess로 띄우고 연결한다 (protocol/README.md 참고).
    QMainWindow window;
    window.setWindowTitle("DBaro");
    window.resize(1280, 800);
    window.show();

    return app.exec();
}
