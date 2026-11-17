#include "ConnectionStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
constexpr int kFormatVersion = 1;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}
} // namespace

ConnectionStore::ConnectionStore()
    : ConnectionStore(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                      + QStringLiteral("/connections.json"))
{
}

ConnectionStore::ConnectionStore(const QString &filePath)
    : m_filePath(filePath)
{
}

const ConnectionProfile *ConnectionStore::find(const QString &id) const
{
    if (id.isEmpty())
        return nullptr;
    for (const ConnectionProfile &profile : m_profiles) {
        if (profile.id == id)
            return &profile;
    }
    return nullptr;
}

void ConnectionStore::setProfiles(const QList<ConnectionProfile> &profiles)
{
    m_profiles = profiles;
}

bool ConnectionStore::load(QString *error)
{
    m_profiles.clear();

    QFile file(m_filePath);
    if (!file.exists())
        return true; // 처음 실행: 연결 없음

    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, parseError.errorString());
        return false;
    }

    const QJsonArray connections = document.object().value("connections").toArray();
    for (const QJsonValue &value : connections) {
        if (value.isObject())
            m_profiles.append(ConnectionProfile::fromJson(value.toObject()));
    }
    return true;
}

bool ConnectionStore::save(QString *error) const
{
    const QFileInfo info(m_filePath);
    if (!QDir().mkpath(info.absolutePath())) {
        setError(error, QStringLiteral("Cannot create folder: %1").arg(info.absolutePath()));
        return false;
    }

    QJsonArray connections;
    for (const ConnectionProfile &profile : m_profiles)
        connections.append(profile.toJson());

    const QJsonObject root{
        {"version", kFormatVersion},
        {"connections", connections},
    };

    // 쓰는 도중에 실패해도 기존 파일이 깨지지 않도록 임시 파일에 쓴 뒤 교체한다.
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    return true;
}
