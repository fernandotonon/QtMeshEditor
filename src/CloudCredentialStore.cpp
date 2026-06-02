#include "CloudCredentialStore.h"
#include "AppSettingsKeys.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>
#ifndef CRED_PERSIST_LOCAL_USER
#define CRED_PERSIST_LOCAL_USER 2
#endif
#endif

#ifdef Q_OS_MACOS
#include <Security/Security.h>
#endif

#if defined(Q_OS_LINUX) && defined(HAVE_LIBSECRET)
#include "CloudCredentialStore_linux.h"
#endif

namespace {

constexpr auto kTestOrganizationName = "QtMeshEditorTests";

bool useIsolatedTestStorage()
{
    return QCoreApplication::organizationName() == QLatin1StringView(kTestOrganizationName);
}

QByteArray sessionToPayload(const CloudSession& session)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("token"), session.token);
    obj.insert(QStringLiteral("expiresAt"), session.expiresAt);
    if (!session.email.isEmpty())
        obj.insert(QStringLiteral("email"), session.email);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

CloudSession sessionFromPayload(const QByteArray& payload)
{
    CloudSession session;
    if (payload.isEmpty())
        return session;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return session;

    const QJsonObject obj = doc.object();
    session.token = obj.value(QStringLiteral("token")).toString();
    session.expiresAt = static_cast<qint64>(obj.value(QStringLiteral("expiresAt")).toDouble(0));
    session.email = obj.value(QStringLiteral("email")).toString();
    return session;
}

QString fallbackFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/cloud_session.dat");
}

bool writeFallbackFile(const QByteArray& payload)
{
    const QString path = fallbackFilePath();
    if (path.isEmpty())
        return false;

    const QFileInfo info(path);
    QDir dir = info.dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
        return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (file.write(payload) != payload.size())
        return false;
    file.close();
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

QByteArray readFallbackFile()
{
    QFile file(fallbackFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

void removeFallbackFile()
{
    QFile::remove(fallbackFilePath());
}

bool storeSecretBytes(const QByteArray& payload)
{
    if (useIsolatedTestStorage())
        return writeFallbackFile(payload);

#if defined(Q_OS_MACOS)
    const QByteArray service = QByteArrayLiteral("QtMeshEditor");
    const QByteArray account = QByteArrayLiteral("QtMeshCloud");

    const auto query = [&]() {
        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(dict, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(dict, kSecAttrService,
                             CFStringCreateWithCString(nullptr, service.constData(), kCFStringEncodingUTF8));
        CFDictionarySetValue(dict, kSecAttrAccount,
                             CFStringCreateWithCString(nullptr, account.constData(), kCFStringEncodingUTF8));
        CFDictionarySetValue(dict, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(dict, kSecMatchLimit, kSecMatchLimitOne);
        return dict;
    };

    CFDictionaryRef existing = query();
    SecItemDelete(existing);
    CFRelease(existing);

    CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(payload.constData()),
                                  static_cast<CFIndex>(payload.size()));
    CFMutableDictionaryRef add = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(add, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(add, kSecAttrService,
                         CFStringCreateWithCString(nullptr, service.constData(), kCFStringEncodingUTF8));
    CFDictionarySetValue(add, kSecAttrAccount,
                         CFStringCreateWithCString(nullptr, account.constData(), kCFStringEncodingUTF8));
    CFDictionarySetValue(add, kSecValueData, data);
    const OSStatus status = SecItemAdd(add, nullptr);
    CFRelease(data);
    CFRelease(add);
    return status == errSecSuccess;

#elif defined(Q_OS_WIN)
    const wchar_t* target = L"QtMeshEditor/QtMeshCloud";
    CredDeleteW(target, CRED_TYPE_GENERIC, 0);

    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target);
    cred.CredentialBlobSize = static_cast<DWORD>(payload.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(payload.constData()));
    cred.Persist = CRED_PERSIST_LOCAL_USER;
    cred.UserName = const_cast<LPWSTR>(L"QtMeshEditor");
    return CredWriteW(&cred, 0) != FALSE;

#elif defined(Q_OS_LINUX) && defined(HAVE_LIBSECRET)
    return qtmesh_cloud_secret_store(payload.constData()) != 0;

#else
    return writeFallbackFile(payload);
#endif
}

QByteArray loadSecretBytes()
{
    if (useIsolatedTestStorage())
        return readFallbackFile();

#if defined(Q_OS_MACOS)
    const QByteArray service = QByteArrayLiteral("QtMeshEditor");
    const QByteArray account = QByteArrayLiteral("QtMeshCloud");

    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(dict, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(dict, kSecAttrService,
                         CFStringCreateWithCString(nullptr, service.constData(), kCFStringEncodingUTF8));
    CFDictionarySetValue(dict, kSecAttrAccount,
                         CFStringCreateWithCString(nullptr, account.constData(), kCFStringEncodingUTF8));
    CFDictionarySetValue(dict, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(dict, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(dict, &result);
    CFRelease(dict);
    if (status != errSecSuccess || result == nullptr)
        return {};

    CFDataRef data = reinterpret_cast<CFDataRef>(result);
    return QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                      static_cast<int>(CFDataGetLength(data)));

#elif defined(Q_OS_WIN)
    const wchar_t* target = L"QtMeshEditor/QtMeshCloud";
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &cred) || cred == nullptr)
        return {};
    const QByteArray payload(reinterpret_cast<const char*>(cred->CredentialBlob),
                             static_cast<int>(cred->CredentialBlobSize));
    CredFree(cred);
    return payload;

#elif defined(Q_OS_LINUX) && defined(HAVE_LIBSECRET)
    char* raw = qtmesh_cloud_secret_load();
    if (!raw)
        return {};
    const QByteArray payload(raw);
    qtmesh_cloud_secret_free(raw);
    return payload;

#else
    return readFallbackFile();
#endif
}

void deleteSecretBytes()
{
    if (useIsolatedTestStorage()) {
        removeFallbackFile();
        return;
    }

#if defined(Q_OS_MACOS)
    const QByteArray service = QByteArrayLiteral("QtMeshEditor");
    const QByteArray account = QByteArrayLiteral("QtMeshCloud");
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(dict, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(dict, kSecAttrService,
                         CFStringCreateWithCString(nullptr, service.constData(), kCFStringEncodingUTF8));
    CFDictionarySetValue(dict, kSecAttrAccount,
                         CFStringCreateWithCString(nullptr, account.constData(), kCFStringEncodingUTF8));
    SecItemDelete(dict);
    CFRelease(dict);

#elif defined(Q_OS_WIN)
    CredDeleteW(L"QtMeshEditor/QtMeshCloud", CRED_TYPE_GENERIC, 0);

#elif defined(Q_OS_LINUX) && defined(HAVE_LIBSECRET)
    qtmesh_cloud_secret_delete();

#else
    removeFallbackFile();
#endif
}

} // namespace

bool CloudCredentialStore::saveSession(const CloudSession& session)
{
    if (!session.hasToken())
        return false;
    return storeSecretBytes(sessionToPayload(session));
}

CloudSession CloudCredentialStore::loadSession()
{
    return sessionFromPayload(loadSecretBytes());
}

void CloudCredentialStore::clearSession()
{
    deleteSecretBytes();
    removeFallbackFile();
}

bool CloudCredentialStore::hasSession()
{
    return loadSession().hasToken();
}

void CloudCredentialStore::migrateLegacySettingsIfNeeded()
{
    QSettings settings;
    const QString legacyToken = settings.value(AppSettingsKeys::cloudToken()).toString();
    if (legacyToken.isEmpty())
        return;

    CloudSession session;
    session.token = legacyToken;
    session.expiresAt = settings.value(AppSettingsKeys::cloudTokenExpiresAt()).toLongLong();
    session.email = settings.value(AppSettingsKeys::cloudUserEmail()).toString();
    if (saveSession(session)) {
        settings.remove(AppSettingsKeys::cloudToken());
        settings.remove(AppSettingsKeys::cloudTokenExpiresAt());
        settings.remove(AppSettingsKeys::cloudUserEmail());
        settings.sync();
    }
}
