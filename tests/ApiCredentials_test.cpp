// SPDX-License-Identifier: GPL-3.0-only

#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include "settings/ApiCredentials.h"
#include "settings/CredentialStore.h"
#include "settings/INISettingsObject.h"

namespace {
// A throwaway credential, so the tests never touch the launcher's real entries.
class TestKind final {
   public:
    TestKind()
        : kind{ QStringLiteral("TestApiCredentials-") + QUuid::createUuid().toString(QUuid::WithoutBraces),
                { QStringLiteral("NewName"), QStringLiteral("OldName") },
                "test key",
                "The test key could not be verified after saving." }
    {}
    ~TestKind()
    {
        QString ignoredError;
        CredentialStore::remove(kind.storeName, &ignoredError);
    }

    ApiCredentials::Kind kind;
};

void writeLegacySettings(const QString& path, const QString& newName, const QString& oldName)
{
    INISettingsObject settings(path);
    settings.registerSetting("NewName", "");
    settings.registerSetting("OldName", "");
    settings.set("NewName", newName);
    settings.set("OldName", oldName);
}
}  // namespace

class ApiCredentialsTest : public QObject {
    Q_OBJECT

   private slots:
    void movesPlaintextValueIntoSecureStorage()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("settings.cfg");
        writeLegacySettings(path, QStringLiteral("  new-secret  "), QStringLiteral("old-secret"));
        TestKind test;

        INISettingsObject settings(path);
        QCOMPARE(ApiCredentials::loadAndMigrate(&settings, test.kind), QStringLiteral("new-secret"));
        QCOMPARE(CredentialStore::read(test.kind.storeName), QStringLiteral("new-secret"));
        QVERIFY(settings.get("NewName").toString().isEmpty());
        QVERIFY(settings.get("OldName").toString().isEmpty());
    }

    void fallsBackToTheOlderSettingName()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("settings.cfg");
        writeLegacySettings(path, QString(), QStringLiteral("old-secret"));
        TestKind test;

        INISettingsObject settings(path);
        QCOMPARE(ApiCredentials::loadAndMigrate(&settings, test.kind), QStringLiteral("old-secret"));
        QCOMPARE(CredentialStore::read(test.kind.storeName), QStringLiteral("old-secret"));
        QVERIFY(settings.get("OldName").toString().isEmpty());
    }

    void storedValueWinsAndClearsPlaintextCopy()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("settings.cfg");
        writeLegacySettings(path, QStringLiteral("stale"), QString());
        TestKind test;
        QVERIFY(CredentialStore::write(test.kind.storeName, QStringLiteral("stored")));

        INISettingsObject settings(path);
        QCOMPARE(ApiCredentials::loadAndMigrate(&settings, test.kind), QStringLiteral("stored"));
        QCOMPARE(CredentialStore::read(test.kind.storeName), QStringLiteral("stored"));
        QVERIFY(settings.get("NewName").toString().isEmpty());
    }

    void saveTrimsAndEmptyRemoves()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("settings.cfg");
        writeLegacySettings(path, QStringLiteral("legacy"), QString());
        TestKind test;
        INISettingsObject settings(path);
        QVERIFY(!ApiCredentials::loadAndMigrate(&settings, test.kind).isEmpty());

        QString error;
        QVERIFY2(ApiCredentials::save(&settings, test.kind, QStringLiteral("  fresh  "), &error), qPrintable(error));
        QCOMPARE(CredentialStore::read(test.kind.storeName), QStringLiteral("fresh"));

        QVERIFY2(ApiCredentials::save(&settings, test.kind, QStringLiteral("   "), &error), qPrintable(error));
        QVERIFY(CredentialStore::read(test.kind.storeName).isEmpty());
        QVERIFY(settings.get("NewName").toString().isEmpty());
    }
};

QTEST_GUILESS_MAIN(ApiCredentialsTest)

#include "ApiCredentials_test.moc"
