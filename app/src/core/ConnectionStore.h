#pragma once

#include "ConnectionProfile.h"

#include <QList>
#include <QString>

// 연결 목록을 JSON 파일로 저장하고 불러온다. 비밀번호는 저장하지 않는다 (SecretStore 담당).
// 기본 위치는 OS의 앱 데이터 폴더 (Windows: %APPDATA%\DBaro\DBaro\connections.json).
class ConnectionStore
{
public:
    ConnectionStore();
    explicit ConnectionStore(const QString &filePath);

    QString filePath() const { return m_filePath; }

    const QList<ConnectionProfile> &profiles() const { return m_profiles; }
    const ConnectionProfile *find(const QString &id) const;
    void setProfiles(const QList<ConnectionProfile> &profiles);

    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr) const;

private:
    QString m_filePath;
    QList<ConnectionProfile> m_profiles;
};
