// SPDX-License-Identifier: GPL-3.0-only

#include "CredentialStore.h"

#include "BuildConfig.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {
#ifndef Q_OS_WIN
QMutex sessionCredentialsMutex;
QHash<QString, QString> sessionCredentials;
#endif

#ifdef Q_OS_WIN
QString windowsError(const QString& operation, DWORD code)
{
    return QObject::tr("%1 failed with Windows error %2.")
        .arg(operation, QString::number(code));
}
#endif
}  // namespace

QString CredentialStore::targetName(const QString& name)
{
    return QStringLiteral("%1/%2")
        .arg(BuildConfig.LAUNCHER_NAME, name.trimmed());
}

bool CredentialStore::isPersistent()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QString CredentialStore::read(const QString& name, QString* error)
{
    if (error) {
        error->clear();
    }
    const QString target = targetName(name);

#ifdef Q_OS_WIN
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()),
                   CRED_TYPE_GENERIC, 0, &credential)) {
        const DWORD code = GetLastError();
        if (code != ERROR_NOT_FOUND && error) {
            *error = windowsError(QObject::tr("Reading the secure credential"),
                                  code);
        }
        return {};
    }

    const auto* characters = reinterpret_cast<const QChar*>(
        credential->CredentialBlob);
    const qsizetype characterCount =
        credential->CredentialBlobSize / sizeof(QChar);
    const QString secret(characters, characterCount);
    if (credential->CredentialBlob && credential->CredentialBlobSize > 0) {
        SecureZeroMemory(credential->CredentialBlob,
                         credential->CredentialBlobSize);
    }
    CredFree(credential);
    return secret;
#else
    QMutexLocker locker(&sessionCredentialsMutex);
    return sessionCredentials.value(target);
#endif
}

bool CredentialStore::write(const QString& name, const QString& secret,
                            QString* error)
{
    if (error) {
        error->clear();
    }
    if (secret.isEmpty()) {
        return remove(name, error);
    }
    const QString target = targetName(name);

#ifdef Q_OS_WIN
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(
        reinterpret_cast<LPCWSTR>(target.utf16()));
    credential.CredentialBlobSize =
        static_cast<DWORD>(secret.size() * sizeof(QChar));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<ushort*>(secret.utf16()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    const QString username = QStringLiteral("J Launcher secure storage");
    credential.UserName = const_cast<LPWSTR>(
        reinterpret_cast<LPCWSTR>(username.utf16()));

    if (!CredWriteW(&credential, 0)) {
        if (error) {
            *error = windowsError(QObject::tr("Saving the secure credential"),
                                  GetLastError());
        }
        return false;
    }
    return true;
#else
    QMutexLocker locker(&sessionCredentialsMutex);
    sessionCredentials.insert(target, secret);
    return true;
#endif
}

bool CredentialStore::remove(const QString& name, QString* error)
{
    if (error) {
        error->clear();
    }
    const QString target = targetName(name);

#ifdef Q_OS_WIN
    if (!CredDeleteW(reinterpret_cast<LPCWSTR>(target.utf16()),
                     CRED_TYPE_GENERIC, 0)) {
        const DWORD code = GetLastError();
        if (code != ERROR_NOT_FOUND) {
            if (error) {
                *error = windowsError(
                    QObject::tr("Removing the secure credential"), code);
            }
            return false;
        }
    }
    return true;
#else
    QMutexLocker locker(&sessionCredentialsMutex);
    sessionCredentials.remove(target);
    return true;
#endif
}
