#pragma once

#include <QObject>
#include <QStringList>

class QLocalServer;

// DBaro는 사용자마다 한 프로세스만 띄운다 (Java 엔진 하나를 모든 창이 함께 쓰고,
// 작업 공간·실행 기록 파일을 한 곳에서만 쓰게). 두 번째로 실행하면(점프 목록, 시작 메뉴)
// 인자를 이미 떠 있는 DBaro에 넘기고 끝난다.
class SingleInstance : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstance(const QString &key, QObject *parent = nullptr);

    // 이미 실행 중인 DBaro가 있으면 arguments를 넘기고 true
    bool forwardToRunning(const QStringList &arguments);
    // 요청을 받기 시작한다
    bool listen();

signals:
    void argumentsReceived(const QStringList &arguments);

private:
    QString m_name;
    QLocalServer *m_server = nullptr;
};
