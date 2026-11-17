#pragma once

#include <QCborMap>
#include <QHash>
#include <QObject>
#include <QStringList>

#include <functional>

class QProcess;

// 엔진이 돌려준 오류. position/line은 실행한 문장 안의 위치다 (없으면 -1).
struct EngineError
{
    QString message;
    QString sqlState;
    int vendorCode = 0;
    int position = -1; // 0부터 시작하는 글자 위치
    int line = -1;     // 1부터 시작하는 줄

    bool isError() const { return !message.isEmpty(); }
    static EngineError fromCbor(const QCborMap &map);
    static EngineError of(const QString &message);
};

// 엔진 프로세스(Kotlin/JVM)를 띄우고 표준 입출력으로 요청·응답을 주고받는다 (protocol/README.md).
// 처음 요청할 때 엔진을 띄우고, 엔진이 죽으면 기다리던 요청을 모두 오류로 끝낸 뒤 다음 요청 때 다시 띄운다.
class EngineClient : public QObject
{
    Q_OBJECT

public:
    using Callback = std::function<void(const QCborMap &result, const EngineError &error)>;

    explicit EngineClient(QObject *parent = nullptr);
    ~EngineClient() override;

    // 기본 위치를 찾지 않고 실행 명령을 직접 정한다.
    void setCommand(const QString &program, const QStringList &arguments);

    // Java: DBARO_JAVA → 앱 옆 runtime/ → JAVA_HOME → PATH.
    // 엔진: DBARO_ENGINE_HOME → 앱 옆 engine/ → 저장소의 installDist 결과.
    static bool locateDefault(QString *program, QStringList *arguments, QString *error);

    bool isRunning() const;
    void request(const QString &method, const QCborMap &params, Callback callback = {});
    void stop();

signals:
    void engineLog(const QString &line);
    void engineStopped(const QString &reason);

private:
    bool ensureStarted(QString *error);
    void readFrames();
    void readLog();
    void onFinished();
    void failAll(const QString &reason);

    QProcess *m_process = nullptr;
    QString m_program;
    QStringList m_arguments;
    bool m_commandSet = false;
    QByteArray m_buffer;
    QByteArray m_logBuffer;
    qint64 m_nextId = 1;
    QHash<qint64, Callback> m_pending;
};
