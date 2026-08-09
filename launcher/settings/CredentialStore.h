// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

/**
 * Stores launcher secrets outside the normal settings file.
 *
 * Windows builds use Credential Manager. Platforms without a native backend
 * keep values for the current process only, so secrets are never silently
 * persisted as plaintext launcher settings.
 */
class CredentialStore {
public:
    static bool isPersistent();

    static QString read(const QString& name, QString* error = nullptr);
    static bool write(const QString& name, const QString& secret,
                      QString* error = nullptr);
    static bool remove(const QString& name, QString* error = nullptr);

private:
    static QString targetName(const QString& name);
};
