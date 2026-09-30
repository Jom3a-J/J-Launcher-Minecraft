// SPDX-License-Identifier: GPL-3.0-only

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <server/ServerDataStore.h>

class ServerDataStoreTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        // Legacy records live in application settings; keep each test's settings separate.
        m_previousOrganization = QCoreApplication::organizationName();
        m_previousApplication = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("JLauncherServerDataStoreTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("ServerDataStore_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    }

    void cleanup()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
        QCoreApplication::setOrganizationName(m_previousOrganization);
        QCoreApplication::setApplicationName(m_previousApplication);
    }

    void storesValuesPerServerAndReloadsThem()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString folder = root.filePath("records");
        {
            ServerDataStore store(folder);
            QVERIFY(store.setValue("alpha", ServerDataGroup::Automation, "enabled", true));
            QVERIFY(store.setValue("alpha", ServerDataGroup::Automation, "retention", 4));
            QVERIFY(store.setValue("alpha", ServerDataGroup::ContentSources, "sodium.jar", "modrinth:AANobbMI:abc"));
            QVERIFY(store.setValue("beta", ServerDataGroup::Automation, "enabled", false));
            QVERIFY(store.remove("alpha", ServerDataGroup::ContentSources, "sodium.jar"));
            QVERIFY(store.remove("alpha", ServerDataGroup::ContentSources, "never-added.jar"));
        }
        QVERIFY(QFileInfo::exists(QDir(folder).filePath("alpha.json")));
        QVERIFY(QFileInfo::exists(QDir(folder).filePath("beta.json")));

        const ServerDataStore reloaded(folder);
        QCOMPARE(reloaded.value("alpha", ServerDataGroup::Automation, "enabled").toBool(), true);
        QCOMPARE(reloaded.value("alpha", ServerDataGroup::Automation, "retention").toInt(), 4);
        QVERIFY(!reloaded.value("alpha", ServerDataGroup::ContentSources, "sodium.jar").isValid());
        QCOMPARE(reloaded.value("beta", ServerDataGroup::Automation, "enabled", true).toBool(), false);
        QCOMPARE(reloaded.value("gamma", ServerDataGroup::Monitoring, "cpuWarning", 85).toInt(), 85);
    }

    void listsKeepOnlyTheNewestEntries()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerDataStore store(root.path());
        for (const QString& entry : { "one", "two", "three", "four" }) {
            QVERIFY(store.addToList("s", ServerDataGroup::Automation, "history", entry, 3,
                                    ServerDataStore::Order::NewestFirst));
            QVERIFY(store.addToList("s", ServerDataGroup::PlayerHistory, "events", entry, 3,
                                    ServerDataStore::Order::NewestLast));
        }
        QCOMPARE(store.list("s", ServerDataGroup::Automation, "history"),
                 (QStringList{ "four", "three", "two" }));
        QCOMPARE(store.list("s", ServerDataGroup::PlayerHistory, "events"),
                 (QStringList{ "two", "three", "four" }));
    }

    void importsLegacySettingsOnceAndLeavesThemInPlace()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString id = QStringLiteral("20260930120000_abcdef12");
        QSettings settings;
        settings.setValue(QStringLiteral("ServerAutomation/%1/enabled").arg(id), true);
        settings.setValue(QStringLiteral("ServerAutomation/%1/retention").arg(id), 5);
        settings.setValue(QStringLiteral("ServerAutomation/%1/history").arg(id), QStringList{ "only entry" });
        settings.setValue(QStringLiteral("ServerMonitoring/%1/cpuWarning").arg(id), 70);
        settings.setValue(QStringLiteral("ServerPlayerHistory/%1/events").arg(id), QStringList{ "a joined", "a left" });
        settings.setValue(QStringLiteral("ServerContentSources/%1/tracked.jar").arg(id), "curseforge:1:2");
        settings.setValue(QStringLiteral("ServerContentMetadata/%1/tracked.jar/versionId").arg(id), "v2");
        settings.setValue(QStringLiteral("ServerContentMetadata/%1/tracked.jar/hashAlgorithm").arg(id), 1);
        settings.setValue(QStringLiteral("ServerUpdates/%1/latestRollbackBackup").arg(id), "server-before");
        settings.setValue(QStringLiteral("ServerAutomation/other-server/enabled"), true);
        settings.sync();

        ServerDataStore store(root.path());
        QVERIFY(store.importLegacySettings(id));
        QCOMPARE(store.value(id, ServerDataGroup::Automation, "enabled").toBool(), true);
        QCOMPARE(store.value(id, ServerDataGroup::Automation, "retention").toInt(), 5);
        QCOMPARE(store.list(id, ServerDataGroup::Automation, "history"), QStringList{ "only entry" });
        QCOMPARE(store.value(id, ServerDataGroup::Monitoring, "cpuWarning").toInt(), 70);
        QCOMPARE(store.list(id, ServerDataGroup::PlayerHistory, "events"), (QStringList{ "a joined", "a left" }));
        QCOMPARE(store.value(id, ServerDataGroup::ContentSources, "tracked.jar").toString(), QStringLiteral("curseforge:1:2"));
        const QVariantMap metadata = store.value(id, ServerDataGroup::ContentMetadata, "tracked.jar").toMap();
        QCOMPARE(metadata.value("versionId").toString(), QStringLiteral("v2"));
        QCOMPARE(metadata.value("hashAlgorithm").toInt(), 1);
        QCOMPARE(store.value(id, ServerDataGroup::Updates, "latestRollbackBackup").toString(), QStringLiteral("server-before"));
        QVERIFY(!store.value(id, ServerDataGroup::Automation, "action").isValid());

        // The old values stay where they were, as a backup.
        QCOMPARE(QSettings().value(QStringLiteral("ServerAutomation/%1/retention").arg(id)).toInt(), 5);

        // Once the server has its own file, the old values are never read again.
        QVERIFY(store.setValue(id, ServerDataGroup::Automation, "retention", 9));
        settings.setValue(QStringLiteral("ServerAutomation/%1/retention").arg(id), 1);
        settings.sync();
        ServerDataStore reopened(root.path());
        QVERIFY(!reopened.importLegacySettings(id));
        QCOMPARE(reopened.value(id, ServerDataGroup::Automation, "retention").toInt(), 9);

        // Nothing to import creates no file.
        QVERIFY(!reopened.importLegacySettings(QStringLiteral("unknown-server")));
        QVERIFY(!QFileInfo::exists(QDir(root.path()).filePath("unknown-server.json")));
    }

    void refusesServerIdsThatAreNotPlainNames()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ServerDataStore store(root.filePath("records"));
        for (const QString& id : { QStringLiteral("../escape"), QStringLiteral("a/b"), QStringLiteral("a\\b"),
                                   QStringLiteral("C:evil"), QStringLiteral(".."), QString() }) {
            QVERIFY2(!store.setValue(id, ServerDataGroup::Automation, "enabled", true), qPrintable(id));
            QVERIFY(!store.importLegacySettings(id));
        }
        QVERIFY(!QFileInfo::exists(root.filePath("escape.json")));
    }

    void startsOverWhenTheFileCannotBeRead()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QFile broken(QDir(root.path()).filePath("s.json"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{ not json");
        broken.close();

        ServerDataStore store(root.path());
        QCOMPARE(store.value("s", ServerDataGroup::Monitoring, "ramWarning", 90).toInt(), 90);
        QVERIFY(store.setValue("s", ServerDataGroup::Monitoring, "ramWarning", 80));
        QCOMPARE(ServerDataStore(root.path()).value("s", ServerDataGroup::Monitoring, "ramWarning").toInt(), 80);
    }

private:
    QString m_previousOrganization;
    QString m_previousApplication;
};

QTEST_GUILESS_MAIN(ServerDataStoreTest)

#include "ServerDataStore_test.moc"
