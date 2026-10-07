// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>

class SettingsObject;

/**
 * The optional personal API keys: the CurseForge API key and the Modrinth token.
 *
 * They live in CredentialStore, never in the plaintext settings file. Older
 * builds kept them as plaintext settings; those copies are moved across at
 * startup, and only cleared once the secure copy reads back intact.
 */
namespace ApiCredentials {

struct Kind {
    QString storeName;           // CredentialStore entry
    QStringList legacySettings;  // plaintext settings it replaces, read in this order
    const char* logName;         // how log lines name it, e.g. "CurseForge API key"
    const char* verifyFailed;    // error shown when a saved value does not read back
};

const Kind& curseForge();
const Kind& modrinth();

/// Registers the legacy settings, moves any plaintext copy into the credential
/// store, and returns the value to use for this session.
QString loadAndMigrate(SettingsObject* settings, const Kind& kind);

/// Stores the trimmed value (removes it when empty) and checks it reads back
/// unchanged. Only then are the plaintext copies cleared.
bool save(SettingsObject* settings, const Kind& kind, const QString& value, QString* error = nullptr);

}  // namespace ApiCredentials
