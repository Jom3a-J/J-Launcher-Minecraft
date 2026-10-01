// SPDX-License-Identifier: GPL-3.0-only

// Turning modpacks into server content: profiles, client-only filtering and published server packs.

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

class ServerModpackPrepTest : public QObject {
    Q_OBJECT

   private slots:
    void validatesModpackServerProfiles()
    {
        const auto fabric = ServerModpackInstaller::profileForVersions(
            "1.21.1", "0.16.10", {}, {}, {});
        QVERIFY(fabric.isValid());
        QCOMPARE(fabric.minecraftVersion, QString("1.21.1"));
        QCOMPARE(fabric.loaderType, QString("fabric"));
        QCOMPARE(fabric.loaderVersion, QString("0.16.10"));

        const auto neoForge = ServerModpackInstaller::profileForVersions(
            "1.20.1", {}, {}, "47.1.106", {});
        QVERIFY(neoForge.isValid());
        QCOMPARE(neoForge.loaderType, QString("neoforge"));

        QVERIFY(!ServerModpackInstaller::profileForVersions(
                     "1.21.1", "0.16.10", "52.0.1", {}, {})
                     .isValid());
        QVERIFY(!ServerModpackInstaller::profileForVersions(
                     "1.21.1", {}, {}, {}, "0.27.1")
                     .isValid());
        QVERIFY(!ServerModpackInstaller::profileForVersions(
                     {}, "0.16.10", {}, {}, {})
                     .isValid());
    }

    void readsProviderLoaderFromSavedComponentDocument()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QJsonArray components{
            QJsonObject{
                { "uid", "net.minecraft" },
                { "version", "1.21.1" },
            },
            QJsonObject{
                { "uid", "net.neoforged" },
                { "version", "21.1.233" },
            },
        };
        QVERIFY(writeFile(
            QDir(temporaryRoot.path()).filePath("mmc-pack.json"),
            QJsonDocument(QJsonObject{
                { "formatVersion", 1 },
                { "components", components },
            }).toJson()));

        const auto profile =
            ServerModpackInstaller::profileFromInstanceRoot(temporaryRoot.path());
        QVERIFY2(profile.isValid(), qPrintable(profile.error));
        QCOMPARE(profile.minecraftVersion, QString("1.21.1"));
        QCOMPARE(profile.loaderType, QString("neoforge"));
        QCOMPARE(profile.loaderVersion, QString("21.1.233"));
    }

    void preservesOverrideWordsInFilePaths()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString source = temporary.filePath("overrides");
        const QString metadata = temporary.filePath("mrpack");
        const QString relative = "config/resource_overrides.json";
        QVERIFY(writeFile(QDir(source).filePath(relative), "{}"));
        Override::createOverrides("overrides", metadata, source);
        const auto paths = Override::readOverrides("overrides", metadata);
        QVERIFY(paths.contains(relative));
        QVERIFY(!paths.contains("json"));
    }

    void checksRequiredSettingsBeforeWorldGeneration()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(writeFile(root.filePath("server-setup-required.txt"),
                          "# Pack requirements\ncustom-generator\n"));
        QVERIFY(ServerProperties::worldSetupIssue(root.path()).contains("custom-generator"));
        QVERIFY(writeFile(root.filePath("server.properties"), "custom-generator=sky\n"));
        QVERIFY(ServerProperties::worldSetupIssue(root.path()).isEmpty());
        QVERIFY(writeFile(root.filePath("server.properties"), "level-name=existing\n"));
        QVERIFY(writeFile(root.filePath("existing/level.dat"), "world"));
        QVERIFY(ServerProperties::worldSetupIssue(root.path()).isEmpty());
    }

    void checksTopographyAcrossProviders()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(writeFile(root.filePath("config/topography/Topography.js"), "// presets"));
        QVERIFY(!ServerProperties::worldSetupIssue(root.path()).isEmpty());
        QVERIFY(writeFile(root.filePath("server.properties"), "topography-preset=void\n"));
        QVERIFY(ServerProperties::worldSetupIssue(root.path()).isEmpty());
    }

    void checksMissingFileRequirementsBeforeStartup()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString requirements =
            root.filePath("jlauncher_required_server_files.txt");
        QVERIFY(writeFile(requirements, "mods/required-library.jar\n"));
        QVERIFY(ServerProperties::worldSetupIssue(root.path())
                    .contains("mods/required-library.jar"));
        QVERIFY(writeFile(root.filePath("mods/required-library.jar"), "library"));
        QVERIFY(ServerProperties::worldSetupIssue(root.path()).isEmpty());
        QVERIFY(writeFile(requirements, "../outside.jar\n"));
        QVERIFY(ServerProperties::worldSetupIssue(root.path())
                    .contains("unsafe path"));
    }

    void preparesServerContentAndFiltersClientOnlyFiles()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        const QString destination = root.filePath("prepared");

        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/universal.jar"), "universal"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/client-only.jar"), "client"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("config/common.toml"), "config"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("config/client-options.toml"), "client config"));
        QVERIFY(writeFile(
            QDir(gameRoot).filePath(
                "datapacks/worldgen_removals/data/example/biome_modifier/remove.json"),
            "{}"));
        QVERIFY(writeFile(
            QDir(gameRoot).filePath("configureddefaults/config/common.snbt"),
            "enabled: true"));
        QVERIFY(writeFile(
            QDir(gameRoot).filePath("ftbteambases/structures/base.nbt"),
            "structure"));
        QVERIFY(writeFile(
            QDir(gameRoot).filePath("global_data_packs/skyfactory/pack.mcmeta"),
            "{}"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("default-server.properties"),
                          "allow-flight=true\nspawn-protection=512\n"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("server-icon.png"), "icon"));
        QVERIFY(writeFile(
            QDir(gameRoot).filePath("future-provider-data/rules.json"),
            "{}"));

        const QJsonArray files{
            QJsonObject{
                { "path", "mods/universal.jar" },
                { "env", QJsonObject{{ "client", "required" }, { "server", "required" }} },
            },
            QJsonObject{
                { "path", "mods/client-only.jar" },
                { "env", QJsonObject{{ "client", "required" }, { "server", "unsupported" }} },
            },
        };
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("mrpack/modrinth.index.json"),
            QJsonDocument(QJsonObject{
                { "formatVersion", 1 },
                { "game", "minecraft" },
                { "files", files },
            }).toJson()));
        QVERIFY(writeFile(QDir(instanceRoot).filePath("mrpack/client-overrides.txt"),
                          "config/client-options.toml\n"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("mrpack/overrides.txt"),
            "config/common.toml\n"
            "configureddefaults/config/common.snbt\n"
            "datapacks/worldgen_removals/data/example/biome_modifier/remove.json\n"
            "ftbteambases/structures/base.nbt\n"
            "global_data_packs/skyfactory/pack.mcmeta\n"
            "future-provider-data/rules.json\n"
            "default-server.properties\n"
            "server-icon.png\n"));

        QStringList skipped;
        QString error;
        QVERIFY2(ServerModpackInstaller::prepareContent(
                     instanceRoot, gameRoot, destination, &skipped, &error),
                 qPrintable(error));
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("mods/universal.jar")));
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("config/common.toml")));
        QVERIFY(QFileInfo::exists(QDir(destination).filePath(
            "datapacks/worldgen_removals/data/example/biome_modifier/remove.json")));
        QVERIFY(QFileInfo::exists(QDir(destination).filePath(
            "configureddefaults/config/common.snbt")));
        QVERIFY(QFileInfo::exists(QDir(destination).filePath(
            "ftbteambases/structures/base.nbt")));
        QVERIFY(QFileInfo::exists(QDir(destination).filePath(
            "global_data_packs/skyfactory/pack.mcmeta")));
        QVERIFY(QFileInfo::exists(QDir(destination).filePath(
            "future-provider-data/rules.json")));
        QCOMPARE(QFile(QDir(destination).filePath("server.properties")).exists(), true);
        QCOMPARE(QFile(QDir(destination).filePath("server-icon.png")).exists(), true);
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath("mods/client-only.jar")));
        QVERIFY(!QFileInfo::exists(
            QDir(destination).filePath("config/client-options.toml")));
        QVERIFY(skipped.contains("mods/client-only.jar"));
        QVERIFY(skipped.contains("config/client-options.toml"));

        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/ftb-client.jar"), "client"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("server-pack/client-only.txt"),
            "mods/ftb-client.jar\n"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath(
                "server-pack/server-files/mods/ftb-server.jar"),
            "server"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("server-pack/include.txt"),
            "mods/ftb-server.jar\n"));
        skipped.clear();
        error.clear();
        QVERIFY2(ServerModpackInstaller::prepareContent(
                     instanceRoot, gameRoot, destination, &skipped, &error),
                 qPrintable(error));
        QVERIFY(!QFileInfo::exists(
            QDir(destination).filePath("mods/ftb-client.jar")));
        QVERIFY(QFileInfo::exists(
            QDir(destination).filePath("mods/ftb-server.jar")));
        QVERIFY(skipped.contains("mods/ftb-client.jar"));
    }

    void filtersLegacyAndJarDeclaredClientOnlyMods()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        const QString destination = root.filePath("prepared");

        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/legacy-client.jar"),
                          "legacy client"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/server.jar"), "server"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/both.jar"), "both"));
        QVERIFY(writeFabricModJar(QDir(gameRoot).filePath("mods/jar-client.jar"),
                                  "jar_client", "client"));
        QVERIFY(writeFabricModJar(QDir(gameRoot).filePath("mods/jar-both.jar"),
                                  "jar_both", "*"));

        const auto writeLegacyMetadata = [&](const QString& metadataName,
                                             const QString& filename,
                                             const QString& side) {
            return writeFile(
                QDir(gameRoot).filePath("jarmods/" + metadataName + ".pw.toml"),
                QString("name = \"%1\"\nfilename = \"%1\"\nside = \"%2\"\n"
                        "[download]\nmode = \"url\"\nurl = \"https://example.invalid/%1\"\n"
                        "hash-format = \"sha1\"\nhash = \"00\"\n"
                        "[update]\n[update.modrinth]\n"
                        "mod-id = \"test-%1\"\nversion = \"1\"\n")
                    .arg(filename, side)
                    .toUtf8());
        };
        QVERIFY(writeLegacyMetadata("legacy-client", "legacy-client.jar", "client"));
        QVERIFY(writeLegacyMetadata("server", "server.jar", "server"));
        QVERIFY(writeLegacyMetadata("both", "both.jar", "both"));

        QStringList skipped;
        QString error;
        QVERIFY2(ServerModpackInstaller::prepareContent(
                     instanceRoot, gameRoot, destination, &skipped, &error),
                 qPrintable(error));

        QVERIFY(!QFileInfo::exists(
            QDir(destination).filePath("mods/legacy-client.jar")));
        QVERIFY(!QFileInfo::exists(
            QDir(destination).filePath("mods/jar-client.jar")));
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("mods/server.jar")));
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("mods/both.jar")));
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("mods/jar-both.jar")));
        QVERIFY(skipped.contains("mods/legacy-client.jar"));
        QVERIFY(skipped.contains("mods/jar-client.jar"));
    }

    void overlaysAtLauncherServerOnlyRootFiles()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        const QString destination = root.filePath("prepared");

        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/universal.jar"),
                          "universal"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/client-only.jar"),
                          "client"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("config/common.cfg"),
                          "common"));
        QVERIFY(writeFile(
            QDir(gameRoot).filePath(
                "global_data_packs/skyfactory/pack.mcmeta"),
            "{}"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("server-pack/provider.txt"),
            "atlauncher\n"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("server-pack/client-only.txt"),
            "mods/client-only.jar\n"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath(
                "server-pack/server-files/log4j2_17-111.xml"),
            "log4j configuration"));

        QStringList skipped;
        QString error;
        QVERIFY2(ServerModpackInstaller::prepareContent(
                     instanceRoot, gameRoot, destination, &skipped, &error),
                 qPrintable(error));
        QVERIFY(QFileInfo::exists(
            QDir(destination).filePath("mods/universal.jar")));
        QVERIFY(QFileInfo::exists(
            QDir(destination).filePath("config/common.cfg")));
        QVERIFY(QFileInfo::exists(
            QDir(destination).filePath(
                "global_data_packs/skyfactory/pack.mcmeta")));
        QVERIFY(QFileInfo::exists(
            QDir(destination).filePath("log4j2_17-111.xml")));
        QVERIFY(!QFileInfo::exists(
            QDir(destination).filePath("mods/client-only.jar")));
        QVERIFY(skipped.contains("mods/client-only.jar"));
    }

    void remembersConfirmedClientOnlyJarHashes()
    {
        const QString previousOrganization = QCoreApplication::organizationName();
        const QString previousApplication = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("JLauncherTests"));
        QCoreApplication::setApplicationName(QStringLiteral("KnownClientOnly"));
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        const QString jarPath = QDir(gameRoot).filePath("mods/runtime-client.jar");
        QVERIFY(writeForgeModJar(
            jarPath, "META-INF/mods.toml",
            "modLoader=\"javafml\"\nloaderVersion=\"[47,)\"\n"
            "[[mods]]\nmodId=\"runtimeclient\"\nversion=\"1\"\n"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("config/server.toml"), "server"));
        QVERIFY(!ServerModpackInstaller::isKnownClientOnlyFile(jarPath));
        QVERIFY(ServerModpackInstaller::markKnownClientOnlyFile(jarPath));
        QVERIFY(ServerModpackInstaller::isKnownClientOnlyFile(jarPath));

        QStringList skipped;
        QString error;
        QVERIFY2(ServerModpackInstaller::prepareContent(
                     instanceRoot, gameRoot, root.filePath("prepared"),
                     &skipped, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(root.filePath("prepared/mods/runtime-client.jar")));
        QVERIFY(skipped.contains("mods/runtime-client.jar"));

        QFile jar(jarPath);
        QVERIFY(jar.open(QIODevice::ReadOnly));
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(jar.readAll(), QCryptographicHash::Sha256).toHex());
        QSettings().remove(
            QStringLiteral("ServerCompatibility/KnownClientOnlyHashes/") + hash);
        QCoreApplication::setOrganizationName(previousOrganization);
        QCoreApplication::setApplicationName(previousApplication);
    }

    void recordsIncompleteProviderServerManifestForRepair()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(QDir().mkpath(gameRoot));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("server-pack/include.txt"),
            "future-provider-data/required.dat\n"));

        QStringList warnings;
        QString error;
        QVERIFY2(ServerModpackInstaller::prepareContent(
                     instanceRoot, gameRoot, root.filePath("prepared"), nullptr,
                     &error, &warnings), qPrintable(error));
        QVERIFY(warnings.join('\n').contains("future-provider-data/required.dat"));
        QFile requirements(root.filePath(
            "prepared/jlauncher_required_server_files.txt"));
        QVERIFY(requirements.open(QIODevice::ReadOnly));
        QVERIFY(requirements.readAll().contains("future-provider-data/required.dat"));
    }

    void rejectsMissingRequiredServerOnlyFile()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(QDir().mkpath(gameRoot));
        QVERIFY(writeFile(QDir(gameRoot).filePath("config/usable.toml"), "usable"));

        const QJsonArray files{
            QJsonObject{
                { "path", "mods/server-required.jar" },
                { "env", QJsonObject{{ "client", "unsupported" }, { "server", "required" }} },
            },
            QJsonObject{
                { "path", "config/usable.toml" },
                { "env", QJsonObject{{ "client", "optional" }, { "server", "required" }} },
            },
        };
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("mrpack/modrinth.index.json"),
            QJsonDocument(QJsonObject{
                { "formatVersion", 1 },
                { "game", "minecraft" },
                { "files", files },
            }).toJson()));

        QStringList warnings;
        QString error;
        QVERIFY2(ServerModpackInstaller::prepareContent(
                     instanceRoot, gameRoot, root.filePath("prepared"), nullptr,
                     &error, &warnings), qPrintable(error));
        QVERIFY(warnings.join('\n').contains("server-required.jar"));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.21.1", "0.16.10", {}, {}, {});
        const auto repairable =
            ServerModpackInstaller::createMatchingServer(
                &manager, profile, instanceRoot, gameRoot, "Repairable Server");
        QVERIFY2(repairable.isValid(), qPrintable(repairable.error));
        QVERIFY(repairable.warnings.join('\n').contains("server-required.jar"));
        QCOMPARE(repairable.missingFiles,
                 QStringList({ "mods/server-required.jar" }));
        const auto repairableServer = manager.getServer(repairable.serverId);
        QVERIFY(repairableServer);
        QVERIFY(ServerProperties::worldSetupIssue(repairableServer->serverDirectory())
                    .contains("server-required.jar"));
        QCOMPARE(manager.serverCount(), 1);

        QVERIFY(writeFile(
            QDir(instanceRoot).filePath(
                "mrpack/server-files/mods/server-required.jar"),
            "server"));
        error.clear();
        QVERIFY2(ServerModpackInstaller::prepareContent(
                     instanceRoot, gameRoot, root.filePath("prepared"),
                     nullptr, &error),
                 qPrintable(error));
        QVERIFY(QFileInfo::exists(
            root.filePath("prepared/mods/server-required.jar")));
        QVERIFY(ServerProperties::worldSetupIssue(root.filePath("prepared")).isEmpty());
    }

    void usesPublishedServerPackAsAuthoritativeContent()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        const QString destination = root.filePath("prepared");

        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/client-only.jar"),
                          "client"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath(
                "server-pack/server-files/Published Pack/mods/server.jar"),
            "server"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath(
                "server-pack/server-files/Published Pack/config/server.toml"),
            "config"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath(
                "server-pack/server-files/Published Pack/future-provider-data/rules.json"),
            "{}"));
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath(
                "server-pack/published-server-pack.txt"),
            "technic\n"));

        QString error;
        QVERIFY2(ServerModpackInstaller::prepareContent(
                     instanceRoot, gameRoot, destination, nullptr, &error),
                 qPrintable(error));
        QVERIFY(QFileInfo::exists(
            QDir(destination).filePath("mods/server.jar")));
        QVERIFY(QFileInfo::exists(
            QDir(destination).filePath("config/server.toml")));
        QVERIFY(QFileInfo::exists(
            QDir(destination).filePath("future-provider-data/rules.json")));
        QVERIFY(!QFileInfo::exists(
            QDir(destination).filePath("mods/client-only.jar")));
    }

    void acceptsPublishedBootstrapScriptWithoutServerJar()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(QDir().mkpath(gameRoot));
        QVERIFY(writeFile(QDir(instanceRoot).filePath(
                              "server-pack/published-server-pack.txt"),
                          "curseforge\n"));
        QVERIFY(writeFile(QDir(instanceRoot).filePath(
                              "server-pack/server-files/start.bat"),
                          "@echo off\r\n"));

        QCOMPARE(ServerModpackInstaller::publishedServerRootChoices(instanceRoot),
                 QStringList({ "." }));
        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", {}, "47.4.20", {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Bootstrap Pack");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QVERIFY(QFileInfo::exists(
            QDir(server->serverDirectory()).filePath("start.bat")));
        QVERIFY(!QFileInfo::exists(server->serverJarPath()));
    }

    void publishedServerPackKeepsClientLabelledFiles()
    {
        for (const QString &serverRoot : { QStringLiteral("server-pack/server-files"),
                                           QStringLiteral("server-pack/server-files/RLCraft Server") }) {
            QTemporaryDir temporaryRoot;
            QVERIFY(temporaryRoot.isValid());
            const QDir root(temporaryRoot.path());
            const QString instanceRoot = root.filePath("instance");
            const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
            const QByteArray bqTweaker("published BQTweaker bytes");
            const QByteArray legendaryTooltips("published LegendaryTooltips bytes");
            QVERIFY(writeFile(QDir(gameRoot).filePath("mods/BQTweaker.jar"),
                              "client BQTweaker bytes"));
            QVERIFY(writeFile(QDir(gameRoot).filePath("mods/LegendaryTooltips.jar"),
                              "client LegendaryTooltips bytes"));
            QVERIFY(writeFile(QDir(instanceRoot).filePath(
                                  "mrpack/modrinth.index.json"),
                              QJsonDocument(QJsonObject{
                                  { "formatVersion", 1 },
                                  { "game", "minecraft" },
                                  { "dependencies", QJsonObject{
                                      { "minecraft", "1.20.1" },
                                      { "fabric-loader", "0.15.11" },
                                  } },
                                  { "files", QJsonArray{
                                      QJsonObject{
                                          { "path", "mods/BQTweaker.jar" },
                                          { "env", QJsonObject{
                                              { "client", "required" },
                                              { "server", "unsupported" },
                                          } },
                                      },
                                      QJsonObject{
                                          { "path", "mods/LegendaryTooltips.jar" },
                                          { "env", QJsonObject{
                                              { "client", "required" },
                                              { "server", "unsupported" },
                                          } },
                                      },
                                  } },
                              }).toJson()));
            QVERIFY(writeFile(QDir(instanceRoot).filePath(
                                  "server-pack/published-server-pack.txt"),
                              "curseforge\n"));
            QVERIFY(writeFile(QDir(instanceRoot).filePath(
                                  serverRoot + "/mods/BQTweaker.jar"), bqTweaker));
            QVERIFY(writeFile(QDir(instanceRoot).filePath(
                                  serverRoot + "/mods/LegendaryTooltips.jar"),
                              legendaryTooltips));

            ServerManager manager(root.filePath("server-data"));
            const auto profile = ServerModpackInstaller::profileForVersions(
                "1.20.1", "0.15.11", {}, {}, {});
            const auto result = ServerModpackInstaller::createMatchingServer(
                &manager, profile, instanceRoot, gameRoot, "RLCraft Server");
            QVERIFY2(result.isValid(), qPrintable(result.error));
            QVERIFY(result.hasDedicatedServerPack);
            QVERIFY(result.skippedClientFiles.isEmpty());
            QVERIFY(std::any_of(
                result.warnings.cbegin(), result.warnings.cend(),
                [](const QString &warning) {
                    return warning.contains("marked game-only", Qt::CaseInsensitive)
                        && warning.contains("supplied server pack", Qt::CaseInsensitive);
                }));
            QVERIFY(std::none_of(
                result.warnings.cbegin(), result.warnings.cend(),
                [](const QString &warning) {
                    return warning.contains("hash", Qt::CaseInsensitive);
                }));

            const auto server = manager.getServer(result.serverId);
            QVERIFY(server);
            QCOMPARE(server->status(), ServerStatus::Stopped);
            for (const auto &expected : {
                     std::pair{ QStringLiteral("BQTweaker.jar"), bqTweaker },
                     std::pair{ QStringLiteral("LegendaryTooltips.jar"), legendaryTooltips },
                 }) {
                QFile retained(QDir(server->modsDirectory()).filePath(expected.first));
                QVERIFY(retained.open(QIODevice::ReadOnly));
                QCOMPARE(retained.readAll(), expected.second);
            }
        }
    }

    void createsPublishedFabricServerWhenDependencyCheckIsInconclusive()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(QDir().mkpath(gameRoot));
        const QString sourcePath = QDir(instanceRoot).filePath(
            "server-pack/server-files/mods/connected-glass.jar");
        QVERIFY(writeFabricModJar(
            sourcePath, "connectedglass", "*", QJsonObject{{ "fusion", ">=1.2.9" }}));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray sourceBytes = source.readAll();
        QVERIFY(writeFile(QDir(instanceRoot).filePath(
                              "server-pack/published-server-pack.txt"),
                          "curseforge\n"));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot,
            "Published Fabric Dependency Check");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(result.hasDedicatedServerPack);
        const auto warning = std::find_if(
            result.warnings.cbegin(), result.warnings.cend(),
            [](const QString &candidate) {
                return candidate.contains("could not confirm", Qt::CaseInsensitive)
                    && candidate.contains("fusion");
            });
        QVERIFY(warning != result.warnings.cend());
        QVERIFY(warning->startsWith("The launcher could not confirm"));
        QVERIFY(warning->contains("server was created", Qt::CaseInsensitive));
        QVERIFY(warning->contains("loader will check", Qt::CaseInsensitive));

        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QCOMPARE(server->status(), ServerStatus::Stopped);
        QFile installed(QDir(server->modsDirectory()).filePath("connected-glass.jar"));
        QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(), sourceBytes);
    }

    void createsPublishedForgeServerWhenDeclaredVersionIsIncompatible()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(QDir().mkpath(gameRoot));
        const QString examplePath = QDir(instanceRoot).filePath(
            "server-pack/server-files/mods/example.jar");
        QVERIFY(writeForgeModJar(
            examplePath, "META-INF/mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[47,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"example\"\n"
                "version=\"1.0.0\"\n"
                "displayName=\"Example\"\n"
                "[[dependencies.example]]\n"
                "modId=\"requiredlib\"\n"
                "mandatory=true\n"
                "versionRange=\"[2,)\"\n"
                "side=\"SERVER\"\n")));
        QVERIFY(writeForgeModJar(
            QDir(instanceRoot).filePath(
                "server-pack/server-files/mods/required-library.jar"),
            "META-INF/mods.toml",
            QByteArrayLiteral(
                "modLoader=\"javafml\"\n"
                "loaderVersion=\"[47,)\"\n"
                "license=\"Test\"\n"
                "[[mods]]\n"
                "modId=\"requiredlib\"\n"
                "version=\"1.0.0\"\n"
                "displayName=\"Required Library\"\n")));
        QFile source(examplePath);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray sourceBytes = source.readAll();
        QVERIFY(writeFile(QDir(instanceRoot).filePath(
                              "server-pack/published-server-pack.txt"),
                          "curseforge\n"));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", {}, "47.1.0", {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot,
            "Published Forge Dependency Check");
        QVERIFY2(result.isValid(), qPrintable(result.error));
        const auto warning = std::find_if(
            result.warnings.cbegin(), result.warnings.cend(),
            [](const QString &candidate) {
                return candidate.contains("could not confirm", Qt::CaseInsensitive)
                    && candidate.contains("requiredlib")
                    && candidate.contains("installed version 1.0.0");
            });
        QVERIFY(warning != result.warnings.cend());
        QVERIFY(warning->startsWith("The launcher could not confirm"));
        QVERIFY(warning->contains("Details:"));

        const auto server = manager.getServer(result.serverId);
        QVERIFY(server);
        QCOMPARE(server->status(), ServerStatus::Stopped);
        QFile installed(QDir(server->modsDirectory()).filePath("example.jar"));
        QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(), sourceBytes);
    }

    void rejectsPublishedServerPackUnsafeProviderPath()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeFile(QDir(instanceRoot).filePath(
                              "server-pack/published-server-pack.txt"),
                          "modrinth\n"));
        QVERIFY(writeFile(QDir(instanceRoot).filePath(
                              "mrpack/modrinth.index.json"),
                          QJsonDocument(QJsonObject{
                              { "formatVersion", 1 },
                              { "game", "minecraft" },
                              { "dependencies", QJsonObject{
                                  { "minecraft", "1.20.1" },
                                  { "fabric-loader", "0.15.11" },
                              } },
                              { "files", QJsonArray{
                                  QJsonObject{{ "path", "../outside.jar" }},
                              } },
                          }).toJson()));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot,
            "Unsafe Published Server Pack");
        QVERIFY(!result.isValid());
        QCOMPARE(result.failureCategory,
                 ServerModpackFailureCategory::CompatibilityMetadata);
        QCOMPARE(result.failureStage,
                 ServerModpackFailureStage::CompatibilityCheck);
        QVERIFY(result.error.contains("unsafe server file path", Qt::CaseInsensitive));
        QCOMPARE(manager.serverCount(), 0);
    }

    void rejectsPublishedServerPackMissingDownloadedContent()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(QDir().mkpath(gameRoot));
        QVERIFY(writeFile(QDir(instanceRoot).filePath(
                              "server-pack/published-server-pack.txt"),
                          "curseforge\n"));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.20.1", "0.15.11", {}, {}, {});
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot,
            "Missing Published Server Pack");
        QVERIFY(!result.isValid());
        QCOMPARE(result.failureCategory,
                 ServerModpackFailureCategory::ContentProjection);
        QCOMPARE(result.failureStage,
                 ServerModpackFailureStage::ContentPreparation);
        QVERIFY(result.error.contains("not extracted", Qt::CaseInsensitive));
        QCOMPARE(manager.serverCount(), 0);
    }

    void publishedDependencyInspectionLimitsAreAdvisoryButUnsafePathsFail()
    {
        for (bool unsafePath : { false, true }) {
            QTemporaryDir root;
            QVERIFY(root.isValid());
            const QString instanceRoot = root.filePath("instance");
            const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
            QVERIFY(QDir().mkpath(gameRoot));
            const QString jarPath = QDir(instanceRoot).filePath(
                "server-pack/server-files/mods/nested.jar");
            QVERIFY(writeFile(QDir(instanceRoot).filePath(
                "server-pack/published-server-pack.txt"), "curseforge\n"));
            if (unsafePath) {
                QVERIFY(QDir().mkpath(QFileInfo(jarPath).absolutePath()));
                MMCZip::ArchiveWriter archive(jarPath);
                QVERIFY(archive.open());
                QVERIFY(archive.addFile("fabric.mod.json", QByteArray(
                    R"({"schemaVersion":1,"id":"unsafe","version":"1.0.0","jars":[{"file":"../outside.jar"}]})")));
                QVERIFY(archive.close());
            } else {
                QByteArray nested;
                for (int level = 0; level < 10; ++level) {
                    QVERIFY(writeFabricModJar(jarPath, QString("nested%1").arg(level), "*", {}, nested));
                    QFile jar(jarPath);
                    QVERIFY(jar.open(QIODevice::ReadOnly));
                    nested = jar.readAll();
                }
            }
            ServerManager manager(root.filePath("servers"));
            const auto profile = ServerModpackInstaller::profileForVersions(
                "1.20.1", "0.15.11", {}, {}, {});
            const auto result = ServerModpackInstaller::createMatchingServer(
                &manager, profile, instanceRoot, gameRoot, "Nested metadata");
            QCOMPARE(result.isValid(), !unsafePath);
            if (unsafePath) {
                QVERIFY(result.error.contains("unsafe bundled dependency path"));
                QCOMPARE(manager.serverCount(), 0);
            } else {
                QVERIFY(result.warnings.join('\n').contains("eight nested"));
                QCOMPARE(manager.getServer(result.serverId)->status(), ServerStatus::Stopped);
            }
        }
    }
};

QTEST_GUILESS_MAIN(ServerModpackPrepTest)

#include "ServerModpackPrep_test.moc"
