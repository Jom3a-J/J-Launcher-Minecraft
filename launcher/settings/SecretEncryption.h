// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QString>

/**
 * Authenticated encryption for secrets that are too large for a credential
 * store entry, such as Minecraft account tokens.
 *
 * The ciphertext is written to ordinary launcher files. The key is a random
 * 32 bytes held in the operating system's credential store through
 * CredentialStore, so the encryption is only as good as that store: on a
 * platform without a native backend there is nowhere safe to keep the key and
 * encrypt() reports failure. Callers decide whether plaintext compatibility is
 * acceptable for that platform.
 *
 * This protects a copied file - a backup, a synced folder, another machine -
 * because the key does not travel with it. It does not protect against a
 * process already running as the user, which can ask the credential store for
 * the key exactly as the launcher does.
 */
namespace SecretEncryption {

/// Encrypts with XChaCha20-Poly1305 to a versioned base64 payload carrying its
/// own nonce.
/// Returns an empty string on failure.
QString encrypt(const QByteArray& plaintext);

/// Reverses encrypt(), including legacy version-1 XSalsa20-Poly1305 payloads.
/// Returns an empty array if the payload is malformed, was tampered with, or
/// was written under a different key.
QByteArray decrypt(const QString& payload);

/// Discards the stored key. Any payload encrypted under it becomes
/// permanently unreadable, so this is only for removing launcher data.
bool forgetKey(QString* error = nullptr);

}  // namespace SecretEncryption
