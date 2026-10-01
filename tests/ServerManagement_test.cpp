// SPDX-License-Identifier: GPL-3.0-only

// Managing servers: saving, deleting, backups, automation, recording and player access.

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

class ServerManagementTest : public QObject {
    Q_OBJECT

   private slots:
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

    void keepsRecordsOnlyWhileAServerCanComeBack()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString records = QDir(temporaryRoot.path()).filePath("server-records");
        const auto recordFile = [&records](const QString& id) {
            return QDir(records).filePath(id + QStringLiteral(".json"));
        };

        QString keptId;
        {
            ServerManager manager(temporaryRoot.path());
            const auto kept = manager.createServer("Kept server", "1.21.8");
            const auto gone = manager.createServer("Permanently deleted", "1.21.8");
            QVERIFY(kept && gone);
            keptId = kept->id();
            QVERIFY(manager.dataStore().setValue(keptId, ServerDataGroup::Automation, "enabled", true));
            QVERIFY(manager.dataStore().setValue(gone->id(), ServerDataGroup::Automation, "enabled", true));

            // Deleting for good removes the records with the server.
            const QString goneId = gone->id();
            QVERIFY(manager.deleteServerPermanently(goneId));
            QVERIFY(!QFileInfo::exists(recordFile(goneId)));

            // Records of a server that was deleted earlier and can no longer be undone.
            QVERIFY(manager.dataStore().setValue("server-from-an-earlier-session",
                                                 ServerDataGroup::Automation, "enabled", true));
            QVERIFY(manager.save());
        }
        QVERIFY(QFileInfo::exists(recordFile("server-from-an-earlier-session")));

        ServerManager reopened(temporaryRoot.path());
        QVERIFY(reopened.load());
        QVERIFY(!QFileInfo::exists(recordFile("server-from-an-earlier-session")));
        QVERIFY(QFileInfo::exists(recordFile(keptId)));
        QVERIFY(reopened.dataStore().value(keptId, ServerDataGroup::Automation, "enabled").toBool());

        // A server in the Recycle Bin keeps its records while Undo Delete can still restore it.
        if (trashIsUnavailable(temporaryRoot.path())) {
            QSKIP("This environment has no supported desktop trash service.");
        }
        QVERIFY(reopened.deleteServer(keptId));
        QVERIFY(reopened.load());
        QVERIFY(QFileInfo::exists(recordFile(keptId)));
        QVERIFY(reopened.restoreLastDeletedServer());
        QVERIFY(reopened.dataStore().value(keptId, ServerDataGroup::Automation, "enabled").toBool());
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

    void permanentDeletionReleasesServerInstance()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerManager manager(temporaryRoot.path());
        std::weak_ptr<ServerInstance> weakServer;
        QString serverId;
        {
            const auto server = manager.createServer("Released server", "1.21.8");
            QVERIFY(server);
            serverId = server->id();
            weakServer = server;
        }

        QVERIFY(!weakServer.expired());
        QVERIFY(manager.deleteServerPermanently(serverId));
        QVERIFY(weakServer.expired());
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

    void restoreRollsBackWhenClearingFails()
    {
#ifndef Q_OS_WIN
        QSKIP("Locked-file deletion behavior is Windows-specific.");
#endif
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Locked restore server", "1.21.8");
        QVERIFY(server);
        const QDir serverDirectory(server->serverDirectory());
        QVERIFY(writeFile(serverDirectory.filePath("a-world/level.dat"), "original world"));
        QVERIFY(writeFile(serverDirectory.filePath("z-locked.txt"), "locked original"));

        QString error;
        ServerBackupInfo backup;
        QVERIFY2(manager.createServerBackup(server->id(), "Restore source", &backup, &error),
                 qPrintable(error));
        QVERIFY(backup.valid);

        QFile lockedFile(serverDirectory.filePath("z-locked.txt"));
        QVERIFY(lockedFile.open(QIODevice::ReadOnly));
        QVERIFY(!manager.restoreServerBackup(server->id(), backup.path, &error));
        QVERIFY(error.contains("automatic", Qt::CaseInsensitive));
        QVERIFY(error.contains("backup", Qt::CaseInsensitive));
        QFile restoredWorld(serverDirectory.filePath("a-world/level.dat"));
        QVERIFY(restoredWorld.open(QIODevice::ReadOnly));
        QCOMPARE(restoredWorld.readAll(), QByteArray("original world"));
        lockedFile.close();
    }

    void restoreResynchronizesServerPort()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Port restore server", "1.21.8");
        QVERIFY(server);
        QVERIFY(writeFile(server->serverPropertiesPath(), "server-port=25570\n"));
        server->syncPortFromServerProperties();
        QCOMPARE(server->port(), 25570);

        QString error;
        ServerBackupInfo backup;
        QVERIFY2(manager.createServerBackup(server->id(), "Port checkpoint", &backup, &error),
                 qPrintable(error));
        QVERIFY(backup.valid);
        server->setPort(25565);
        QCOMPARE(server->port(), 25565);

        QVERIFY2(manager.restoreServerBackup(server->id(), backup.path, &error),
                 qPrintable(error));
        QCOMPARE(server->port(), 25570);

        ServerManager reloaded(temporaryRoot.path());
        QVERIFY(reloaded.load());
        const auto restoredServer = reloaded.getServer(server->id());
        QVERIFY(restoredServer);
        QCOMPARE(restoredServer->port(), 25570);
    }

    void runsScheduledAutomationWithoutTheServerWindow()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        struct ApplicationSettingsNames {
            QString organization;
            QString application;
            ~ApplicationSettingsNames()
            {
                QCoreApplication::setOrganizationName(organization);
                QCoreApplication::setApplicationName(application);
            }
        } previousNames{ QCoreApplication::organizationName(),
                         QCoreApplication::applicationName() };
        QCoreApplication::setOrganizationName(QStringLiteral("JLauncherAutomationTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("ServerManager_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Automation server", "1.21.8");
        QVERIFY(server);
        QVERIFY(writeFile(QDir(server->serverDirectory()).filePath("world/level.dat"),
                          "synthetic world"));

        ServerDataStore &records = manager.dataStore();
        const QString &automation = ServerDataGroup::Automation;
        QVERIFY(records.setValue(server->id(), automation, "enabled", true));
        QVERIFY(records.setValue(server->id(), automation, "action", "backup"));
        QVERIFY(records.setValue(server->id(), automation, "time", "12:34"));
        QVERIFY(records.setValue(server->id(), automation, "retention", 0));

        QSignalSpy recorded(&manager, &ServerManager::automationRecorded);
        const QDateTime scheduledTime(QDate(2026, 9, 29), QTime(12, 34));
        manager.runDueAutomations(scheduledTime);
        QCOMPARE(recorded.size(), 1);
        QVERIFY(!manager.listServerBackups(server->id()).isEmpty());
        QCOMPARE(records.value(server->id(), automation, "lastRun").toString(),
                 scheduledTime.date().toString(Qt::ISODate));
        const QStringList firstHistory = records.list(server->id(), automation, "history");
        QCOMPARE(firstHistory.size(), 1);
        QVERIFY(firstHistory.first().contains("BACKUP"));
        const qsizetype backupCount = manager.listServerBackups(server->id()).size();

        manager.runDueAutomations(scheduledTime);
        QCOMPARE(recorded.size(), 1);
        QCOMPARE(records.list(server->id(), automation, "history").size(), 1);
        QCOMPARE(manager.listServerBackups(server->id()).size(), backupCount);
    }

    void recordsCrashAndPlayerHistoryWithoutServerWindow()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        struct ApplicationSettingsNames {
            QString organization;
            QString application;
            ~ApplicationSettingsNames()
            {
                QCoreApplication::setOrganizationName(organization);
                QCoreApplication::setApplicationName(application);
            }
        } previousNames{ QCoreApplication::organizationName(),
                         QCoreApplication::applicationName() };
        QCoreApplication::setOrganizationName(QStringLiteral("JLauncherServerRecordingTests"));
        QCoreApplication::setApplicationName(QStringLiteral("ServerManager_%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        struct SettingsCleanup {
            QString diagnosticsPrefix;
            QString historyPrefix;
            ~SettingsCleanup()
            {
                QSettings settings;
                settings.remove(diagnosticsPrefix);
                settings.remove(historyPrefix);
                settings.sync();
            }
        } cleanup;

        const QByteArray priorJavaMajor = qgetenv("JLAUNCHER_FAKE_JAVA_MAJOR");
        const bool hadJavaMajor = qEnvironmentVariableIsSet("JLAUNCHER_FAKE_JAVA_MAJOR");
        qputenv("JLAUNCHER_FAKE_JAVA_MAJOR", "21");
        struct JavaMajorCleanup {
            QByteArray value;
            bool wasSet = false;
            ~JavaMajorCleanup()
            {
                if (wasSet) qputenv("JLAUNCHER_FAKE_JAVA_MAJOR", value);
                else qunsetenv("JLAUNCHER_FAKE_JAVA_MAJOR");
            }
        } javaCleanup{ priorJavaMajor, hadJavaMajor };

        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Recorded crash", "1.21.8");
        QVERIFY(server);
        server->setVersion("1.21.8");
        server->setPort(unusedPort());
        server->setEulaAccepted(true);
        server->setMinMemory(1024);
        server->setMaxMemory(2048);
        server->setJavaPath(fakeMinecraftServerPath());
        server->setExtraJvmArguments(QStringLiteral("-Dfake.crash-memory"));
        QVERIFY(server->port() != 0);
        QVERIFY(QFileInfo::exists(server->javaPath()));
        QVERIFY(writeSyntheticJar(server->serverJarPath()));

        cleanup.diagnosticsPrefix = QStringLiteral("ServerDiagnostics/%1/").arg(server->id());
        cleanup.historyPrefix = QStringLiteral("ServerPlayerHistory/%1/").arg(server->id());
        QSettings settings;
        settings.remove(cleanup.diagnosticsPrefix);
        settings.remove(cleanup.historyPrefix);
        QSignalSpy crashRecorded(&manager, &ServerManager::serverDiagnosticsRecorded);
        QSignalSpy playerRecorded(&manager, &ServerManager::playerHistoryRecorded);

        QVERIFY(server->start());
        QTRY_COMPARE_WITH_TIMEOUT(server->status(), ServerStatus::Error, 5000);
        QCOMPARE(crashRecorded.size(), 1);
        QVERIFY(manager.dataStore()
                    .value(server->id(), ServerDataGroup::Diagnostics, QStringLiteral("lastCrash"))
                    .toString().contains(QStringLiteral("OutOfMemoryError")));

        emit server->playerActivity(QStringLiteral("FixturePlayer"), true);
        QCOMPARE(playerRecorded.size(), 1);
        const QStringList events = manager.dataStore().list(
            server->id(), ServerDataGroup::PlayerHistory, QStringLiteral("events"));
        QCOMPARE(events.size(), 1);
        QVERIFY(events.first().contains(QStringLiteral("FixturePlayer joined")));
    }

    void preparesModpackOnWorkerAndInstallsPreparedFiles()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString instanceRoot = temporaryRoot.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeFile(QDir(gameRoot).filePath("config/server.toml"), "server-config"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("server.properties"), "motd=fixture"));
        const ServerModpackProfile profile = ServerModpackInstaller::profileForVersions(
            "1.21.1", "0.16.10", {}, {}, {});
        QVERIFY(profile.isValid());

        ServerManager manager(temporaryRoot.filePath("manager-data"));
        const QString stagingRoot = manager.serversRoot();
        const auto preparedFuture = QtConcurrent::run([=]() {
            return ServerModpackInstaller::prepareMatchingServer(
                profile, instanceRoot, gameRoot, stagingRoot);
        });
        PreparedServerModpack prepared = preparedFuture.result();
        QVERIFY2(prepared.isReady(), qPrintable(prepared.result.error));
        const auto installed = ServerModpackInstaller::installPreparedServer(
            &manager, std::move(prepared), "Prepared server");
        QVERIFY2(installed.isValid(), qPrintable(installed.error));

        ServerManager synchronousManager(temporaryRoot.filePath("synchronous-data"));
        const auto synchronous = ServerModpackInstaller::createMatchingServer(
            &synchronousManager, profile, instanceRoot, gameRoot, "Synchronous server");
        QVERIFY2(synchronous.isValid(), qPrintable(synchronous.error));
        const QString installedRoot =
            manager.getServer(installed.serverId)->serverDirectory();
        const QString synchronousRoot =
            synchronousManager.getServer(synchronous.serverId)->serverDirectory();
        QStringList installedFiles;
        QStringList synchronousFiles;
        QDirIterator installedIterator(installedRoot, QDir::Files | QDir::NoDotAndDotDot,
                                       QDirIterator::Subdirectories);
        while (installedIterator.hasNext()) {
            installedIterator.next();
            installedFiles.append(QDir(installedRoot).relativeFilePath(
                installedIterator.filePath()));
        }
        QDirIterator synchronousIterator(synchronousRoot, QDir::Files | QDir::NoDotAndDotDot,
                                         QDirIterator::Subdirectories);
        while (synchronousIterator.hasNext()) {
            synchronousIterator.next();
            synchronousFiles.append(QDir(synchronousRoot).relativeFilePath(
                synchronousIterator.filePath()));
        }
        installedFiles.sort();
        synchronousFiles.sort();
        QCOMPARE(installedFiles, synchronousFiles);
        for (const QString &relativePath : installedFiles) {
            QFile installedFile(QDir(installedRoot).filePath(relativePath));
            QFile synchronousFile(QDir(synchronousRoot).filePath(relativePath));
            QVERIFY(installedFile.open(QIODevice::ReadOnly));
            QVERIFY(synchronousFile.open(QIODevice::ReadOnly));
            QCOMPARE(installedFile.readAll(), synchronousFile.readAll());
        }
    }

    void failedPreparedInstallCleansPreparingDirectory()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString instanceRoot = temporaryRoot.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        QVERIFY(writeFile(QDir(gameRoot).filePath("config/server.toml"), "server-config"));
        const ServerModpackProfile profile = ServerModpackInstaller::profileForVersions(
            "1.21.1", "0.16.10", {}, {}, {});
        const QString stagingRoot = temporaryRoot.filePath("servers");
        PreparedServerModpack prepared = ServerModpackInstaller::prepareMatchingServer(
            profile, instanceRoot, gameRoot, stagingRoot);
        QVERIFY2(prepared.isReady(), qPrintable(prepared.result.error));
        QVERIFY(QFileInfo::exists(prepared.preparedDirectory));
        const auto failed = ServerModpackInstaller::installPreparedServer(
            nullptr, std::move(prepared), "No manager");
        QVERIFY(!failed.isValid());
        QCOMPARE(QDir(stagingRoot).entryList(
                     QStringList{ QStringLiteral(".preparing-*") },
                     QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
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

QTEST_GUILESS_MAIN(ServerManagementTest)

#include "ServerManagement_test.moc"
