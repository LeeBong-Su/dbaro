#include "SecretStore.h"

#include <QtGlobal>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

#include <string>
#elif defined(Q_OS_MACOS)
#include <QByteArray>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace {
// 자격 증명 관리자·키체인에 보이는 이름: "DBaro/connection/<연결 id>"
QString targetName(const QString &connectionId)
{
    return QStringLiteral("DBaro/connection/%1").arg(connectionId);
}

#if defined(Q_OS_MACOS)
// Core Foundation 객체를 범위가 끝날 때 놓는다.
template <typename T>
struct CfRef {
    T ref = nullptr;
    explicit CfRef(T value) : ref(value) {}
    ~CfRef()
    {
        if (ref)
            CFRelease(ref);
    }
    CfRef(const CfRef &) = delete;
    CfRef &operator=(const CfRef &) = delete;
};

CFMutableDictionaryRef newDictionary()
{
    return CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
}

// 로그인 키체인의 일반 암호 항목 하나: 서비스 "DBaro", 계정 "DBaro/connection/<id>"
CFMutableDictionaryRef newKeychainQuery(const QString &connectionId)
{
    CFMutableDictionaryRef query = newDictionary();
    const CfRef<CFStringRef> service(QStringLiteral("DBaro").toCFString());
    const CfRef<CFStringRef> account(targetName(connectionId).toCFString());
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service.ref);
    CFDictionarySetValue(query, kSecAttrAccount, account.ref);
    return query;
}

QString statusText(OSStatus status)
{
    const CfRef<CFStringRef> text(SecCopyErrorMessageString(status, nullptr));
    return text.ref ? QString::fromCFString(text.ref) : QStringLiteral("error %1").arg(status);
}
#endif
} // namespace

bool SecretStore::isSupported()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
}

bool SecretStore::savePassword(const QString &connectionId, const QString &user, const QString &password,
                               QString *error)
{
#if defined(Q_OS_WIN)
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
#elif defined(Q_OS_MACOS)
    Q_UNUSED(user)
    const CfRef<CFMutableDictionaryRef> query(newKeychainQuery(connectionId));
    const CfRef<CFDataRef> data(password.toUtf8().toCFData());

    // 이미 있으면 값만 바꾸고, 없으면 새로 넣는다.
    const CfRef<CFMutableDictionaryRef> changes(newDictionary());
    CFDictionarySetValue(changes.ref, kSecValueData, data.ref);
    OSStatus status = SecItemUpdate(query.ref, changes.ref);
    if (status == errSecItemNotFound) {
        const CfRef<CFStringRef> label(targetName(connectionId).toCFString()); // 키체인 접근 앱에 보이는 이름
        CFDictionarySetValue(query.ref, kSecAttrLabel, label.ref);
        CFDictionarySetValue(query.ref, kSecValueData, data.ref);
        status = SecItemAdd(query.ref, nullptr);
    }
    if (status == errSecSuccess)
        return true;
    if (error)
        *error = QStringLiteral("Keychain: %1").arg(statusText(status));
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
#if defined(Q_OS_WIN)
    const std::wstring target = targetName(connectionId).toStdWString();
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return std::nullopt;

    const QString password = QString::fromUtf16(reinterpret_cast<const char16_t *>(credential->CredentialBlob),
                                                credential->CredentialBlobSize / sizeof(char16_t));
    CredFree(credential);
    return password;
#elif defined(Q_OS_MACOS)
    const CfRef<CFMutableDictionaryRef> query(newKeychainQuery(connectionId));
    CFDictionarySetValue(query.ref, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.ref, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    if (SecItemCopyMatching(query.ref, &result) != errSecSuccess || !result)
        return std::nullopt;
    const CfRef<CFTypeRef> owned(result);
    if (CFGetTypeID(result) != CFDataGetTypeID())
        return std::nullopt;
    return QString::fromUtf8(QByteArray::fromCFData(static_cast<CFDataRef>(result)));
#else
    Q_UNUSED(connectionId)
    return std::nullopt;
#endif
}

bool SecretStore::hasPassword(const QString &connectionId)
{
#if defined(Q_OS_WIN)
    const std::wstring target = targetName(connectionId).toStdWString();
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return false;
    CredFree(credential);
    return true;
#elif defined(Q_OS_MACOS)
    // 값은 읽지 않으므로 키체인 접근 허락을 묻지 않는다.
    const CfRef<CFMutableDictionaryRef> query(newKeychainQuery(connectionId));
    CFDictionarySetValue(query.ref, kSecMatchLimit, kSecMatchLimitOne);
    return SecItemCopyMatching(query.ref, nullptr) == errSecSuccess;
#else
    Q_UNUSED(connectionId)
    return false;
#endif
}

bool SecretStore::removePassword(const QString &connectionId)
{
#if defined(Q_OS_WIN)
    const std::wstring target = targetName(connectionId).toStdWString();
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0))
        return true;
    return GetLastError() == ERROR_NOT_FOUND; // 원래 없으면 지운 것과 같다
#elif defined(Q_OS_MACOS)
    const CfRef<CFMutableDictionaryRef> query(newKeychainQuery(connectionId));
    const OSStatus status = SecItemDelete(query.ref);
    return status == errSecSuccess || status == errSecItemNotFound;
#else
    Q_UNUSED(connectionId)
    return true;
#endif
}
