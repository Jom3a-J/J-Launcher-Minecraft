// SPDX-License-Identifier: GPL-3.0-only

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>

#include <archive/ArchiveWriter.h>
#include <server/ServerInstance.h>
#include <server/ServerManager.h>
#include <server/ServerModpackInstaller.h>
#include <server/ServerPlayerAccess.h>
#include <FileSystem.h>

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

    void createsMatchingStoppedServerFromInstance()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/common.jar"),
                          "common"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("config/common.toml"),
                          "config"));

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
        QVERIFY(!QFileInfo::exists(server->serverJarPath()));
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
