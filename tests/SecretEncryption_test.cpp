// SPDX-License-Identifier: GPL-3.0-only

#include <QJsonObject>
#include <QTest>

#include "minecraft/auth/AccountData.h"
#include "settings/SecretEncryption_p.h"

class SecretEncryptionTest : public QObject {
    Q_OBJECT

private slots:
    void xchachaRoundTrip();
    void legacyPayloadStillDecrypts();
    void tamperingIsRejected();
    void unreadableAccountPayloadSurvivesAutosave();
    void clearedAccountDoesNotRestoreReadablePayload();
};

namespace {
QByteArray testKey()
{
    QByteArray key(32, Qt::Uninitialized);
    for (qsizetype i = 0; i < key.size(); ++i) {
        key[i] = static_cast<char>(i);
    }
    return key;
}
}  // namespace

void SecretEncryptionTest::xchachaRoundTrip()
{
    const QByteArray plaintext("account token fixture");
    const QString payload =
        SecretEncryption::Private::encryptV2(plaintext, testKey());

    QVERIFY2(payload.startsWith(QLatin1Char('2')),
             "New ciphertext must use the XChaCha20 payload version.");
    QCOMPARE(SecretEncryption::Private::decryptWithKey(payload, testKey()),
             plaintext);
}

void SecretEncryptionTest::legacyPayloadStillDecrypts()
{
    // Version-1 fixture: crypto_secretbox with key bytes 0..31, nonce bytes
    // 0..23, and plaintext "legacy account secret". Keeping this fixture
    // proves upgrades do not strand accounts encrypted by the first release.
    const QString payload = QStringLiteral(
        "1AAECAwQFBgcICQoLDA0ODxAREhMUFRYXj75dICxFhCGvioW6mxjB4jKaXy6ks4Jx2F/gSwb7auQ3xjS6+A==");

    QCOMPARE(SecretEncryption::Private::decryptWithKey(payload, testKey()),
             QByteArray("legacy account secret"));
}

void SecretEncryptionTest::tamperingIsRejected()
{
    QString payload = SecretEncryption::Private::encryptV2(
        QByteArray("must remain authenticated"), testKey());
    QVERIFY(!payload.isEmpty());

    const qsizetype position = payload.size() - 3;
    payload[position] = payload.at(position) == QLatin1Char('A')
        ? QLatin1Char('B')
        : QLatin1Char('A');

    QVERIFY(SecretEncryption::Private::decryptWithKey(payload, testKey())
                .isEmpty());
}

void SecretEncryptionTest::unreadableAccountPayloadSurvivesAutosave()
{
    const QString opaquePayload = QStringLiteral("9future-or-damaged-payload");
    QJsonObject stored;
    stored["type"] = QStringLiteral("MSA");
    stored["msa-client-id"] = QStringLiteral("test-client");
    stored["secrets"] = opaquePayload;

    AccountData account;
    QVERIFY(account.resumeStateFromV3(stored));

    const QJsonObject saved = account.saveState();
    QCOMPARE(saved.value("secrets").toString(), opaquePayload);
    QVERIFY(!saved.contains("msa"));
    QVERIFY(!saved.contains("utoken"));
    QVERIFY(!saved.contains("xrp-mc"));
    QVERIFY(!saved.contains("ygg"));
}

void SecretEncryptionTest::clearedAccountDoesNotRestoreReadablePayload()
{
    AccountData account;
    account.type = AccountType::MSA;
    account.preservedSecretsPayload = QStringLiteral("2previous-ciphertext");
    account.preservedSecretsPayloadIsUnreadable = false;

    const QJsonObject saved = account.saveState();
    QVERIFY(!saved.contains("secrets"));
}

QTEST_GUILESS_MAIN(SecretEncryptionTest)

#include "SecretEncryption_test.moc"
