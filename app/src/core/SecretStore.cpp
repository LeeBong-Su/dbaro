#include "SecretStore.h"

#include <QtGlobal>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

#include <string>
#endif

namespace {
// 자격 증명 관리자에 보이는 이름: "DBaro/connection/<연결 id>"
QString targetName(const QString &connectionId)
{
    return QStringLiteral("DBaro/connection/%1").arg(connectionId);
}
} // namespace

bool SecretStore::isSupported()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool SecretStore::savePassword(const QString &connectionId, const QString &user, const QString &password,
                               QString *error)
{
#ifdef Q_OS_WIN
    const std::wstring target = targetName(connectionId).toStdWString();
    const std::wstring userName = user.toStdWString();

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.UserName = const_cast<LPWSTR>(userName.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(password.size() * sizeof(char16_t));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<QChar *>(password.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

    if (CredWriteW(&credential, 0))
        return true;
    if (error)
        *error = QStringLiteral("CredWriteW failed (error %1)").arg(GetLastError());
    return false;
#else
    Q_UNUSED(connectionId)
    Q_UNUSED(user)
    Q_UNUSED(password)
    if (error)
        *error = QStringLiteral("Saving passwords is not supported on this OS yet.");
    return false;
#endif
}

std::optional<QString> SecretStore::loadPassword(const QString &connectionId)
{
#ifdef Q_OS_WIN
    const std::wstring target = targetName(connectionId).toStdWString();
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return std::nullopt;

    const QString password = QString::fromUtf16(reinterpret_cast<const char16_t *>(credential->CredentialBlob),
                                                credential->CredentialBlobSize / sizeof(char16_t));
    CredFree(credential);
    return password;
#else
    Q_UNUSED(connectionId)
    return std::nullopt;
#endif
}

bool SecretStore::hasPassword(const QString &connectionId)
{
#ifdef Q_OS_WIN
    const std::wstring target = targetName(connectionId).toStdWString();
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return false;
    CredFree(credential);
    return true;
#else
    Q_UNUSED(connectionId)
    return false;
#endif
}

bool SecretStore::removePassword(const QString &connectionId)
{
#ifdef Q_OS_WIN
    const std::wstring target = targetName(connectionId).toStdWString();
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0))
        return true;
    return GetLastError() == ERROR_NOT_FOUND; // 원래 없으면 지운 것과 같다
#else
    Q_UNUSED(connectionId)
    return true;
#endif
}
