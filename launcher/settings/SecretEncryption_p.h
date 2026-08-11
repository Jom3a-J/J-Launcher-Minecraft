// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QString>

// Private, deterministic-key entry points used by SecretEncryption itself and
// its unit tests. Application code must use SecretEncryption.h so keys continue
// to come only from the operating system credential store.
namespace SecretEncryption::Private {

QString encryptV2(const QByteArray& plaintext, const QByteArray& key);
QByteArray decryptWithKey(const QString& payload, const QByteArray& key);

}  // namespace SecretEncryption::Private
