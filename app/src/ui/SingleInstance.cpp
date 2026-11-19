#include "SingleInstance.h"

#include "WindowsIntegration.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>

SingleInstance::SingleInstance(const QString &key, QObject *parent)
    : QObject(parent)
{
    // 사용자마다 다른 이름 (같은 PC의 다른 사용자와 섞이지 않게)
    const QByteArray user = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER")).toUtf8()
                            + QDir::homePath().toUtf8();
    m_name = key + u'-' + QString::fromLatin1(QCryptographicHash::hash(user, QCryptographicHash::Sha1).toHex().left(16));
}

bool SingleInstance::forwardToRunning(const QStringList &arguments)
{
    QLocalSocket socket;
    socket.connectToServer(m_name);
    if (!socket.waitForConnected(500))
        return false;
    WindowsIntegration::allowForegroundForOtherProcess(); // 받는 쪽이 창을 앞으로 가져올 수 있게
    socket.write(QJsonDocument(QJsonArray::fromStringList(arguments)).toJson(QJsonDocument::Compact) + '\n');
    socket.waitForBytesWritten(2000);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.waitForDisconnected(1000);
    return true;
}

bool SingleInstance::listen()
{
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_name)) {
        // 비정상 종료로 남은 소켓 파일 (Unix)
        QLocalServer::removeServer(m_name);
        if (!m_server->listen(m_name))
            return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server->nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                if (!socket->canReadLine())
                    return;
                const QJsonDocument document = QJsonDocument::fromJson(socket->readLine());
                QStringList arguments;
                for (const QJsonValue &value : document.array())
                    arguments << value.toString();
                emit argumentsReceived(arguments);
            });
        }
    });
    return true;
}
