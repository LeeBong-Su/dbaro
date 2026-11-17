#include "EngineClient.h"

#include <QCborValue>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QtEndian>

#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

QString findJava()
{
    const QString explicitJava = qEnvironmentVariable("DBARO_JAVA");
    if (!explicitJava.isEmpty())
        return explicitJava;

#ifdef Q_OS_WIN
    const QString executable = QStringLiteral("java.exe");
#else
    const QString executable = QStringLiteral("java");
#endif
    const QString javaHome = qEnvironmentVariable("JAVA_HOME");
    if (!javaHome.isEmpty()) {
        const QString candidate = QDir(javaHome).filePath(QStringLiteral("bin/") + executable);
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return QStandardPaths::findExecutable(QStringLiteral("java"));
}

QString findEngineLib()
{
    const QString home = qEnvironmentVariable("DBARO_ENGINE_HOME");
    if (!home.isEmpty() && QFileInfo::exists(QDir(home).filePath(QStringLiteral("lib"))))
        return QDir(home).filePath(QStringLiteral("lib"));

    QDir dir(QCoreApplication::applicationDirPath());
    if (QFileInfo::exists(dir.filePath(QStringLiteral("engine/lib"))))
        return dir.filePath(QStringLiteral("engine/lib"));

    // 개발 중: 저장소의 engine/build/install/dbaro-engine/lib (./gradlew installDist)
    for (int level = 0; level < 8; ++level) {
        const QString candidate = dir.filePath(QStringLiteral("engine/build/install/dbaro-engine/lib"));
        if (QFileInfo::exists(candidate))
            return candidate;
        if (!dir.cdUp())
            break;
    }
    return {};
}

} // namespace

EngineError EngineError::fromCbor(const QCborMap &map)
{
    EngineError error;
    error.message = map.value(QStringLiteral("message")).toString();
    if (error.message.isEmpty())
        error.message = QStringLiteral("Unknown engine error");
    error.sqlState = map.value(QStringLiteral("sqlState")).toString();
    error.vendorCode = int(map.value(QStringLiteral("vendorCode")).toInteger());
    error.position = int(map.value(QStringLiteral("position")).toInteger(-1));
    error.line = int(map.value(QStringLiteral("line")).toInteger(-1));
    return error;
}

EngineError EngineError::of(const QString &message)
{
    EngineError error;
    error.message = message;
    return error;
}

EngineClient::EngineClient(QObject *parent)
    : QObject(parent)
{
}

EngineClient::~EngineClient()
{
    stop();
}

void EngineClient::setCommand(const QString &program, const QStringList &arguments)
{
    m_program = program;
    m_arguments = arguments;
    m_commandSet = true;
}

bool EngineClient::locateDefault(QString *program, QStringList *arguments, QString *error)
{
    const QString java = findJava();
    if (java.isEmpty()) {
        *error = tr("Java was not found. Install JDK 21 or set JAVA_HOME.");
        return false;
    }
    const QString lib = findEngineLib();
    if (lib.isEmpty()) {
        *error = tr("The DBaro engine was not found. Build it with \"gradlew installDist\" in the engine folder "
                    "or set DBARO_ENGINE_HOME.");
        return false;
    }

    const QString driverDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/drivers");
    *program = java;
    *arguments = {QStringLiteral("-cp"), QDir(lib).filePath(QStringLiteral("*")), QStringLiteral("dbaro.engine.MainKt"),
                  QStringLiteral("--driver-dir"), QDir::toNativeSeparators(driverDir)};
    return true;
}

bool EngineClient::isRunning() const
{
    return m_process != nullptr;
}

bool EngineClient::ensureStarted(QString *error)
{
    if (m_process)
        return true;

    if (!m_commandSet && !locateDefault(&m_program, &m_arguments, error))
        return false;

    auto *process = new QProcess(this);
#ifdef Q_OS_WIN
    // GUI 앱에서 콘솔 프로그램(java.exe)을 띄워도 검은 창이 뜨지 않게 한다.
    process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    connect(process, &QProcess::readyReadStandardOutput, this, &EngineClient::readFrames);
    connect(process, &QProcess::readyReadStandardError, this, &EngineClient::readLog);
    connect(process, &QProcess::finished, this, &EngineClient::onFinished);

    process->start(m_program, m_arguments);
    if (!process->waitForStarted(15000)) {
        *error = tr("Could not start the engine: %1").arg(process->errorString());
        delete process;
        return false;
    }

    m_process = process;
    m_buffer.clear();
    m_logBuffer.clear();
    return true;
}

void EngineClient::request(const QString &method, const QCborMap &params, Callback callback)
{
    QString error;
    if (!ensureStarted(&error)) {
        if (callback)
            QTimer::singleShot(0, this, [callback, error] { callback({}, EngineError::of(error)); });
        return;
    }

    const qint64 id = m_nextId++;
    QCborMap message;
    message.insert(QStringLiteral("id"), id);
    message.insert(QStringLiteral("method"), method);
    message.insert(QStringLiteral("params"), params);
    const QByteArray payload = QCborValue(message).toCbor();

    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(payload.size()), frame.data());
    frame.append(payload);

    if (callback)
        m_pending.insert(id, std::move(callback));
    m_process->write(frame);
}

void EngineClient::readFrames()
{
    if (!m_process)
        return;
    m_buffer.append(m_process->readAllStandardOutput());

    while (m_buffer.size() >= 4) {
        const quint32 length = qFromBigEndian<quint32>(m_buffer.constData());
        if (m_buffer.size() < qsizetype(4) + qsizetype(length))
            break;
        const QByteArray payload = m_buffer.mid(4, length);
        m_buffer.remove(0, 4 + qsizetype(length));

        const QCborMap message = QCborValue::fromCbor(payload).toMap();
        const qint64 id = message.value(QStringLiteral("id")).toInteger(-1);
        const Callback callback = m_pending.take(id);
        if (!callback)
            continue;
        if (message.contains(QStringLiteral("error")))
            callback({}, EngineError::fromCbor(message.value(QStringLiteral("error")).toMap()));
        else
            callback(message.value(QStringLiteral("result")).toMap(), {});
    }
}

void EngineClient::readLog()
{
    if (!m_process)
        return;
    m_logBuffer.append(m_process->readAllStandardError());
    qsizetype newline;
    while ((newline = m_logBuffer.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_logBuffer.left(newline)).trimmed();
        m_logBuffer.remove(0, newline + 1);
        if (!line.isEmpty())
            emit engineLog(line);
    }
}

void EngineClient::onFinished()
{
    QProcess *process = m_process;
    if (!process)
        return;
    const QString reason = tr("The engine stopped (exit code %1).").arg(process->exitCode());
    m_process = nullptr;
    process->deleteLater();
    failAll(reason);
    emit engineStopped(reason);
}

void EngineClient::stop()
{
    QProcess *process = m_process;
    if (!process)
        return;
    m_process = nullptr;
    process->disconnect(this);

    // 표준 입력을 닫으면 엔진이 세션을 정리하고 스스로 끝난다.
    process->closeWriteChannel();
    if (!process->waitForFinished(3000)) {
        process->kill();
        process->waitForFinished(1000);
    }
    delete process;
    const QString reason = tr("The engine was stopped.");
    failAll(reason);
    emit engineStopped(reason); // 세션들이 연결이 끊긴 것을 알게 한다
}

void EngineClient::failAll(const QString &reason)
{
    const QHash<qint64, Callback> pending = std::exchange(m_pending, {});
    for (const Callback &callback : pending)
        callback({}, EngineError::of(reason));
}
