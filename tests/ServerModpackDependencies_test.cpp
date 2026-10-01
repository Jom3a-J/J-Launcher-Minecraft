// SPDX-License-Identifier: GPL-3.0-only

// Creating servers from modpacks and checking their mod dependencies.

#include <QDir>
#include <QDirIterator>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrent/QtConcurrentRun>
#include <QUuid>
#include <algorithm>
#include <utility>

#include <archive/ArchiveWriter.h>
#include <server/ServerInstance.h>
#include <server/ServerManager.h>
#include <server/ServerModpackInstaller.h>
#include <server/ServerPackCompatibility.h>
#include <server/ServerPlayerAccess.h>
#include <server/ServerProperties.h>
#include <FileSystem.h>
#include <modplatform/helpers/OverrideUtils.h>

#include "ServerTestSupport.h"

using namespace ServerTestSupport;

class ServerModpackDependenciesTest : public QObject {
    Q_OBJECT

   private slots:
    void createsMatchingStoppedServerFromInstance()
    {
        const QString previousOrganization = QCoreApplication::organizationName();
        const QString previousApplication = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("JLauncherTests"));
        QCoreApplication::setApplicationName(QStringLiteral("ServerManagerTracking"));

        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        const QByteArray commonContents("common");
        const QString commonSha1 = QString::fromLatin1(
            QCryptographicHash::hash(commonContents, QCryptographicHash::Sha1).toHex());
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/common.jar"), commonContents));
        QVERIFY(writeFile(
            QDir(gameRoot).filePath("jarmods/common.pw.toml"),
            QString(
                "name = \"Common\"\n"
                "filename = \"common.jar\"\n"
                "side = \"both\"\n"
                "[download]\n"
                "mode = \"metadata:curseforge\"\n"
                "url = \"\"\n"
                "hash-format = \"sha1\"\n"
                "hash = \"%1\"\n"
                "[update.curseforge]\n"
                "file-id = 456\n"
                "project-id = 123\n")
                .arg(commonSha1).toUtf8()));
        QCOMPARE(ServerModpackInstaller::contentTrackingSource(
                     gameRoot, QDir(gameRoot).filePath("mods/common.jar")),
                 QString("curseforge:123:456"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/changed.jar"), "changed"));
        QVERIFY(ServerModpackInstaller::contentTrackingSource(
                    gameRoot, QDir(gameRoot).filePath("mods/changed.jar")).isEmpty());
        QVERIFY(writeFile(QDir(gameRoot).filePath("config/common.toml"),
                          "config"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath(
                "server-pack/server-properties.txt"),
            "topography-preset=void\n"));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.21.1", "0.16.10", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Paired Pack Server");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(!result.warnings.isEmpty());
        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QCOMPARE(server->name(), QString("Paired Pack Server"));
        QCOMPARE(server->version(), QString("1.21.1"));
        QCOMPARE(server->loaderType(), QString("fabric"));
        QCOMPARE(server->loaderVersion(), QString("0.16.10"));
        QCOMPARE(server->status(), ServerStatus::Stopped);
        QVERIFY(QFileInfo::exists(
            QDir(server->serverDirectory()).filePath("mods/common.jar")));
        QVERIFY(QFileInfo::exists(
            QDir(server->serverDirectory()).filePath("config/common.toml")));
        QString propertiesError;
        const auto properties = ServerProperties::load(
            server->serverPropertiesPath(), &propertiesError);
        QVERIFY2(propertiesError.isEmpty(), qPrintable(propertiesError));
        QCOMPARE(properties.value("topography-preset"), QString("void"));
        QCOMPARE(properties.value("server-port"),
                 QString::number(server->port()));
        QCOMPARE(manager.dataStore()
                     .value(server->id(), ServerDataGroup::ContentSources, "common.jar")
                     .toString(),
                 QString("curseforge:123:456"));
        QCoreApplication::setOrganizationName(previousOrganization);
        QCoreApplication::setApplicationName(previousApplication);
        QVERIFY(!QFileInfo::exists(server->serverJarPath()));
    }

    void appliesKnownSettingsAndBlocksOnlyUnsafeFirstStart()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeFabricModJar(
            QDir(gameRoot).filePath("mods/example.jar"), "example", "1.0.0"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("server-pack/server-setup-required.txt"),
            "topography-preset\n"));
        QVERIFY(writeFile(
            QDir(gameRoot).filePath("server.properties"),
            "topography-preset=wrong\n"));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.21.1", "0.16.10", {}, {}, {});

        // Missing setup does not waste a valid download or server projection.
        // It creates a stopped server and blocks only unsafe first startup.
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("server-pack/server-properties.txt"),
            "topography-preset=\n"));
        const auto needsSetup = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Needs Settings");
        QVERIFY2(needsSetup.isValid(), qPrintable(needsSetup.error));
        QVERIFY(needsSetup.warnings.join('\n').contains("topography-preset"));
        const auto incompleteServer = manager.getServer(needsSetup.serverId);
        QVERIFY(incompleteServer);
        QVERIFY(!incompleteServer->start());
        QCOMPARE(incompleteServer->status(), ServerStatus::Error);
        QVERIFY(incompleteServer->consoleLog().contains("topography-preset"));
        QVERIFY(manager.deleteServerPermanently(incompleteServer->id()));
        QCOMPARE(manager.serverCount(), 0);

        // A known adapter value overrides the incorrect packaged default.
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("server-pack/server-properties.txt"),
            "topography-preset=void\n"));
        const auto configured = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Configured Settings");
        QVERIFY2(configured.isValid(), qPrintable(configured.error));
        const auto server = manager.getServer(configured.serverId);
        QVERIFY(server);
        const auto properties = ServerProperties::load(server->serverPropertiesPath());
        QCOMPARE(properties.value("topography-preset"), QString("void"));
        QCOMPARE(manager.serverCount(), 1);
    }

    void createsRepairableDerivedFabricServerWithMissingDependency()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeFabricModJar(
            QDir(gameRoot).filePath("mods/connected-glass.jar"),
            "connectedglass", "*", QJsonObject{{ "fusion", ">=1.2.9" }}));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Incomplete Fabric Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(result.warnings.join('\n').contains("connectedglass"));
        QVERIFY(result.warnings.join('\n').contains("fusion"));
        QVERIFY(result.dependencyRequirements.join('\n').contains("fusion"));
        QCOMPARE(result.missingDependencyIds, QStringList{ QStringLiteral("fusion") });
        const auto incompleteServer = manager.getServer(result.serverId);
        QVERIFY(incompleteServer);
        incompleteServer->setEulaAccepted(true);
        QVERIFY(!incompleteServer->start());
        QVERIFY(incompleteServer->consoleLog().contains("fusion"));
        QVERIFY(manager.deleteServerPermanently(incompleteServer->id()));
        QCOMPARE(manager.serverCount(), 0);

        const QString nestedFusionPath = root.filePath("nested-fusion.jar");
        QVERIFY(writeFabricModJar(nestedFusionPath, "fusion", "*"));
        QFile nestedFusion(nestedFusionPath);
        QVERIFY(nestedFusion.open(QIODevice::ReadOnly));
        QVERIFY(writeFabricModJar(
            QDir(gameRoot).filePath("mods/dependency-bundle.jar"),
            "dependency_bundle", "*", {}, nestedFusion.readAll()));
        const auto completeResult = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Complete Fabric Pack");
        QVERIFY2(completeResult.isValid(), qPrintable(completeResult.error));
        const auto completeServer = manager.getServer(completeResult.serverId);
        QVERIFY(completeServer);
        QCOMPARE(ServerModpackInstaller::checkServerDependencies(
                     completeServer->serverDirectory(), completeServer->loaderType(),
                     completeServer->version(), completeServer->loaderVersion()).state,
                 ServerDependencyCheckState::Compatible);
        QCOMPARE(manager.serverCount(), 1);
    }

    void createsRepairableDerivedForgeServerWithMissingDependency()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/example.jar"), "META-INF/mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[47,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"example\"\n"
                "version=\"1.0.0\"\n"
                "displayName=\"Example\"\n"
                "[[dependencies.example]]\n"
                "modId=\"forge\"\n"
                "mandatory=true\n"
                "versionRange=\"[47,)\"\n"
                "ordering=\"NONE\"\n"
                "side=\"BOTH\"\n"
                "[[dependencies.example]]\n"
                "modId=\"clienthelper\"\n"
                "mandatory=true\n"
                "versionRange=\"[1,)\"\n"
                "ordering=\"NONE\"\n"
                "side=\"CLIENT\"\n"
                "[[dependencies.example]]\n"
                "modId=\"requiredlib\"\n"
                "mandatory=true\n"
                "versionRange=\"[1,)\"\n"
                "ordering=\"NONE\"\n"
                "side=\"SERVER\"\n")));
        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", {}, "47.1.0", {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Incomplete Forge Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(result.warnings.join('\n').contains("Forge", Qt::CaseInsensitive));
        QVERIFY(result.warnings.join('\n').contains("example"));
        QVERIFY(result.warnings.join('\n').contains("requiredlib"));
        QVERIFY(!result.warnings.join('\n').contains("clienthelper"));
        const auto missingServer = manager.getServer(result.serverId);
        QVERIFY(missingServer);
        missingServer->setEulaAccepted(true);
        QVERIFY(!missingServer->start());
        QVERIFY(missingServer->consoleLog().contains("requiredlib"));
        QVERIFY(manager.deleteServerPermanently(missingServer->id()));
        QCOMPARE(manager.serverCount(), 0);

        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/library-bundle.jar"),
            "META-INF/mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[47,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"bundle\"\n"
                "version=\"1.0.0\"\n"
                "displayName=\"Bundle\"\n"
                "[[mods]]\n"
                "modId=\"requiredlib\"\n"
                "version=\"0.5.0\"\n"
                "displayName=\"Required Library\"\n")));
        const auto mismatchedResult = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Mismatched Forge Pack");
        QVERIFY2(mismatchedResult.isValid(), qPrintable(mismatchedResult.error));
        QVERIFY(mismatchedResult.warnings.join('\n').contains("version", Qt::CaseInsensitive));
        QVERIFY(mismatchedResult.warnings.join('\n').contains("[1,)"));
        QVERIFY(mismatchedResult.warnings.join('\n').contains("0.5.0"));
        QVERIFY(manager.deleteServerPermanently(mismatchedResult.serverId));
        QCOMPARE(manager.serverCount(), 0);

        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/library-bundle.jar"),
            "META-INF/mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[47,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"bundle\"\n"
                "version=\"1.0.0\"\n"
                "displayName=\"Bundle\"\n"
                "[[mods]]\n"
                "modId=\"requiredlib\"\n"
                "version=\"${file.jarVersion}\"\n"
                "displayName=\"Required Library\"\n"),
            "1.0.0"));
        const auto completeResult = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Complete Forge Pack");
        QVERIFY2(completeResult.isValid(), qPrintable(completeResult.error));
        QCOMPARE(manager.serverCount(), 1);
    }

    void ignoresModernForgeMetadataForLegacyForge()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath(
                "mods/SpellboundSpireTowers-2.0.0_for_1.12.2.jar"),
            "META-INF/mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[28,)\"\n"
                "[[mods]]\n"
                "modId=\"spellboundspiretowers\"\n"
                "version=\"1.0.0\"\n"
                "displayName=\"Spellbound Spire Towers\"\n"
                "[[dependencies.spellboundspiretowers]]\n"
                "modId=\"minecraft\"\n"
                "mandatory=true\n"
                "versionRange=\"[1.14.4]\"\n"
                "ordering=\"NONE\"\n"
                "side=\"BOTH\"\n")));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.12.2", {}, "14.23.5.2860", {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot,
            "Legacy Forge Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(std::any_of(
            result.warnings.cbegin(), result.warnings.cend(),
            [](const QString& warning) {
                return warning.contains("Legacy Forge", Qt::CaseInsensitive);
            }));
        QCOMPARE(manager.serverCount(), 1);
    }

    void createsRepairableDerivedNeoForgeServerWithMissingDependency()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/example.jar"),
            "META-INF/neoforge.mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[4,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"example\"\n"
                "version=\"1.0.0\"\n"
                "displayName=\"Example\"\n"
                "[[dependencies.example]]\n"
                "modId=\"neoforge\"\n"
                "type=\"required\"\n"
                "versionRange=\"[21.1,)\"\n"
                "ordering=\"NONE\"\n"
                "side=\"BOTH\"\n"
                "[[dependencies.example]]\n"
                "modId=\"clienthelper\"\n"
                "type=\"required\"\n"
                "versionRange=\"[1,)\"\n"
                "ordering=\"NONE\"\n"
                "side=\"CLIENT\"\n"
                "[[dependencies.example]]\n"
                "modId=\"optionalhelper\"\n"
                "type=\"optional\"\n"
                "versionRange=\"1.0\"\n"
                "ordering=\"NONE\"\n"
                "side=\"SERVER\"\n"
                "[[dependencies.example]]\n"
                "modId=\"requiredlib\"\n"
                "type=\"required\"\n"
                "versionRange=\"[1,2)\"\n"
                "ordering=\"NONE\"\n"
                "side=\"SERVER\"\n"
                "[[dependencies.example]]\n"
                "modId=\"qualifiedlib\"\n"
                "type=\"required\"\n"
                "versionRange=\"[1.0-beta,)\"\n"
                "ordering=\"NONE\"\n"
                "side=\"SERVER\"\n"
                "[[dependencies.example]]\n"
                "modId=\"badmod\"\n"
                "type=\"incompatible\"\n"
                "versionRange=\"(,1.0],[2.0,)\"\n"
                "ordering=\"NONE\"\n"
                "side=\"SERVER\"\n")));
        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/qualified-library.jar"),
            "META-INF/neoforge.mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[4,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"qualifiedlib\"\n"
                "version=\"1.0-beta\"\n"
                "displayName=\"Qualified Library\"\n")));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.21.1", {}, {}, "21.1.0", {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Incomplete NeoForge Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(result.warnings.join('\n').contains("NeoForge", Qt::CaseInsensitive));
        QVERIFY(result.warnings.join('\n').contains("example"));
        QVERIFY(result.warnings.join('\n').contains("requiredlib"));
        QVERIFY(!result.warnings.join('\n').contains("clienthelper"));
        QVERIFY(manager.deleteServerPermanently(result.serverId));
        QVERIFY(!result.warnings.join('\n').contains("optionalhelper"));
        QCOMPARE(manager.serverCount(), 0);

        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/required-library.jar"),
            "META-INF/neoforge.mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[4,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"requiredlib\"\n"
                "version=\"2.0.0\"\n"
                "displayName=\"Required Library\"\n"
                "[[mods]]\n"
                "modId=\"optionalhelper\"\n"
                "version=\"99.0.0\"\n"
                "displayName=\"Optional Helper\"\n")));
        const auto mismatchedResult = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Mismatched NeoForge Pack");
        QVERIFY2(mismatchedResult.isValid(), qPrintable(mismatchedResult.error));
        QVERIFY(mismatchedResult.warnings.join('\n').contains("version", Qt::CaseInsensitive));
        QVERIFY(mismatchedResult.warnings.join('\n').contains("[1,2)"));
        QVERIFY(mismatchedResult.warnings.join('\n').contains("2.0.0"));
        QVERIFY(manager.deleteServerPermanently(mismatchedResult.serverId));
        QCOMPARE(manager.serverCount(), 0);

        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/required-library.jar"),
            "META-INF/neoforge.mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[4,)\"\n"
                "license=\"Test\"\n"
                "properties={ release=\"1.5.0\" }\n"
                "[[mods]]\n"
                "modId=\"requiredlib\"\n"
                "version=\"${file.release}\"\n"
                "displayName=\"Required Library\"\n"
                "[[mods]]\n"
                "modId=\"optionalhelper\"\n"
                "version=\"99.0.0\"\n"
                "displayName=\"Optional Helper\"\n")));
        const auto completeResult = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Complete NeoForge Pack");
        QVERIFY2(completeResult.isValid(), qPrintable(completeResult.error));
        QVERIFY(std::any_of(
            completeResult.warnings.cbegin(), completeResult.warnings.cend(),
            [](const QString& warning) { return warning.contains("qualifiedlib"); }));
        QCOMPARE(manager.serverCount(), 1);

        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/bad-mod.jar"),
            "META-INF/neoforge.mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[4,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"badmod\"\n"
                "version=\"1.5.0\"\n"
                "displayName=\"Bad Mod\"\n")));
        const auto outsideConflictResult = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Allowed NeoForge Pack");
        QVERIFY2(outsideConflictResult.isValid(), qPrintable(outsideConflictResult.error));
        QCOMPARE(manager.serverCount(), 2);

        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/bad-mod.jar"),
            "META-INF/neoforge.mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[4,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"badmod\"\n"
                "version=\"2.0.0\"\n"
                "displayName=\"Bad Mod\"\n")));
        const auto incompatibleResult = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Incompatible NeoForge Pack");
        QVERIFY2(incompatibleResult.isValid(), qPrintable(incompatibleResult.error));
        QVERIFY(incompatibleResult.warnings.join('\n').contains("incompatible", Qt::CaseInsensitive));
        QVERIFY(incompatibleResult.warnings.join('\n').contains("badmod"));
        const auto incompatibleServer = manager.getServer(incompatibleResult.serverId);
        QVERIFY(incompatibleServer);
        incompatibleServer->setEulaAccepted(true);
        QVERIFY(!incompatibleServer->start());
        QCOMPARE(manager.serverCount(), 3);
    }

    void createsDerivedServerWhenProviderVersionsDisagree()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/common.jar"), "common"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("config/common.toml"), "common"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("mrpack/modrinth.index.json"),
            QJsonDocument(QJsonObject{
                { "formatVersion", 1 },
                { "game", "minecraft" },
                { "dependencies", QJsonObject{
                    { "minecraft", "1.19.2" },
                    { "fabric-loader", "0.14.0" },
                } },
                { "files", QJsonArray{
                    QJsonObject{
                        { "path", "mods/common.jar" },
                        { "env", QJsonObject{{ "client", "required" }, { "server", "required" }} },
                    },
                } },
            }).toJson()));
        QVERIFY(writeFile(QDir(instanceRoot).filePath("mrpack/overrides.txt"),
                          "config/common.toml\n"));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Version Mismatch Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(result.warnings.join('\n').contains("installed", Qt::CaseInsensitive));
        QVERIFY(result.warnings.join('\n').contains("1.20.1"));
        QVERIFY(result.warnings.join('\n').contains("0.15.11"));
        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QCOMPARE(server->status(), ServerStatus::Stopped);
        QCOMPARE(server->version(), QString("1.20.1"));
        QCOMPARE(server->loaderType(), QString("fabric"));
        QCOMPARE(server->loaderVersion(), QString("0.15.11"));
        QVERIFY(QFileInfo::exists(QDir(server->serverDirectory()).filePath("mods/common.jar")));
    }

    void createsDerivedServerWithConflictingProviderDocuments()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/common.jar"), "common"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("mrpack/modrinth.index.json"),
            QJsonDocument(QJsonObject{
                { "formatVersion", 1 },
                { "game", "minecraft" },
                { "dependencies", QJsonObject{
                    { "minecraft", "1.20.1" },
                    { "fabric-loader", "0.15.11" },
                } },
                { "files", QJsonArray{
                    QJsonObject{
                        { "path", "mods/common.jar" },
                        { "env", QJsonObject{{ "client", "required" }, { "server", "required" }} },
                    },
                } },
            }).toJson()));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("flame/manifest.json"),
            QJsonDocument(QJsonObject{
                { "manifestType", "minecraftModpack" },
                { "manifestVersion", 1 },
                { "minecraft", QJsonObject{
                    { "version", "1.19.2" },
                    { "modLoaders", QJsonArray{QJsonObject{{ "id", "forge-43.2.0" }}}},
                } },
            }).toJson()));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Conflicting Providers");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.provider, QString("modrinth"));
        QVERIFY(result.warnings.join('\n').contains("conflicting provider", Qt::CaseInsensitive));
        QVERIFY(result.warnings.join('\n').contains("Modrinth", Qt::CaseSensitive));
        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QCOMPARE(server->status(), ServerStatus::Stopped);
        QCOMPARE(server->version(), QString("1.20.1"));
        QCOMPARE(server->loaderType(), QString("fabric"));
    }

    void createsDerivedServerWithInvalidOptionalHashMetadata()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/common.jar"), "common"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("mrpack/modrinth.index.json"),
            QJsonDocument(QJsonObject{
                { "formatVersion", 1 },
                { "game", "minecraft" },
                { "dependencies", QJsonObject{
                    { "minecraft", "1.20.1" },
                    { "fabric-loader", "0.15.11" },
                } },
                { "files", QJsonArray{
                    QJsonObject{
                        { "path", "mods/common.jar" },
                        { "env", QJsonObject{{ "client", "required" }, { "server", "required" }} },
                        { "hashes", QJsonObject{{ "sha512", "not-a-hash" }} },
                    },
                } },
            }).toJson()));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Invalid Hash Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(result.warnings.join('\n').contains("unverified", Qt::CaseInsensitive));
        QVERIFY(!result.warnings.join('\n').contains("will start", Qt::CaseInsensitive)
                || result.warnings.join('\n').contains("may", Qt::CaseInsensitive));
        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QCOMPARE(server->status(), ServerStatus::Stopped);
        QFile retained(QDir(server->modsDirectory()).filePath("common.jar"));
        QVERIFY(retained.open(QIODevice::ReadOnly));
        QCOMPARE(retained.readAll(), QByteArray("common"));
    }

    void createsDerivedServerWithMalformedOptionalMetadataUsingSafeFallback()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/keep.jar"), "keep"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("config/keep.toml"), "keep"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("top-level-arbitrary.dat"), "must not copy"));
        QVERIFY(writeFile(QDir(instanceRoot).filePath("mrpack/modrinth.index.json"),
                          "{ malformed"));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Malformed Fallback Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(result.warnings.join('\n').contains("malformed", Qt::CaseInsensitive));
        QVERIFY(result.warnings.join('\n').contains("installed", Qt::CaseInsensitive));
        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QCOMPARE(server->status(), ServerStatus::Stopped);
        QCOMPARE(server->version(), QString("1.20.1"));
        QVERIFY(QFileInfo::exists(QDir(server->serverDirectory()).filePath("mods/keep.jar")));
        QVERIFY(QFileInfo::exists(QDir(server->serverDirectory()).filePath("config/keep.toml")));
        QVERIFY(!QFileInfo::exists(
            QDir(server->serverDirectory()).filePath("top-level-arbitrary.dat")));
    }

    void createsDerivedServerWithConflictingSideDeclarations()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/shared.jar"), "shared"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("mrpack/modrinth.index.json"),
            QJsonDocument(QJsonObject{
                { "formatVersion", 1 },
                { "game", "minecraft" },
                { "dependencies", QJsonObject{
                    { "minecraft", "1.20.1" },
                    { "fabric-loader", "0.15.11" },
                } },
                { "files", QJsonArray{
                    QJsonObject{
                        { "path", "mods/shared.jar" },
                        { "env", QJsonObject{{ "client", "required" }, { "server", "required" }} },
                    },
                    QJsonObject{
                        { "path", "mods/shared.jar" },
                        { "env", QJsonObject{{ "client", "required" }, { "server", "unsupported" }} },
                    },
                } },
            }).toJson()));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Side Conflict Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(result.warnings.join('\n').contains("game-only", Qt::CaseInsensitive));
        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QCOMPARE(server->status(), ServerStatus::Stopped);
        QVERIFY(QFileInfo::exists(QDir(server->modsDirectory()).filePath("shared.jar")));
    }

    void offersAmbiguousPublishedServerRootsForSelection()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(QDir().mkpath(gameRoot));
        QVERIFY(writeFile(QDir(instanceRoot).filePath("server-pack/published-server-pack.txt"),
                          "curseforge\n"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("server-pack/server-files/PackA/mods/a.jar"), "a"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("server-pack/server-files/PackB/mods/b.jar"), "b"));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        QCOMPARE(ServerModpackInstaller::publishedServerRootChoices(instanceRoot),
                 QStringList({ "PackA", "PackB" }));
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Ambiguous Pack");
        QVERIFY(!result.isValid());
        QCOMPARE(result.failureCategory, ServerModpackFailureCategory::ContentProjection);
        QCOMPARE(result.failureStage, ServerModpackFailureStage::ContentPreparation);
        QVERIFY(result.error.contains("more than one possible", Qt::CaseInsensitive));
        QCOMPARE(manager.serverCount(), 0);

        const auto selected = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Selected Pack", 0, 0,
            "PackB");
        QVERIFY2(selected.isValid(), qPrintable(selected.error));
        const auto server = manager.getServer(selected.serverId);
        QVERIFY(server);
        QVERIFY(QFileInfo::exists(QDir(server->modsDirectory()).filePath("b.jar")));
        QVERIFY(!QFileInfo::exists(QDir(server->modsDirectory()).filePath("a.jar")));
    }

    void rejectsEmptyModpackProjection()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(QDir().mkpath(gameRoot));
        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Empty Projection");
        QVERIFY(!result.isValid());
        QVERIFY(result.error.contains("no usable server content", Qt::CaseInsensitive));
        QCOMPARE(result.failureCategory, ServerModpackFailureCategory::ContentProjection);
        QCOMPARE(result.failureStage, ServerModpackFailureStage::ContentPreparation);
        QCOMPARE(manager.serverCount(), 0);
    }

    void rejectsInvalidActualProfile()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(QDir().mkpath(gameRoot));
        ServerManager manager(root.filePath("server-data"));
        const auto emptyMinecraft = ServerModpackInstaller::profileForVersions(
            {}, "0.15.11", {}, {}, {});
        QVERIFY(!emptyMinecraft.isValid());
        const auto emptyResult = ServerModpackInstaller::createMatchingServer(
            &manager, emptyMinecraft, instanceRoot, gameRoot, "Empty MC");
        QVERIFY(!emptyResult.isValid());
        QCOMPARE(emptyResult.failureCategory, ServerModpackFailureCategory::InvalidProfile);
        QCOMPARE(emptyResult.failureStage, ServerModpackFailureStage::ProfileInspection);
        const auto multiLoader = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", "47.1.0", {}, {});
        QVERIFY(!multiLoader.isValid());
        const auto multiResult = ServerModpackInstaller::createMatchingServer(
            &manager, multiLoader, instanceRoot, gameRoot, "Multi Loader");
        QVERIFY(!multiResult.isValid());
        QCOMPARE(multiResult.failureCategory, ServerModpackFailureCategory::InvalidProfile);
        QCOMPARE(multiResult.failureStage, ServerModpackFailureStage::ProfileInspection);
        QCOMPARE(manager.serverCount(), 0);
    }

    void rejectsUnwritableServerContentDestination()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(QDir().mkpath(gameRoot));
        const QString blocker = root.filePath("destination-blocker");
        QVERIFY(writeFile(blocker, "not a directory"));
        QString error;
        QVERIFY(!ServerModpackInstaller::prepareContent(
            instanceRoot, gameRoot, blocker, nullptr, &error));
        QVERIFY(!error.isEmpty());
    }

    void rejectsMalformedForgeDependencyVersionRange()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/example.jar"), "META-INF/mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[47,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"example\"\n"
                "version=\"1.0.0\"\n"
                "displayName=\"Example\"\n"
                "[[dependencies.example]]\n"
                "modId=\"forge\"\n"
                "mandatory=true\n"
                "versionRange=\"[47,\"\n"
                "ordering=\"NONE\"\n"
                "side=\"BOTH\"\n")));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", {}, "47.1.0", {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Malformed Forge Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(result.warnings.join('\n').contains("invalid version range", Qt::CaseInsensitive));
        QVERIFY(result.warnings.join('\n').contains("[47,"));
        QVERIFY(result.warnings.join('\n').contains("could not fully verify", Qt::CaseInsensitive));
        QCOMPARE(manager.serverCount(), 1);
    }

    void acceptsInclusiveUnboundedForgeVersionRange()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/structure-gel.jar"),
            "META-INF/mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[47,)\"\n"
                "[[mods]]\n"
                "modId=\"structure_gel\"\n"
                "version=\"2.16.2\"\n"
                "displayName=\"Structure Gel API\"\n")));
        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/the-conjurer.jar"),
            "META-INF/mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[47,)\"\n"
                "[[mods]]\n"
                "modId=\"conjurer_illager\"\n"
                "version=\"1.1.6\"\n"
                "displayName=\"The Conjurer\"\n"
                "[[dependencies.conjurer_illager]]\n"
                "modId=\"structure_gel\"\n"
                "mandatory=true\n"
                "versionRange=\"[2.13.1,]\"\n"
                "ordering=\"NONE\"\n"
                "side=\"BOTH\"\n")));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", {}, "47.4.20", {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot,
            "Inclusive Unbounded Range Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(manager.serverCount(), 1);
    }

    void allowsForgeMetadataWithLenientMultilineText()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath(
                "mods/walljump-forge-1.16.4-1.3.7.jar"),
            "META-INF/mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[35,)\"\n"
                "[[mods]]\n"
                "modId=\"walljump\"\n"
                "version=\"1.16.4-1.3.7\"\n"
                "displayName=\"Wall-Jump!\"\n"
                "description=\"Jump from wall to wall!\n"
                "Jump towards a wall and hold the wall jump key.\"\n")));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.16.5", {}, "36.2.28", {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot,
            "Lenient Forge Metadata Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(std::any_of(
            result.warnings.cbegin(), result.warnings.cend(),
            [](const QString& warning) {
                return warning.contains("walljump-forge-1.16.4-1.3.7.jar")
                    && warning.contains("strictly parse", Qt::CaseInsensitive);
            }));
        QCOMPARE(manager.serverCount(), 1);
    }

    void defersForgeExactMinecraftPatchMismatch()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/Checklist-1.16.5-1.2.2.jar"),
            "META-INF/mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[31,)\"\n"
                "[[mods]]\n"
                "modId=\"checklist\"\n"
                "version=\"1.2.2\"\n"
                "displayName=\"Checklist\"\n"
                "[[dependencies.checklist]]\n"
                "modId=\"minecraft\"\n"
                "mandatory=true\n"
                "versionRange=\"[1.16.4]\"\n"
                "ordering=\"NONE\"\n"
                "side=\"BOTH\"\n")));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.16.5", {}, "36.2.28", {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot,
            "Forge Patch Compatibility Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(std::any_of(
            result.warnings.cbegin(), result.warnings.cend(),
            [](const QString& warning) {
                return warning.contains("Checklist")
                    && warning.contains("newer patch", Qt::CaseInsensitive);
            }));
        QCOMPARE(manager.serverCount(), 1);
    }

    void validatesExternalModrinthServerProjection()
    {
        const QString instanceRoot = qEnvironmentVariable(
            "JLAUNCHER_LIVE_MODRINTH_INSTANCE").trimmed();
        if (instanceRoot.isEmpty()) {
            QSKIP("Set JLAUNCHER_LIVE_MODRINTH_INSTANCE to run the live Modrinth projection test.");
        }
        QVERIFY2(QFileInfo(instanceRoot).isDir(), qPrintable(instanceRoot));

        QFile indexFile(QDir(instanceRoot).filePath(
            "mrpack/modrinth.index.json"));
        QVERIFY2(indexFile.open(QIODevice::ReadOnly), qPrintable(indexFile.fileName()));
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            indexFile.readAll(), &parseError);
        QVERIFY2(parseError.error == QJsonParseError::NoError
                     && document.isObject(),
                 qPrintable(parseError.errorString()));
        const QJsonObject dependencies = document.object()
                                             .value("dependencies")
                                             .toObject();
        const auto profile = ServerModpackInstaller::profileForVersions(
            dependencies.value("minecraft").toString(),
            dependencies.value("fabric-loader").toString(),
            dependencies.value("forge").toString(),
            dependencies.value("neoforge").toString(),
            dependencies.value("quilt-loader").toString());
        QVERIFY2(profile.isValid(), qPrintable(profile.error));

        QTemporaryDir serverData;
        QVERIFY(serverData.isValid());
        ServerManager manager(serverData.path());
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot,
            QDir(instanceRoot).filePath("minecraft"),
            "Live Modrinth Projection");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(!result.hasDedicatedServerPack);
        QCOMPARE(result.provider, QString("modrinth"));
        QCOMPARE(manager.serverCount(), 1);
    }

    void recognizesForgeBundledDependencies()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto nested = root.filePath("ponder.jar");
        QVERIFY(writeForgeModJar(nested, "META-INF/mods.toml",
            "modLoader=\"javafml\"\n[[mods]]\nmodId=\"ponder\"\nversion=\"1.0.91\"\n"));
        QFile file(nested);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        const auto instance = root.filePath("instance");
        const auto game = QDir(instance).filePath("minecraft");
        QVERIFY(QDir().mkpath(QDir(game).filePath("mods")));
        const auto parent = QDir(game).filePath("mods/create.jar");
        const auto makeParent = [&](bool includeNested) {
            MMCZip::ArchiveWriter writer(parent);
            return writer.open()
                && writer.addFile("META-INF/mods.toml", QByteArray(
                    "modLoader=\"javafml\"\n[[mods]]\nmodId=\"create\"\nversion=\"6.0.8\"\n"
                    "[[dependencies.create]]\nmodId=\"ponder\"\nmandatory=true\nversionRange=\"[1.0.91,)\"\nside=\"BOTH\"\n"))
                && writer.addFile("META-INF/jarjar/metadata.json", QByteArray(
                    "{\"jars\":[{\"path\":\"META-INF/jarjar/ponder.jar\"}]}"))
                && (!includeNested || writer.addFile("META-INF/jarjar/ponder.jar", bytes))
                && writer.close();
        };
        ServerManager manager(root.filePath("servers"));
        const auto profile = ServerModpackInstaller::profileForVersions("1.20.1", {}, "47.4.20", {}, {});
        QVERIFY(makeParent(true));
        const auto result = ServerModpackInstaller::createMatchingServer(&manager, profile, instance, game, "Bundled");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(makeParent(false));
        const auto missing = ServerModpackInstaller::createMatchingServer(&manager, profile, instance, game, "Missing");
        QVERIFY2(missing.isValid(), qPrintable(missing.error));
        QVERIFY(missing.warnings.join('\n').contains("ponder.jar"));
        const auto missingServer = manager.getServer(missing.serverId);
        QVERIFY(missingServer);
        missingServer->setEulaAccepted(true);
        QVERIFY(!missingServer->start());
        QVERIFY(missingServer->consoleLog().contains("ponder.jar"));
        QCOMPARE(manager.serverCount(), 2);
    }

    void treatsRuntimeRequirementsAsProvidedOnNeoForge()
    {
        // "java" is chosen by the launcher and NeoForge answers to "forge" as
        // well, so neither is a mod anyone could download. Listing them as
        // missing sends the user looking for something that does not exist.
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeForgeModJar(
            QDir(gameRoot).filePath("mods/example.jar"),
            "META-INF/neoforge.mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[4,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"example\"\n"
                "version=\"1.0.0\"\n"
                "displayName=\"Example\"\n"
                "[[dependencies.example]]\n"
                "modId=\"java\"\n"
                "type=\"required\"\n"
                "versionRange=\"[21,)\"\n"
                "ordering=\"NONE\"\n"
                "side=\"BOTH\"\n"
                "[[dependencies.example]]\n"
                "modId=\"forge\"\n"
                "type=\"required\"\n"
                "versionRange=\"[21.1,)\"\n"
                "ordering=\"NONE\"\n"
                "side=\"BOTH\"\n")));
        QTemporaryDir serverData;
        QVERIFY(serverData.isValid());
        ServerManager manager(serverData.path());
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.21.1", {}, {}, "21.1.0", {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Runtime Requirements");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY2(result.missingDependencyIds.isEmpty(),
                 qPrintable(result.missingDependencyIds.join(", ")));
        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        const auto check = ServerModpackInstaller::checkServerDependencies(
            server->serverDirectory(), "neoforge", "1.21.1", "21.1.0");
        QVERIFY2(check.missingDependencyIds.isEmpty(),
                 qPrintable(check.missingDependencyIds.join(", ")));
    }

    void recognizesForgeEmbeddedJarWithoutIndex()
    {
        // Some mods, Connector among them, embed the jar that declares their mod
        // id without shipping META-INF/jarjar/metadata.json. Ignoring those jars
        // reports the mod as missing even while it sits in the mods folder, so
        // installing it again can never clear the failure.
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto nested = root.filePath("connector-mod.jar");
        QVERIFY(writeForgeModJar(nested, "META-INF/mods.toml",
            "modLoader=\"javafml\"\n[[mods]]\nmodId=\"connectormod\"\nversion=\"1.0.0\"\n"));
        QFile file(nested);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        const auto instance = root.filePath("instance");
        const auto game = QDir(instance).filePath("minecraft");
        QVERIFY(QDir().mkpath(QDir(game).filePath("mods")));
        {
            MMCZip::ArchiveWriter writer(QDir(game).filePath("mods/requires-connector.jar"));
            QVERIFY(writer.open());
            QVERIFY(writer.addFile("META-INF/mods.toml", QByteArray(
                "modLoader=\"javafml\"\n[[mods]]\nmodId=\"fabricmod\"\nversion=\"1.0.0\"\n"
                "[[dependencies.fabricmod]]\nmodId=\"connectormod\"\nmandatory=true\n"
                "versionRange=\"[1.0.0,)\"\nside=\"BOTH\"\n")));
            QVERIFY(writer.close());
        }
        {
            MMCZip::ArchiveWriter writer(QDir(game).filePath("mods/connector.jar"));
            QVERIFY(writer.open());
            QVERIFY(writer.addFile("META-INF/MANIFEST.MF", QByteArray(
                "Manifest-Version: 1.0\nImplementation-Title: Connector\n")));
            QVERIFY(writer.addFile("META-INF/jarjar/connector-mod.jar", bytes));
            QVERIFY(writer.close());
        }
        ServerManager manager(root.filePath("servers"));
        const auto profile = ServerModpackInstaller::profileForVersions("1.20.1", {}, "47.4.20", {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instance, game, "Embedded Without Index");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY2(result.missingDependencyIds.isEmpty(),
                 qPrintable(result.missingDependencyIds.join(", ")));
        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QCOMPARE(ServerModpackInstaller::checkServerDependencies(
                     server->serverDirectory(), "forge", "1.20.1", "47.4.20")
                     .missingDependencyIds,
                 QStringList{});
    }

    void createsRepairableDerivedFabricServerWithMissingBundledJar()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        const QString jarPath = QDir(gameRoot).filePath("mods/broken-bundle.jar");
        QVERIFY(QDir().mkpath(QFileInfo(jarPath).absolutePath()));
        MMCZip::ArchiveWriter archive(jarPath);
        QVERIFY(archive.open());
        QVERIFY(archive.addFile("fabric.mod.json", QByteArray(
            R"({"schemaVersion":1,"id":"brokenbundle","version":"1.0.0","jars":[{"file":"META-INF/jars/missing.jar"}]})")));
        QVERIFY(archive.close());

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto missing = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Missing Fabric Bundle");
        QVERIFY2(missing.isValid(), qPrintable(missing.error));
        QVERIFY(missing.warnings.join('\n').contains("missing.jar"));
        const auto server = manager.getServer(missing.serverId);
        QVERIFY(server);
        server->setEulaAccepted(true);
        QVERIFY(!server->start());
        QVERIFY(server->consoleLog().contains("missing.jar"));
        QCOMPARE(manager.serverCount(), 1);
    }

    void publishedMissingBundledJarRemainsWarningOnly()
    {
        for (const QString &loader : { QStringLiteral("fabric"), QStringLiteral("forge") }) {
            QTemporaryDir temporaryRoot;
            QVERIFY(temporaryRoot.isValid());
            const QDir root(temporaryRoot.path());
            const QString instanceRoot = root.filePath("instance");
            const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
            QVERIFY(QDir().mkpath(gameRoot));
            QVERIFY(writeFile(QDir(instanceRoot).filePath("server-pack/published-server-pack.txt"),
                              "curseforge\n"));
            const QString modsDir = QDir(instanceRoot).filePath("server-pack/server-files/mods");
            QVERIFY(QDir().mkpath(modsDir));
            if (loader == QStringLiteral("fabric")) {
                MMCZip::ArchiveWriter broken(QDir(modsDir).filePath("broken-bundle.jar"));
                QVERIFY(broken.open());
                QVERIFY(broken.addFile("fabric.mod.json", QByteArray(
                    R"({"schemaVersion":1,"id":"brokenbundle","version":"1.0.0","jars":[{"file":"META-INF/jars/missing.jar"}]})")));
                QVERIFY(broken.close());
            } else {
                MMCZip::ArchiveWriter broken(QDir(modsDir).filePath("create.jar"));
                QVERIFY(broken.open());
                QVERIFY(broken.addFile("META-INF/mods.toml", QByteArray(
                    "modLoader=\"javafml\"\n[[mods]]\nmodId=\"create\"\nversion=\"6.0.8\"\n")));
                QVERIFY(broken.addFile("META-INF/jarjar/metadata.json", QByteArray(
                    "{\"jars\":[{\"path\":\"META-INF/jarjar/ponder.jar\"}]}")));
                QVERIFY(broken.close());
            }
            ServerManager manager(root.filePath("server-data"));
            ServerModpackProfile profile;
            if (loader == QStringLiteral("fabric")) {
                profile = ServerModpackInstaller::profileForVersions("1.20.1", "0.15.11", {}, {}, {});
            } else {
                profile = ServerModpackInstaller::profileForVersions("1.20.1", {}, "47.4.20", {}, {});
            }
            const auto result = ServerModpackInstaller::createMatchingServer(
                &manager, profile, instanceRoot, gameRoot, "Published Missing Bundle");
            QVERIFY2(result.isValid(), qPrintable(result.error));
            QVERIFY(result.hasDedicatedServerPack);
            QVERIFY(result.warnings.join('\n').contains(loader == QStringLiteral("fabric")
                                                              ? "missing.jar"
                                                              : "ponder.jar"));
            QCOMPARE(manager.serverCount(), 1);
        }
    }

    void derivedUnverifiedIntegrityWarningsAreBounded()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        constexpr int fileCount = 12;
        QJsonArray files;
        for (int i = 0; i < fileCount; ++i) {
            const QString name = QString("mods/file%1.jar").arg(i);
            QVERIFY(writeFile(QDir(gameRoot).filePath(name), "content"));
            files.append(QJsonObject{
                { "path", name },
                { "env", QJsonObject{{ "client", "required" }, { "server", "required" }} },
            });
        }
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("mrpack/modrinth.index.json"),
            QJsonDocument(QJsonObject{
                { "formatVersion", 1 },
                { "game", "minecraft" },
                { "dependencies", QJsonObject{
                    { "minecraft", "1.20.1" },
                    { "fabric-loader", "0.15.11" },
                } },
                { "files", files },
            }).toJson()));
        const auto report = inspectServerPack(instanceRoot);
        QVERIFY(!report.isIncompatible());
        QCOMPARE(report.unverifiedFileCount, fileCount);
        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Many Unverified");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        const QString joined = result.warnings.join('\n');
        QVERIFY(joined.contains(QString::number(fileCount)));
        QVERIFY(joined.contains("unverified", Qt::CaseInsensitive));
        int unverifiedMentions = 0;
        for (const QString &warning : result.warnings) {
            if (warning.contains("unverified", Qt::CaseInsensitive)) {
                ++unverifiedMentions;
            }
        }
        QCOMPARE(unverifiedMentions, 1);
        QVERIFY(result.warnings.size() <= 12);
    }

    void validatesExternalPublishedServerPack()
    {
        const QString instanceRoot = qEnvironmentVariable(
            "JLAUNCHER_LIVE_PUBLISHED_SERVER_INSTANCE").trimmed();
        if (instanceRoot.isEmpty()) {
            QSKIP("Set JLAUNCHER_LIVE_PUBLISHED_SERVER_INSTANCE to run the live published-pack test.");
        }
        QVERIFY2(QFileInfo(instanceRoot).isDir(), qPrintable(instanceRoot));

        const auto profile = ServerModpackInstaller::profileForVersions(
            qEnvironmentVariable("JLAUNCHER_LIVE_SERVER_MINECRAFT"), {},
            qEnvironmentVariable("JLAUNCHER_LIVE_SERVER_FORGE"),
            qEnvironmentVariable("JLAUNCHER_LIVE_SERVER_NEOFORGE"), {});
        QVERIFY2(profile.isValid(), qPrintable(profile.error));

        QTemporaryDir serverData;
        QVERIFY(serverData.isValid());
        ServerManager manager(serverData.path());
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot,
            QDir(instanceRoot).filePath("minecraft"),
            "Live Published Server Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(result.hasDedicatedServerPack);
        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QVERIFY(QFileInfo(server->modsDirectory()).isDir());
        QVERIFY(QFileInfo(QDir(server->serverDirectory()).filePath("config")).isDir());
        QCOMPARE(manager.serverCount(), 1);
    }

    void validatesExternalNormalizedServerProjection()
    {
        const QString instanceRoot = qEnvironmentVariable(
            "JLAUNCHER_LIVE_NORMALIZED_INSTANCE").trimmed();
        if (instanceRoot.isEmpty()) {
            QSKIP("Set JLAUNCHER_LIVE_NORMALIZED_INSTANCE to run the live normalized-projection test.");
        }
        QVERIFY2(QFileInfo(instanceRoot).isDir(), qPrintable(instanceRoot));

        const auto profile = ServerModpackInstaller::profileForVersions(
            qEnvironmentVariable("JLAUNCHER_LIVE_SERVER_MINECRAFT"),
            qEnvironmentVariable("JLAUNCHER_LIVE_SERVER_FABRIC"),
            qEnvironmentVariable("JLAUNCHER_LIVE_SERVER_FORGE"),
            qEnvironmentVariable("JLAUNCHER_LIVE_SERVER_NEOFORGE"), {});
        QVERIFY2(profile.isValid(), qPrintable(profile.error));

        QTemporaryDir serverData;
        QVERIFY(serverData.isValid());
        ServerManager manager(serverData.path());
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot,
            QDir(instanceRoot).filePath("minecraft"),
            "Live Normalized Projection");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(!result.hasDedicatedServerPack);
        QCOMPARE(result.provider,
                 qEnvironmentVariable("JLAUNCHER_LIVE_SERVER_PROVIDER"));
        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QVERIFY(QFileInfo(server->serverDirectory()).isDir());
        QCOMPARE(server->version(), profile.minecraftVersion);
        QCOMPARE(server->loaderType(), profile.loaderType);
        QCOMPARE(server->loaderVersion(), profile.loaderVersion);
        const QDir sourceMods(QDir(instanceRoot).filePath("minecraft/mods"));
        if (!sourceMods.entryList(QStringList() << "*.jar", QDir::Files).isEmpty()) {
            QVERIFY(QFileInfo(server->modsDirectory()).isDir());
        }
        QCOMPARE(manager.serverCount(), 1);
    }
};

QTEST_GUILESS_MAIN(ServerModpackDependenciesTest)

#include "ServerModpackDependencies_test.moc"
