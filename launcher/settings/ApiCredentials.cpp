// SPDX-License-Identifier: GPL-3.0-only

#include "ApiCredentials.h"

#include <QCoreApplication>
#include <QDebug>

#include "logs/Privacy.h"
#include "settings/CredentialStore.h"
#include "settings/SettingsObject.h"

namespace ApiCredentials {

namespace {
void resetLegacySettings(SettingsObject* settings, const Kind& kind)
{
    for (const QString& legacy : kind.legacySettings) {
        settings->reset(legacy);
    }
}
}  // namespace

const Kind& curseForge()
{
    static const Kind kind{ QStringLiteral("CurseForgeApiKey"),
                            { QStringLiteral("FlameKeyOverride"), QStringLiteral("CFKeyOverride") },
                            "CurseForge API key",
                            QT_TRANSLATE_NOOP("Application", "The CurseForge API key could not be verified after saving.") };
    return kind;
}

const Kind& modrinth()
{
    static const Kind kind{ QStringLiteral("ModrinthApiToken"),
                            { QStringLiteral("ModrinthToken") },
                            "Modrinth API token",
                            QT_TRANSLATE_NOOP("Application", "The Modrinth API token could not be verified after saving.") };
    return kind;
}

QString loadAndMigrate(SettingsObject* settings, const Kind& kind)
{
    for (const QString& legacy : kind.legacySettings) {
        settings->registerSetting(legacy, "");
    }

    QString readError;
    QString value = CredentialStore::read(kind.storeName, &readError);
    if (!readError.isEmpty()) {
        qWarning() << qUtf8Printable(QStringLiteral("Could not read the stored %1 securely;"
                                                    " preserving the legacy value for this session if available.")
                                         .arg(QLatin1String(kind.logName)))
                   << Privacy::sanitizeText(readError);
    }

    QString legacyValue;
    for (const QString& legacy : kind.legacySettings) {
        legacyValue = settings->get(legacy).toString().trimmed();
        if (!legacyValue.isEmpty()) {
            break;
        }
    }

    if (value.isEmpty() && !legacyValue.isEmpty()) {
        // The plaintext copy is used this session whatever happens below.
        value = legacyValue;
        if (!CredentialStore::isPersistent()) {
            return value;
        }
        QString writeError;
        if (!CredentialStore::write(kind.storeName, legacyValue, &writeError)) {
            qWarning() << qUtf8Printable(QStringLiteral("Could not migrate the %1 to secure storage;"
                                                        " preserving the legacy value for this session.")
                                             .arg(QLatin1String(kind.logName)))
                       << Privacy::sanitizeText(writeError);
            return value;
        }
        QString verifyError;
        const QString stored = CredentialStore::read(kind.storeName, &verifyError);
        if (verifyError.isEmpty() && stored == legacyValue) {
            resetLegacySettings(settings, kind);
        } else {
            const QString reason =
                verifyError.isEmpty() ? QStringLiteral("stored credential did not match") : Privacy::sanitizeText(verifyError);
            qWarning() << qUtf8Printable(QStringLiteral("Could not verify the migrated %1;"
                                                        " preserving the legacy value for this session.")
                                             .arg(QLatin1String(kind.logName)))
                       << reason;
        }
    } else if (!value.isEmpty() && CredentialStore::isPersistent() && readError.isEmpty()) {
        // A successfully read persistent credential supersedes any legacy copy.
        resetLegacySettings(settings, kind);
    } else if (!legacyValue.isEmpty()) {
        // Session-only stores must not destroy the only persistent legacy copy.
        value = legacyValue;
    }
    return value;
}

bool save(SettingsObject* settings, const Kind& kind, const QString& value, QString* error)
{
    const QString normalized = value.trimmed();
    if (error) {
        error->clear();
    }

    QString storageError;
    const bool stored = normalized.isEmpty() ? CredentialStore::remove(kind.storeName, &storageError)
                                             : CredentialStore::write(kind.storeName, normalized, &storageError);
    if (!stored) {
        if (error) {
            *error = storageError;
        }
        return false;
    }

    QString verificationError;
    const QString storedValue = CredentialStore::read(kind.storeName, &verificationError);
    if (!verificationError.isEmpty() || storedValue != normalized) {
        if (error) {
            *error = !verificationError.isEmpty() ? verificationError : QCoreApplication::translate("Application", kind.verifyFailed);
        }
        return false;
    }

    resetLegacySettings(settings, kind);
    return true;
}

}  // namespace ApiCredentials
