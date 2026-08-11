// SPDX-License-Identifier: GPL-3.0-only

#include "SecretEncryption.h"
#include "SecretEncryption_p.h"

#include "CredentialStore.h"

#include <QDebug>
#include <QMutex>
#include <QMutexLocker>

#include <sodium.h>

namespace {

/// Name of the key entry inside the credential store.
const char* KEY_NAME = "AccountSecretKey";

/// Version 1 used crypto_secretbox (XSalsa20-Poly1305). Version 2 uses the
/// documented XChaCha20-Poly1305-IETF construction. Version 1 remains readable
/// so existing accounts migrate on their next successful save.
const char LEGACY_PAYLOAD_VERSION = '1';
const char PAYLOAD_VERSION = '2';

QMutex keyMutex;

bool ensureSodium()
{
    static const bool ready = [] {
        // sodium_init() returns 1 when another caller got there first, which
        // is success as far as we are concerned.
        const int result = sodium_init();
        if (result < 0) {
            qWarning() << "libsodium failed to initialise; account secrets "
                          "cannot be encrypted.";
            return false;
        }
        return true;
    }();
    return ready;
}

/**
 * Returns the encryption key, creating and storing one on first use.
 *
 * Empty means no key is available and the caller must not encrypt: either the
 * credential store has no persistent backend, or storing the key failed.
 */
QByteArray encryptionKey(bool createIfMissing)
{
    if (!ensureSodium()) {
        return {};
    }

    // Without a persistent credential store the key would be lost on exit and
    // every previously written payload would be unreadable.
    if (!CredentialStore::isPersistent()) {
        return {};
    }

    QMutexLocker locker(&keyMutex);

    QString readError;
    const QString stored = CredentialStore::read(QString::fromLatin1(KEY_NAME),
                                                 &readError);
    if (!readError.isEmpty()) {
        qWarning() << "Could not read the account secret key:" << readError;
        return {};
    }

    if (!stored.isEmpty()) {
        const QByteArray key = QByteArray::fromBase64(
            stored.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (key.size() == crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
            return key;
        }
        // Replacing a malformed key would permanently orphan every existing
        // ciphertext. Leave it untouched so the user can repair or restore
        // the credential and AccountData can preserve the opaque payload.
        qWarning() << "The stored account secret key is malformed. It will "
                      "not be replaced automatically.";
        return {};
    }

    // Decryption must never manufacture a replacement key. An encrypted
    // profile copied from another machine, or a temporarily missing
    // credential, must remain recoverable if the original key is restored.
    if (!createIfMissing) {
        return {};
    }

    QByteArray key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES,
                   Qt::Uninitialized);
    randombytes_buf(key.data(), static_cast<size_t>(key.size()));

    QString writeError;
    const QString encodedKey = QString::fromLatin1(key.toBase64());
    if (!CredentialStore::write(QString::fromLatin1(KEY_NAME), encodedKey,
                                &writeError)) {
        qWarning() << "Could not store the account secret key:" << writeError;
        return {};
    }

    QString verifyError;
    const QString persistedKey = CredentialStore::read(
        QString::fromLatin1(KEY_NAME), &verifyError);
    if (!verifyError.isEmpty() || persistedKey != encodedKey) {
        const QString reason = verifyError.isEmpty()
            ? QStringLiteral("stored credential did not match")
            : verifyError;
        qWarning() << "Could not verify the stored account secret key:"
                   << reason;
        sodium_memzero(key.data(), static_cast<size_t>(key.size()));
        return {};
    }

    return key;
}

}  // namespace

namespace SecretEncryption {

namespace Private {

QString encryptV2(const QByteArray& plaintext, const QByteArray& key)
{
    if (!ensureSodium()
        || key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
        return {};
    }

    QByteArray nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
                     Qt::Uninitialized);
    randombytes_buf(nonce.data(), static_cast<size_t>(nonce.size()));

    QByteArray cipher(
        plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES,
        Qt::Uninitialized);
    unsigned long long cipherSize = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char*>(cipher.data()), &cipherSize,
            reinterpret_cast<const unsigned char*>(plaintext.constData()),
            static_cast<unsigned long long>(plaintext.size()), nullptr, 0,
            nullptr,
            reinterpret_cast<const unsigned char*>(nonce.constData()),
            reinterpret_cast<const unsigned char*>(key.constData())) != 0) {
        return {};
    }
    cipher.resize(static_cast<qsizetype>(cipherSize));

    return QString(QLatin1Char(PAYLOAD_VERSION))
        + QString::fromLatin1((nonce + cipher).toBase64());
}

QByteArray decryptWithKey(const QString& payload, const QByteArray& key)
{
    if (!ensureSodium() || payload.isEmpty()
        || key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
        return {};
    }

    const char version = payload.at(0).toLatin1();
    if (version != PAYLOAD_VERSION && version != LEGACY_PAYLOAD_VERSION) {
        return {};
    }

    const QByteArray raw = QByteArray::fromBase64(
        payload.mid(1).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);

    if (version == LEGACY_PAYLOAD_VERSION) {
        if (raw.size() < crypto_secretbox_NONCEBYTES
                             + crypto_secretbox_MACBYTES) {
            return {};
        }
        const QByteArray nonce = raw.left(crypto_secretbox_NONCEBYTES);
        const QByteArray cipher = raw.mid(crypto_secretbox_NONCEBYTES);
        QByteArray plaintext(cipher.size() - crypto_secretbox_MACBYTES,
                             Qt::Uninitialized);
        if (crypto_secretbox_open_easy(
                reinterpret_cast<unsigned char*>(plaintext.data()),
                reinterpret_cast<const unsigned char*>(cipher.constData()),
                static_cast<unsigned long long>(cipher.size()),
                reinterpret_cast<const unsigned char*>(nonce.constData()),
                reinterpret_cast<const unsigned char*>(key.constData())) != 0) {
            return {};
        }
        return plaintext;
    }

    if (raw.size() < crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
                         + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return {};
    }
    const QByteArray nonce =
        raw.left(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    const QByteArray cipher =
        raw.mid(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    QByteArray plaintext(
        cipher.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES,
        Qt::Uninitialized);
    unsigned long long plaintextSize = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(plaintext.data()),
            &plaintextSize, nullptr,
            reinterpret_cast<const unsigned char*>(cipher.constData()),
            static_cast<unsigned long long>(cipher.size()), nullptr, 0,
            reinterpret_cast<const unsigned char*>(nonce.constData()),
            reinterpret_cast<const unsigned char*>(key.constData())) != 0) {
        return {};
    }
    plaintext.resize(static_cast<qsizetype>(plaintextSize));
    return plaintext;
}

}  // namespace Private

QString encrypt(const QByteArray& plaintext)
{
    QByteArray key = encryptionKey(true);
    if (key.isEmpty()) {
        return {};
    }

    const QString payload = Private::encryptV2(plaintext, key);
    sodium_memzero(key.data(), static_cast<size_t>(key.size()));
    if (payload.isEmpty()) {
        qWarning() << "Encrypting an account secret failed.";
    }
    return payload;
}

QByteArray decrypt(const QString& payload)
{
    if (payload.isEmpty()
        || (payload.at(0) != QLatin1Char(PAYLOAD_VERSION)
            && payload.at(0) != QLatin1Char(LEGACY_PAYLOAD_VERSION))) {
        return {};
    }

    QByteArray key = encryptionKey(false);
    if (key.isEmpty()) {
        return {};
    }

    const QByteArray plaintext = Private::decryptWithKey(payload, key);
    sodium_memzero(key.data(), static_cast<size_t>(key.size()));
    if (plaintext.isEmpty()) {
        // Wrong key, or the payload was altered. Both are failures, and
        // neither should be treated as an empty secret.
        qWarning() << "An account secret could not be decrypted. It was "
                      "either tampered with or written under a different key.";
    }
    return plaintext;
}

bool forgetKey(QString* error)
{
    QMutexLocker locker(&keyMutex);
    return CredentialStore::remove(QString::fromLatin1(KEY_NAME), error);
}

}  // namespace SecretEncryption
