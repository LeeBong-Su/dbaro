#pragma once

#include <QString>

#include <optional>

// 연결 비밀번호를 OS 보안 저장소에 둔다. 파일에는 절대 쓰지 않는다.
// Windows는 자격 증명 관리자, macOS는 로그인 키체인. Linux libsecret은 이후에 추가한다.
namespace SecretStore {
bool isSupported();
bool savePassword(const QString &connectionId, const QString &user, const QString &password,
                  QString *error = nullptr);
std::optional<QString> loadPassword(const QString &connectionId);
bool hasPassword(const QString &connectionId);
bool removePassword(const QString &connectionId);
} // namespace SecretStore
