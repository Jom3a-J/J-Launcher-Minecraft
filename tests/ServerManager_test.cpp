// SPDX-License-Identifier: GPL-3.0-only

#include <QDir>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
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

namespace {
bool writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

bool writeSyntheticJar(const QString& path)
{
    MMCZip::ArchiveWriter archive(path);
    return archive.open()
        && archive.addFile("META-INF/MANIFEST.MF",
                           QByteArray("Main-Class: net.minecraft.bundler.Main\n"))
        && archive.close();
}

bool writeFabricModJar(const QString& path, const QString& id,
                       const QString& environment,
                       const QJsonObject& dependencies = {},
                       const QByteArray& nestedJar = {})
{
    if (!QDir().mkpath(QFileInfo(path).dir().absolutePath())) {
        return false;
    }
    QJsonObject metadataObject{
        { "schemaVersion", 1 },
        { "id", id },
        { "version", "1.0.0" },
        { "environment", environment },
    };
    if (!dependencies.isEmpty()) {
        metadataObject.insert("depends", dependencies);
    }
    if (!nestedJar.isEmpty()) {
        metadataObject.insert(
            "jars", QJsonArray{ QJsonObject{{ "file", "META-INF/jars/nested.jar" }} });
    }
    MMCZip::ArchiveWriter archive(path);
    const QByteArray metadata =
        QJsonDocument(metadataObject).toJson(QJsonDocument::Compact);
    if (!archive.open() || !archive.addFile("fabric.mod.json", metadata)) {
        return false;
    }
    if (!nestedJar.isEmpty()
        && !archive.addFile("META-INF/jars/nested.jar", nestedJar)) {
        return false;
    }
    return archive.close();
}

bool writeForgeModJar(const QString& path, const QString& metadataPath,
                      const QByteArray& metadata,
                      const QByteArray& manifestVersion = {})
{
    if (!QDir().mkpath(QFileInfo(path).dir().absolutePath())) {
        return false;
    }
    MMCZip::ArchiveWriter archive(path);
    if (!archive.open() || !archive.addFile(metadataPath, metadata)) {
        return false;
    }
    if (!manifestVersion.isEmpty()
        && !archive.addFile(
            "META-INF/MANIFEST.MF",
            QByteArray("Manifest-Version: 1.0\nImplementation-Version: ")
                + manifestVersion + '\n')) {
        return false;
    }
    return archive.close();
}

bool trashIsUnavailable(const QString& temporaryRoot)
{
    const QString probePath = QDir(temporaryRoot).filePath("trash-capability-probe");
    if (!QDir().mkpath(probePath)) {
        return true;
    }

    QString pathInTrash;
    if (!FS::trash(probePath, &pathInTrash)) {
        QDir(probePath).removeRecursively();
        return true;
    }

    // Avoid leaving the capability probe in the user's trash when this test
    // runs locally.
    if (!pathInTrash.isEmpty()) {
        if (!QFile(pathInTrash).rename(probePath)) {
            QDir(pathInTrash).removeRecursively();
        }
    }
    QDir(probePath).removeRecursively();
    return false;
}
}

class ServerManagerTest : public QObject {
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

    void rejectsIncompleteProviderServerManifest()
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

        QString error;
        QVERIFY(!ServerModpackInstaller::prepareContent(
            instanceRoot, gameRoot, root.filePath("prepared"), nullptr, &error));
        QVERIFY(error.contains("provider-declared", Qt::CaseInsensitive));
        QVERIFY(error.contains("future-provider-data/required.dat"));
    }

    void rejectsMissingRequiredServerOnlyFile()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(QDir().mkpath(gameRoot));

        const QJsonArray files{
            QJsonObject{
                { "path", "mods/server-required.jar" },
                { "env", QJsonObject{{ "client", "unsupported" }, { "server", "required" }} },
            },
        };
        QVERIFY(writeFile(
            QDir(instanceRoot).filePath("mrpack/modrinth.index.json"),
            QJsonDocument(QJsonObject{
                { "formatVersion", 1 },
                { "game", "minecraft" },
                { "files", files },
            }).toJson()));

        QString error;
        QVERIFY(!ServerModpackInstaller::prepareContent(
            instanceRoot, gameRoot, root.filePath("prepared"), nullptr, &error));
        QVERIFY(error.contains("server-only", Qt::CaseInsensitive));
        QVERIFY(error.contains("server-required.jar"));

        ServerManager manager(root.filePath("server-data"));
        const auto profile = ServerModpackInstaller::profileForVersions(
            "1.21.1", "0.16.10", {}, {}, {});
        const auto failedTransaction =
            ServerModpackInstaller::createMatchingServer(
                &manager, profile, instanceRoot, gameRoot, "Must Roll Back");
        QVERIFY(!failedTransaction.isValid());
        QCOMPARE(failedTransaction.failureCategory,
                 ServerModpackFailureCategory::ContentProjection);
        QCOMPARE(failedTransaction.failureStage,
                 ServerModpackFailureStage::ContentPreparation);
        QCOMPARE(manager.serverCount(), 0);

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
        const QString trackingPrefix =
            QString("ServerContentSources/%1/").arg(server->id());
        QCOMPARE(QSettings().value(trackingPrefix + "common.jar").toString(),
                 QString("curseforge:123:456"));
        QSettings().remove(trackingPrefix);
        QCoreApplication::setOrganizationName(previousOrganization);
        QCoreApplication::setApplicationName(previousApplication);
        QVERIFY(!QFileInfo::exists(server->serverJarPath()));
    }

    void rejectsDerivedFabricServerWithMissingDependency()
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
        QVERIFY(!result.isValid());
        QVERIFY(result.error.contains("no dedicated server version",
                                      Qt::CaseInsensitive));
        QVERIFY(result.error.contains("connectedglass"));
        QVERIFY(result.error.contains("fusion"));
        QCOMPARE(result.failureCategory,
                 ServerModpackFailureCategory::DependencyIncompatibility);
        QCOMPARE(result.failureStage,
                 ServerModpackFailureStage::DependencyValidation);
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
        QCOMPARE(manager.serverCount(), 1);
    }

    void rejectsDerivedForgeServerWithMissingDependency()
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
        QVERIFY(!result.isValid());
        QVERIFY(result.error.contains("Forge", Qt::CaseInsensitive));
        QVERIFY(result.error.contains("example"));
        QVERIFY(result.error.contains("requiredlib"));
        QVERIFY(!result.error.contains("clienthelper"));
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
        QVERIFY(!mismatchedResult.isValid());
        QVERIFY(mismatchedResult.error.contains("version", Qt::CaseInsensitive));
        QVERIFY(mismatchedResult.error.contains("[1,)"));
        QVERIFY(mismatchedResult.error.contains("0.5.0"));
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

    void rejectsDerivedNeoForgeServerWithMissingDependency()
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
        QVERIFY(!result.isValid());
        QVERIFY(result.error.contains("NeoForge", Qt::CaseInsensitive));
        QVERIFY(result.error.contains("example"));
        QVERIFY(result.error.contains("requiredlib"));
        QVERIFY(!result.error.contains("clienthelper"));
        QVERIFY(!result.error.contains("optionalhelper"));
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
        QVERIFY(!mismatchedResult.isValid());
        QVERIFY(mismatchedResult.error.contains("version", Qt::CaseInsensitive));
        QVERIFY(mismatchedResult.error.contains("[1,2)"));
        QVERIFY(mismatchedResult.error.contains("2.0.0"));
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
        QVERIFY(!incompatibleResult.isValid());
        QVERIFY(incompatibleResult.error.contains("incompatible", Qt::CaseInsensitive));
        QVERIFY(incompatibleResult.error.contains("badmod"));
        QCOMPARE(manager.serverCount(), 2);
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

    void rejectsAmbiguousPublishedServerRoots()
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
        const auto result = ServerModpackInstaller::createMatchingServer(
            &manager, profile, instanceRoot, gameRoot, "Ambiguous Pack");
        QVERIFY(!result.isValid());
        QCOMPARE(result.failureCategory, ServerModpackFailureCategory::ContentProjection);
        QCOMPARE(result.failureStage, ServerModpackFailureStage::ContentPreparation);
        QVERIFY(result.error.contains("more than one possible", Qt::CaseInsensitive));
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
        QVERIFY(!missing.isValid());
        QCOMPARE(missing.failureCategory, ServerModpackFailureCategory::DependencyIncompatibility);
        QCOMPARE(missing.failureStage, ServerModpackFailureStage::DependencyValidation);
        QVERIFY(missing.error.contains("ponder.jar"));
        QCOMPARE(manager.serverCount(), 1);
    }

    void rejectsDerivedFabricServerWithMissingBundledJar()
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
        QVERIFY(!missing.isValid());
        QCOMPARE(missing.failureCategory, ServerModpackFailureCategory::DependencyIncompatibility);
        QCOMPARE(missing.failureStage, ServerModpackFailureStage::DependencyValidation);
        QVERIFY(missing.error.contains("missing.jar"));
        QCOMPARE(manager.serverCount(), 0);
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

    void persistsAndDeletesManagedServer()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        if (trashIsUnavailable(temporaryRoot.path())) {
            QSKIP("This environment has no supported desktop trash service.");
        }

        QString serverId;
        QString serverDirectory;
        {
            ServerManager manager(temporaryRoot.path());
            const auto server = manager.createServer("Regression server", "1.21.8");
            QVERIFY(server);
            QCOMPARE(server->loaderType(), QString("vanilla"));
            QCOMPARE(manager.serverCount(), 1);
            serverId = server->id();
            serverDirectory = server->serverDirectory();
            QVERIFY(QFileInfo::exists(serverDirectory));
            QVERIFY(manager.save());
        }

        ServerManager reloaded(temporaryRoot.path());
        QVERIFY(reloaded.load());
        QCOMPARE(reloaded.serverCount(), 1);
        const auto restored = reloaded.getServer(serverId);
        QVERIFY(restored);
        QCOMPARE(restored->name(), QString("Regression server"));
        QCOMPARE(restored->version(), QString("1.21.8"));
        QCOMPARE(restored->serverDirectory(), serverDirectory);

        QVERIFY(reloaded.deleteServer(serverId));
        QCOMPARE(reloaded.serverCount(), 0);
        QVERIFY(!QFileInfo::exists(serverDirectory));
    }

    void restoresServerMovedToTrash()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        if (trashIsUnavailable(temporaryRoot.path())) {
            QSKIP("This environment has no supported desktop trash service.");
        }

        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Recoverable server", "1.21.8");
        QVERIFY(server);
        const QString id = server->id();
        const QString originalDirectory = server->serverDirectory();
        QFile marker(QDir(originalDirectory).filePath("marker.txt"));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write("recover me"), qint64(10));
        marker.close();

        QVERIFY(manager.deleteServer(id));
        QVERIFY(manager.hasDeletedServer());
        QVERIFY(!QFileInfo::exists(originalDirectory));

        QString restoredId;
        QVERIFY(manager.restoreLastDeletedServer(&restoredId));
        QCOMPARE(restoredId, id);
        QVERIFY(!manager.hasDeletedServer());
        const auto restored = manager.getServer(id);
        QVERIFY(restored);
        QVERIFY(QFileInfo::exists(QDir(restored->serverDirectory()).filePath("marker.txt")));
    }

    void permanentlyDeletesManagedServer()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        QString serverId;
        QString serverDirectory;
        {
            ServerManager manager(temporaryRoot.path());
            const auto server = manager.createServer("Permanent deletion", "1.21.8");
            QVERIFY(server);
            serverId = server->id();
            serverDirectory = server->serverDirectory();
            QVERIFY(writeFile(QDir(serverDirectory).filePath("marker.txt"), "delete me"));

            QVERIFY(manager.deleteServerPermanently(serverId));
            QCOMPARE(manager.serverCount(), 0);
            QVERIFY(!manager.hasDeletedServer());
            QVERIFY(!QFileInfo::exists(serverDirectory));
        }

        ServerManager reloaded(temporaryRoot.path());
        QVERIFY(reloaded.load());
        QVERIFY(!reloaded.getServer(serverId));
    }

    void stagesBackupRestoreAndCreatesSafetySnapshot()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Backup server", "1.21.8");
        QVERIFY(server);
        const QDir serverDirectory(server->serverDirectory());

        QVERIFY(writeFile(serverDirectory.filePath("world/level.dat"), "original world"));
        QVERIFY(writeFile(serverDirectory.filePath("world/playerdata/player.dat"), "player"));
        QVERIFY(writeFile(serverDirectory.filePath("world/advancements/player.json"), "advancement"));
        QVERIFY(writeFile(serverDirectory.filePath("world/stats/player.json"), "statistics"));
        QVERIFY(writeFile(serverDirectory.filePath("world/dimensions/custom/region/r.0.0.mca"),
                          "custom dimension"));
        QVERIFY(writeFile(serverDirectory.filePath("world_nether/DIM-1/region.mca"), "nether"));
        QVERIFY(writeFile(serverDirectory.filePath("world_the_end/DIM1/region.mca"), "end"));
        QVERIFY(writeFile(serverDirectory.filePath("config/server.toml"), "configuration"));
        QVERIFY(writeFile(serverDirectory.filePath("scripts/startup.js"), "script"));
        QVERIFY(writeFile(serverDirectory.filePath("mods/example.jar"), "mod"));
        QVERIFY(writeFile(serverDirectory.filePath("server.properties"), "motd=Original\n"));
        QVERIFY(writeFile(serverDirectory.filePath("eula.txt"), "eula=true\n"));
        QVERIFY(writeFile(serverDirectory.filePath("whitelist.json"), "[]"));
        QVERIFY(writeFile(serverDirectory.filePath("ops.json"), "[]"));
        QVERIFY(writeFile(serverDirectory.filePath("banned-players.json"), "[]"));
        QVERIFY(writeFile(serverDirectory.filePath("banned-ips.json"), "[]"));
        QVERIFY(writeFile(serverDirectory.filePath("libraries/loader/launch.jar"), "loader"));
        QVERIFY(writeSyntheticJar(server->serverJarPath()));

        QString error;
        ServerBackupInfo created;
        QVERIFY2(manager.createServerBackup(server->id(), "Before update", &created, &error),
                 qPrintable(error));
        QVERIFY(created.valid);
        QCOMPARE(created.serverId, server->id());
        QVERIFY(created.includedCategories.contains("world-data"));
        QVERIFY(created.includedCategories.contains("player-data"));
        QVERIFY(created.includedCategories.contains("configuration"));
        QVERIFY(created.includedCategories.contains("mods"));
        QVERIFY(created.includedCategories.contains("server-runtime"));
        QVERIFY(QFileInfo::exists(QDir(created.path).filePath(".jlauncher-backup.json")));
        QVERIFY(!QFileInfo::exists(QDir(created.path).filePath("backups")));
        QVERIFY(QFileInfo::exists(
            QDir(created.path).filePath("world/dimensions/custom/region/r.0.0.mca")));
        QVERIFY(QFileInfo::exists(QDir(created.path).filePath("world/advancements/player.json")));
        QVERIFY(QFileInfo::exists(QDir(created.path).filePath("world/stats/player.json")));
        QVERIFY(QFileInfo::exists(QDir(created.path).filePath("whitelist.json")));
        QVERIFY(QFileInfo::exists(QDir(created.path).filePath("ops.json")));
        QVERIFY(QFileInfo::exists(QDir(created.path).filePath("banned-players.json")));
        QVERIFY(QFileInfo::exists(QDir(created.path).filePath("banned-ips.json")));
        QVERIFY(QFileInfo::exists(QDir(created.path).filePath("libraries/loader/launch.jar")));

        QVERIFY(writeFile(serverDirectory.filePath("world/level.dat"), "changed world"));
        QVERIFY(writeFile(serverDirectory.filePath("current.txt"), "current state"));
        QVERIFY(QFile::remove(serverDirectory.filePath("mods/example.jar")));
        server->setVersion("1.22");
        server->setLoaderType("paper");
        server->setLoaderVersion("new-build");
        QVERIFY(manager.save());

        QString safetyBackupName;
        QVERIFY2(manager.restoreServerBackup(server->id(), created.path, &error, &safetyBackupName),
                 qPrintable(error));
        QVERIFY(!safetyBackupName.isEmpty());
        QVERIFY(!QFileInfo::exists(serverDirectory.filePath("current.txt")));
        QVERIFY(QFileInfo::exists(serverDirectory.filePath("mods/example.jar")));
        QFile restoredWorld(serverDirectory.filePath("world/level.dat"));
        QVERIFY(restoredWorld.open(QIODevice::ReadOnly));
        QCOMPARE(restoredWorld.readAll(), QByteArray("original world"));
        QVERIFY(QFileInfo::exists(
            serverDirectory.filePath("backups/" + safetyBackupName + "/current.txt")));
        QVERIFY(QFileInfo::exists(
            serverDirectory.filePath("backups/" + safetyBackupName + "/.jlauncher-backup.json")));
        QCOMPARE(server->version(), created.minecraftVersion);
        QCOMPARE(server->loaderType(), created.loaderType);
        QCOMPARE(server->loaderVersion(), created.loaderVersion);

        const QList<ServerBackupInfo> backups = manager.listServerBackups(server->id());
        QVERIFY(backups.size() >= 2);
        QVERIFY(manager.deleteServerBackup(server->id(), created.path, &error));
        QVERIFY(!QFileInfo::exists(created.path));
    }

    void rejectsExternalAndInvalidBackups()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Safe restore server", "1.21.8");
        QVERIFY(server);

        QString error;
        const QString external = temporaryRoot.filePath("external-backup");
        QVERIFY(writeFile(QDir(external).filePath("world/level.dat"), "external"));
        QVERIFY(!manager.restoreServerBackup(server->id(), external, &error));
        QVERIFY(error.contains("outside"));
        error.clear();
        QVERIFY(!manager.deleteServerBackup(server->id(), external, &error));
        QVERIFY(error.contains("outside"));

        const QString invalid =
            QDir(server->serverDirectory()).filePath("backups/server-invalid");
        QVERIFY(writeFile(QDir(invalid).filePath(".jlauncher-backup.json"), "{}"));
        error.clear();
        QVERIFY(!manager.restoreServerBackup(server->id(), invalid, &error));
        QVERIFY(error.contains("format"));
        const QList<ServerBackupInfo> backups = manager.listServerBackups(server->id());
        QCOMPARE(backups.size(), 1);
        QVERIFY(!backups.first().valid);
        QVERIFY(!backups.first().validationError.isEmpty());
    }

    void prunesOnlyValidatedAutomaticBackups()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Retention server", "1.21.8");
        QVERIFY(server);
        QVERIFY(writeFile(QDir(server->serverDirectory()).filePath("world/level.dat"),
                          "retention fixture"));

        QString error;
        ServerBackupInfo backup;
        QVERIFY2(manager.createServerBackup(server->id(), "Manual checkpoint", &backup, &error),
                 qPrintable(error));
        for (int index = 1; index <= 4; ++index) {
            QVERIFY2(manager.createServerBackup(
                         server->id(), QString("Automatic backup %1").arg(index),
                         &backup, &error),
                     qPrintable(error));
            QTest::qWait(2);
        }
        const QString invalidPath = QDir(server->serverDirectory()).filePath(
            "backups/server-Automatic-backup-invalid");
        QVERIFY(writeFile(QDir(invalidPath).filePath("world/level.dat"), "invalid"));

        QVERIFY2(manager.enforceServerBackupRetention(
                     server->id(), "Automatic backup ", 2, &error),
                 qPrintable(error));

        int automaticCount = 0;
        int manualCount = 0;
        int invalidCount = 0;
        for (const ServerBackupInfo& retained : manager.listServerBackups(server->id())) {
            if (!retained.valid) {
                ++invalidCount;
            } else if (retained.name.startsWith("Automatic backup ")) {
                ++automaticCount;
            } else if (retained.name == "Manual checkpoint") {
                ++manualCount;
            }
        }
        QCOMPARE(automaticCount, 2);
        QCOMPARE(manualCount, 1);
        QCOMPARE(invalidCount, 1);

        error.clear();
        QVERIFY(!manager.enforceServerBackupRetention(server->id(), QString(), 1, &error));
        QVERIFY(error.contains("invalid"));
    }

    void rejectsSymbolicLinksInsteadOfFollowingExternalData()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Linked server", "1.21.8");
        QVERIFY(server);

        const QString external = temporaryRoot.filePath("external-secret.txt");
        QVERIFY(writeFile(external, "must not be copied"));
        const QString link =
            QDir(server->serverDirectory()).filePath(
#ifdef Q_OS_WIN
                "config/external-secret-link.lnk"
#else
                "config/external-secret-link"
#endif
            );
        QDir().mkpath(QFileInfo(link).dir().absolutePath());
        if (!QFile::link(external, link) || !QFileInfo(link).isSymLink()) {
            QSKIP("Symbolic links are not available in this test environment.");
        }

        QString error;
        ServerBackupInfo backup;
        QVERIFY(!manager.createServerBackup(server->id(), "Unsafe link", &backup, &error));
        QVERIFY(error.contains("symbolic link", Qt::CaseInsensitive));
        QVERIFY(manager.listServerBackups(server->id()).isEmpty());
    }

    void refusesDeletionWhileServerIsActive()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const bool trashUnavailable = trashIsUnavailable(temporaryRoot.path());
        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Active server", "1.21.8");
        QVERIFY(server);

        const QString fakeJava = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
            "FakeMinecraftServer.exe"
#else
            "FakeMinecraftServer"
#endif
        );
        QVERIFY(QFileInfo::exists(fakeJava));
        QVERIFY(writeSyntheticJar(server->serverJarPath()));
        QTcpServer portProbe;
        QVERIFY(portProbe.listen(QHostAddress::LocalHost, 0));
        server->setPort(portProbe.serverPort());
        portProbe.close();
        server->setJavaPath(fakeJava);
        server->setEulaAccepted(true);

        QVERIFY(server->start());
        QTRY_COMPARE_WITH_TIMEOUT(server->status(), ServerStatus::Running, 5000);
        QVERIFY(!manager.deleteServer(server->id()));
        QVERIFY(!manager.deleteServerPermanently(server->id()));
        QCOMPARE(manager.serverCount(), 1);
        QVERIFY(QFileInfo::exists(server->serverDirectory()));

        QVERIFY(server->stop());
        QTRY_COMPARE_WITH_TIMEOUT(server->status(), ServerStatus::Stopped, 5000);
        if (trashUnavailable) {
            QVERIFY(!manager.deleteServer(server->id()));
        } else {
            QVERIFY(manager.deleteServer(server->id()));
        }
    }

    void managesPlayerAccessWithoutOverwritingMalformedData()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Player access server", "1.21.8");
        QVERIFY(server);

        const QString aliceUuid = "11111111-1111-1111-1111-111111111111";
        const QString bobUuid = "22222222-2222-2222-2222-222222222222";
        const QDir directory(server->serverDirectory());
        QVERIFY(writeFile(directory.filePath("usercache.json"),
                          QJsonDocument(QJsonArray{
                              QJsonObject{{"uuid", aliceUuid}, {"name", "Alice"}}
                          }).toJson()));
        QVERIFY(writeFile(directory.filePath("whitelist.json"),
                          QJsonDocument(QJsonArray{
                              QJsonObject{{"uuid", bobUuid}, {"name", "Bob"}}
                          }).toJson()));

        QString error;
        QList<ServerPlayerInfo> players = ServerPlayerAccess::listPlayers(server, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(players.size(), 2);

        QVERIFY2(ServerPlayerAccess::setWhitelisted(
                     server, aliceUuid, "Alice", true, &error), qPrintable(error));
        QVERIFY2(ServerPlayerAccess::setOperator(
                     server, aliceUuid, "Alice", 3, &error), qPrintable(error));
        QVERIFY2(ServerPlayerAccess::setBanned(
                     server, aliceUuid, "Alice", true, "Automated test", &error),
                 qPrintable(error));

        players = ServerPlayerAccess::listPlayers(server, &error);
        const auto alice = std::find_if(players.cbegin(), players.cend(),
                                        [&aliceUuid](const ServerPlayerInfo& player) {
                                            return player.uuid == aliceUuid;
                                        });
        QVERIFY(alice != players.cend());
        QVERIFY(alice->whitelisted);
        QVERIFY(alice->operatorEnabled);
        QCOMPARE(alice->operatorLevel, 3);
        QVERIFY(alice->banned);

        QVERIFY2(ServerPlayerAccess::clearAccess(server, aliceUuid, &error), qPrintable(error));
        players = ServerPlayerAccess::listPlayers(server, &error);
        const auto clearedAlice = std::find_if(players.cbegin(), players.cend(),
                                               [&aliceUuid](const ServerPlayerInfo& player) {
                                                   return player.uuid == aliceUuid;
                                               });
        QVERIFY(clearedAlice != players.cend());
        QVERIFY(!clearedAlice->whitelisted);
        QVERIFY(!clearedAlice->operatorEnabled);
        QVERIFY(!clearedAlice->banned);
        const auto bob = std::find_if(players.cbegin(), players.cend(),
                                      [&bobUuid](const ServerPlayerInfo& player) {
                                          return player.uuid == bobUuid;
                                      });
        QVERIFY(bob != players.cend());
        QVERIFY(bob->whitelisted);

        const QByteArray malformed("{ broken player data");
        QVERIFY(writeFile(directory.filePath("whitelist.json"), malformed));
        error.clear();
        QVERIFY(!ServerPlayerAccess::setWhitelisted(
            server, bobUuid, "Bob", false, &error));
        QVERIFY(error.contains("invalid JSON"));
        QFile unchanged(directory.filePath("whitelist.json"));
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), malformed);

        error.clear();
        QVERIFY(!ServerPlayerAccess::clearAccess(server, QString(), &error));
        QVERIFY(error.contains("UUID"));
    }

    void enforcesSafePlayerOperationsWhileRunning()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Live player server", "1.21.8");
        QVERIFY(server);

        const QString fakeJava = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
            "FakeMinecraftServer.exe"
#else
            "FakeMinecraftServer"
#endif
        );
        QVERIFY(QFileInfo::exists(fakeJava));
        QVERIFY(writeSyntheticJar(server->serverJarPath()));
        QTcpServer portProbe;
        QVERIFY(portProbe.listen(QHostAddress::LocalHost, 0));
        server->setPort(portProbe.serverPort());
        portProbe.close();
        server->setJavaPath(fakeJava);
        server->setEulaAccepted(true);

        QVERIFY(server->start());
        QTRY_COMPARE_WITH_TIMEOUT(server->status(), ServerStatus::Running, 5000);

        QString error;
        QVERIFY(!ServerPlayerAccess::setWhitelisted(
            server, "33333333-3333-3333-3333-333333333333", "TestPlayer", true, &error));
        QVERIFY(error.contains("Stop"));

        error.clear();
        QVERIFY2(server->kickPlayer("TestPlayer", "Removed by automated test", &error),
                 qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(
            server->consoleLog().contains("COMMAND:kick TestPlayer Removed by automated test"),
            5000);
        error.clear();
        QVERIFY(!server->kickPlayer("Bad Player\nstop", "Unsafe", &error));
        QVERIFY(error.contains("name"));

        QVERIFY(server->stop());
        QTRY_COMPARE_WITH_TIMEOUT(server->status(), ServerStatus::Stopped, 5000);
        error.clear();
        QVERIFY(!server->kickPlayer("TestPlayer", QString(), &error));
        QVERIFY(error.contains("running"));
    }
};

QTEST_GUILESS_MAIN(ServerManagerTest)

#include "ServerManager_test.moc"
