// SPDX-License-Identifier: GPL-3.0-only

// Server content: mod and plugin updates, local content, and server-pack import.

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <utility>

#include <archive/ArchiveWriter.h>
#include <server/ServerDownloader.h>
#include <server/ServerContentUpdater.h>
#include <server/ServerInstance.h>
#include <server/ServerDiagnostics.h>
#include <server/ServerJvmArgs.h>
#include <java/JavaRuntimeInstallTask.h>
#include <java/JavaUtils.h>
#include <server/ServerManager.h>
#include <server/ServerProperties.h>
#include <net/HostScheduler.h>
#include <net/PartFile.h>
#include <net/SegmentedDownload.h>

#include "RangeHttpServer.h"

#include "ServerTestSupport.h"

using namespace ServerTestSupport;

class ServerContentTest : public QObject {
    Q_OBJECT

   private slots:
    void init()
    {
        ServerInstance::clearJavaProbeCacheForTesting();
    }

    void installsVerifiedContentUpdateAtomically()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        FixtureHttpServer fixtureHttp;
        QVERIFY(fixtureHttp.start());
        const QByteArray replacement("verified replacement bytes");
        fixtureHttp.addRoute("/replacement.jar", replacement);

        auto server = std::make_shared<ServerInstance>("content-update", "Content update");
        server->setServerDirectory(temporaryRoot.filePath("server"));
        server->setLoaderType("fabric");
        const QString installed = QDir(server->modsDirectory()).filePath("example-1.jar");
        QVERIFY(writeFile(installed, "working original"));

        ServerContentUpdateRequest request;
        request.url = fixtureHttp.url("/replacement.jar");
        request.installedPath = installed;
        request.replacementName = "example-2.jar";
        request.hashAlgorithm = QCryptographicHash::Sha256;
        request.expectedHash = QCryptographicHash::hash(replacement, request.hashAlgorithm);

        ServerContentUpdater updater;
        bool finished = false;
        ServerContentUpdateResult result;
        connect(&updater, &ServerContentUpdater::finished, this,
                [&finished, &result](const ServerContentUpdateResult& updateResult) {
                    finished = true;
                    result = updateResult;
                });
        QString error;
        QVERIFY2(updater.start(server, request, &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
        QVERIFY(result.success);
        QCOMPARE(result.failure, ServerContentUpdateFailure::None);
        QVERIFY(result.workingFilePreserved);
        QVERIFY(!QFileInfo::exists(installed));
        QFile updated(result.destinationPath);
        QVERIFY(updated.open(QIODevice::ReadOnly));
        QCOMPARE(updated.readAll(), replacement);
    }

    void validatesModrinthContentUpdateMetadata()
    {
        QString error;
        QVERIFY(ServerContentUpdater::parseModrinthVersionResponse(
                    "not json", "example-1.jar", &error).fileName.isEmpty());
        QVERIFY(error.contains("invalid"));

        error.clear();
        const auto metadata = [](const QString& fileName, const QJsonObject& hashes) {
            return QJsonDocument(QJsonArray{ QJsonObject{
                { "id", "version-2" },
                { "version_number", "2.0" },
                { "files", QJsonArray{ QJsonObject{
                    { "primary", true },
                    { "filename", fileName },
                    { "url", "https://cdn.example.invalid/example-2.jar" },
                    { "hashes", hashes },
                } } },
            } }).toJson(QJsonDocument::Compact);
        };
        const QByteArray missingHash = metadata("example-2.jar", {});
        QVERIFY(ServerContentUpdater::parseModrinthVersionResponse(
                    missingHash, "example-1.jar", &error).fileName.isEmpty());
        QVERIFY(error.contains("hash"));

        error.clear();
        const QByteArray unsafeName = metadata(
            "../example-2.jar", QJsonObject{{ "sha256", QString(64, 'a') }});
        QVERIFY(ServerContentUpdater::parseModrinthVersionResponse(
                    unsafeName, "example-1.jar", &error).fileName.isEmpty());
        QVERIFY(error.contains("unsafe"));

        error.clear();
        const QByteArray valid = metadata(
            "example-2.jar", QJsonObject{{ "sha256", QString(64, 'b') }});
        const ServerContentUpdateCandidate candidate =
            ServerContentUpdater::parseModrinthVersionResponse(
                valid, "example-1.jar", &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(candidate.available);
        QVERIFY(!candidate.upToDate);
        QCOMPARE(candidate.versionId, QString("version-2"));
        QCOMPARE(candidate.versionNumber, QString("2.0"));
        QCOMPARE(candidate.fileName, QString("example-2.jar"));
        QCOMPARE(candidate.hashAlgorithm, QCryptographicHash::Sha256);
        QCOMPARE(candidate.expectedHash.size(), 32);

        const ServerContentUpdateCandidate current =
            ServerContentUpdater::parseModrinthVersionResponse(
                valid, "example-2.jar", &error);
        QVERIFY(current.upToDate);
        QVERIFY(!current.available);
    }

    void validatesCurseForgeContentUpdateMetadata()
    {
        QString error;
        QVERIFY(ServerContentUpdater::parseCurseForgeFilesResponse(
                    "not json", "example-1.jar", "forge", &error).fileName.isEmpty());
        QVERIFY(error.contains("invalid", Qt::CaseInsensitive));

        const auto file = [](qint64 id, const QString& name, const QString& date,
                             const QStringList& gameVersions, const QString& sha1,
                             const QString& downloadUrl = QString()) {
            QJsonArray versions;
            for (const QString& version : gameVersions) versions.append(version);
            return QJsonObject{
                { "id", id },
                { "fileName", name },
                { "displayName", QString("Release %1").arg(id) },
                { "fileDate", date },
                { "gameVersions", versions },
                { "downloadUrl", downloadUrl },
                { "hashes", QJsonArray{ QJsonObject{{ "algo", 1 }, { "value", sha1 }} } },
            };
        };
        const QByteArray response = QJsonDocument(QJsonObject{
            { "data", QJsonArray{
                file(99, "fabric-only.jar", "2026-08-18T12:00:00Z",
                     {"1.21.1", "Fabric"}, QString(40, 'a')),
                file(22, "example-2.jar", "2026-08-17T12:00:00Z",
                     {"1.21.1", "Forge"}, QString(40, 'b')),
                file(21, "example-1.jar", "2026-08-16T12:00:00Z",
                     {"1.21.1", "Forge"}, QString(40, 'c')),
            } }
        }).toJson(QJsonDocument::Compact);

        error.clear();
        const ServerContentUpdateCandidate candidate =
            ServerContentUpdater::parseCurseForgeFilesResponse(
                response, "example-1.jar", "forge", &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(candidate.available);
        QCOMPARE(candidate.versionId, QString("22"));
        QCOMPARE(candidate.providerFileId, QString("22"));
        QCOMPARE(candidate.fileName, QString("example-2.jar"));
        QCOMPARE(candidate.hashAlgorithm, QCryptographicHash::Sha1);
        QCOMPARE(candidate.expectedHash.size(), 20);
        QVERIFY(candidate.url.isEmpty());

        const ServerContentUpdateCandidate current =
            ServerContentUpdater::parseCurseForgeFilesResponse(
                response, "example-2.jar", "forge", &error);
        QVERIFY(current.upToDate);
        QVERIFY(!current.available);
    }

    void preservesInstalledContentAcrossUpdateFailuresAndCancellation()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        FixtureHttpServer fixtureHttp;
        QVERIFY(fixtureHttp.start());
        fixtureHttp.addRoute("/wrong.jar", "wrong replacement");
        fixtureHttp.addRoute("/cancel.jar", QByteArray(1024 * 1024, 'x'));

        auto server = std::make_shared<ServerInstance>("failed-update", "Failed update");
        server->setServerDirectory(temporaryRoot.filePath("server"));
        server->setLoaderType("fabric");
        const QString installed = QDir(server->modsDirectory()).filePath("working.jar");
        const QByteArray original("working original bytes");
        QVERIFY(writeFile(installed, original));

        const auto readInstalled = [&installed]() {
            QFile file(installed);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        ServerContentUpdateRequest request;
        request.installedPath = installed;
        request.replacementName = "replacement.jar";
        request.hashAlgorithm = QCryptographicHash::Sha256;
        request.expectedHash = QCryptographicHash::hash("expected bytes", request.hashAlgorithm);

        const auto runFailure = [server, &request](ServerContentUpdater& updater) {
            ServerContentUpdateResult result;
            QSignalSpy finished(&updater, &ServerContentUpdater::finished);
            QString error;
            const bool started = updater.start(server, request, &error);
            if (!started) {
                result.message = error;
            } else if ((finished.isEmpty() && finished.wait(5000)) || !finished.isEmpty()) {
                result = qvariant_cast<ServerContentUpdateResult>(finished.first().first());
            }
            return qMakePair(started, result);
        };

        request.url = fixtureHttp.url("/wrong.jar");
        ServerContentUpdater mismatchUpdater;
        auto outcome = runFailure(mismatchUpdater);
        QVERIFY(outcome.first);
        QCOMPARE(outcome.second.failure, ServerContentUpdateFailure::HashMismatch);
        QVERIFY(outcome.second.workingFilePreserved);
        QVERIFY(outcome.second.retryAvailable);
        QCOMPARE(readInstalled(), original);
        QVERIFY(!QFileInfo::exists(QDir(server->modsDirectory()).filePath("replacement.jar")));

        const quint16 closedPort = unusedPort();
        request.url = QUrl(QString("http://127.0.0.1:%1/unavailable.jar").arg(closedPort));
        ServerContentUpdater networkUpdater;
        outcome = runFailure(networkUpdater);
        QVERIFY(outcome.first);
        QCOMPARE(outcome.second.failure, ServerContentUpdateFailure::Network);
        QCOMPARE(readInstalled(), original);

        request.url = fixtureHttp.url("/cancel.jar");
        ServerContentUpdater cancelledUpdater;
        bool cancelledFinished = false;
        ServerContentUpdateResult cancelledResult;
        connect(&cancelledUpdater, &ServerContentUpdater::finished, this,
                [&cancelledFinished, &cancelledResult](const ServerContentUpdateResult& updateResult) {
                    cancelledFinished = true;
                    cancelledResult = updateResult;
                });
        QString error;
        QVERIFY2(cancelledUpdater.start(server, request, &error), qPrintable(error));
        QVERIFY(cancelledUpdater.cancel());
        QTRY_VERIFY_WITH_TIMEOUT(cancelledFinished, 5000);
        QCOMPARE(cancelledResult.failure, ServerContentUpdateFailure::Cancelled);
        QVERIFY(cancelledResult.workingFilePreserved);
        QVERIFY(cancelledResult.retryAvailable);
        QCOMPARE(readInstalled(), original);
    }

    void rejectsUnsafeContentUpdateRequestsAndActiveServers()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        auto server = std::make_shared<ServerInstance>("unsafe-update", "Unsafe update");
        server->setServerDirectory(temporaryRoot.filePath("server"));
        server->setLoaderType("fabric");
        const QString installed = QDir(server->modsDirectory()).filePath("working.jar");
        QVERIFY(writeFile(installed, "working"));

        ServerContentUpdateRequest request;
        request.url = QUrl("https://example.invalid/replacement.jar");
        request.installedPath = installed;
        request.replacementName = "../outside.jar";
        request.hashAlgorithm = QCryptographicHash::Sha256;
        request.expectedHash = QByteArray(32, 'x');
        ServerContentUpdater updater;
        QString error;
        QVERIFY(!updater.start(server, request, &error));
        QVERIFY(error.contains("unsafe"));

        request.replacementName = "replacement.jar";
        request.expectedHash.clear();
        error.clear();
        QVERIFY(!updater.start(server, request, &error));
        QVERIFY(error.contains("hash"));

        QVERIFY(prepareSyntheticServer(*server, server->serverDirectory()));
        server->setLoaderType("fabric");
        QVERIFY(server->start());
        QTRY_COMPARE_WITH_TIMEOUT(server->status(), ServerStatus::Running, 5000);
        request.expectedHash = QByteArray(32, 'x');
        error.clear();
        QVERIFY(!updater.start(server, request, &error));
        QVERIFY(error.contains("Stop the server"));
        QVERIFY(server->stop());
        QTRY_COMPARE_WITH_TIMEOUT(server->status(), ServerStatus::Stopped, 5000);
    }

    void addsAndReplacesModFiles()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        ServerInstance server("mods-test", "Mods test");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        server.setLoaderType("fabric");
        const QString source = temporaryRoot.filePath("source/example.jar");
        QVERIFY(writeFile(source, "first version"));

        QString error;
        QVERIFY(server.addMods({ source }, &error));
        const QString installed = QDir(server.modsDirectory()).filePath("example.jar");
        QCOMPARE(QFileInfo(installed).size(), qint64(13));

        const QString connectorCache =
            QDir(server.modsDirectory()).filePath(".connector/cached.jar");
        const QString fabricCache = QDir(server.serverDirectory()).filePath(
            ".fabric/processedMods/cached.jar");
        QVERIFY(writeFile(connectorCache, "generated"));
        QVERIFY(writeFile(fabricCache, "generated"));
        QVERIFY(writeFile(source, "replacement version"));
        QVERIFY(server.addMods({ source }, &error));
        QVERIFY(!QFileInfo::exists(QFileInfo(connectorCache).dir().absolutePath()));
        QVERIFY(!QFileInfo::exists(QFileInfo(fabricCache).dir().absolutePath()));
        QFile installedFile(installed);
        QVERIFY(installedFile.open(QIODevice::ReadOnly));
        QCOMPARE(installedFile.readAll(), QByteArray("replacement version"));

        const QString invalid = temporaryRoot.filePath("source/not-a-mod.txt");
        QVERIFY(writeFile(invalid, "not a jar"));
        QVERIFY(!server.addMods({ invalid }, &error));
        QVERIFY(error.contains(".jar"));
    }

    void routesPluginsAndRejectsContentForVanilla()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString source = temporaryRoot.filePath("source/example.jar");
        QVERIFY(writeFile(source, "plugin"));

        ServerInstance paper("plugins-test", "Plugins test");
        paper.setServerDirectory(temporaryRoot.filePath("paper"));
        paper.setLoaderType("paper");
        QString error;
        QVERIFY(paper.addContentFiles({ source }, &error));
        QVERIFY(QFileInfo::exists(QDir(paper.pluginsDirectory()).filePath("example.jar")));
        QVERIFY(!QFileInfo::exists(QDir(paper.modsDirectory()).filePath("example.jar")));

        ServerInstance vanilla("vanilla-content", "Vanilla content");
        vanilla.setServerDirectory(temporaryRoot.filePath("vanilla"));
        vanilla.setLoaderType("vanilla");
        error.clear();
        QVERIFY(!vanilla.addContentFiles({ source }, &error));
        QVERIFY(error.contains("does not support"));
        QVERIFY(!QFileInfo::exists(QDir(vanilla.modsDirectory()).filePath("example.jar")));
        QVERIFY(!QFileInfo::exists(QDir(vanilla.pluginsDirectory()).filePath("example.jar")));
    }

    void blocksLocalContentMutationWhileRunning()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("active-content", "Active content");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server")));
        server.setLoaderType("fabric");
        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);

        const QString source = temporaryRoot.filePath("source/example.jar");
        QVERIFY(writeFile(source, "mod"));
        QString error;
        QVERIFY(!server.addContentFiles({ source }, &error));
        QVERIFY(error.contains("Stop the server"));
        QVERIFY(!QFileInfo::exists(QDir(server.modsDirectory()).filePath("example.jar")));

        const QString installed = QDir(server.modsDirectory()).filePath("installed.jar");
        QVERIFY(writeFile(installed, "mod"));
        error.clear();
        QVERIFY(!server.removeContentFile(installed, &error));
        QVERIFY(error.contains("Stop the server"));
        QVERIFY(QFileInfo::exists(installed));

        const QString pack = temporaryRoot.filePath("active-pack.zip");
        QVERIFY(writeArchive(pack, { { "mods/from-pack.jar", "blocked" } }));
        error.clear();
        QVERIFY(!server.importServerPack(pack, &error));
        QVERIFY(error.contains("Stop the server"));
        QVERIFY(!QFileInfo::exists(QDir(server.modsDirectory()).filePath("from-pack.jar")));

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void removesContentToTheRecycleBin()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("remove-content", "Remove content");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        server.setLoaderType("fabric");
        const QString installed = QDir(server.modsDirectory()).filePath("remove-me.jar");
        QVERIFY(writeFile(installed, "mod to remove"));

        // Only the server's own content files can be removed.
        const QString elsewhere = temporaryRoot.filePath("source/remove-me.jar");
        QVERIFY(writeFile(elsewhere, "not the server's"));
        QString error;
        QVERIFY(!server.removeContentFile(elsewhere, &error));
        QVERIFY(error.contains("not one of this server's"));
        QVERIFY(QFileInfo::exists(elsewhere));

        QString pathInTrash;
        error.clear();
        if (!server.removeContentFile(installed, &error, &pathInTrash)) {
            QVERIFY(error.contains("Recycle Bin"));
            QVERIFY(QFileInfo::exists(installed));
            QSKIP("This system has no Recycle Bin, so the file was kept as intended.");
        }
        QVERIFY(!QFileInfo::exists(installed));
#ifdef Q_OS_WIN
        // Windows reports where the file went; a plain delete would leave this empty.
        QVERIFY(!pathInTrash.isEmpty());
        QVERIFY(QFileInfo::exists(pathInTrash));
#endif
        // Take the file back out of the Recycle Bin so the test leaves nothing behind there.
        if (!pathInTrash.isEmpty()) {
            const QString restored = temporaryRoot.filePath("restored.jar");
            QVERIFY(QFile::rename(pathInTrash, restored));
            QCOMPARE(readFile(restored), QByteArray("mod to remove"));
        }
    }

    void importsOnlyAllowedServerPackPaths()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        const QString archivePath = temporaryRoot.filePath("server-pack.zip");
        QVERIFY(writeArchive(archivePath, {
            { "overrides/mods/example.jar", "mod" },
            { ".minecraft/config/example.toml", "config" },
            { "datapacks/worldgen_removals/pack.mcmeta", "datapack" },
            { "configureddefaults/config/common.snbt", "defaults" },
            { "ftbteambases/structures/base.nbt", "base" },
            { "default-server.properties", "allow-flight=true\n" },
            { "scripts/startup.js", "script" },
            { "README.md", "ignored" },
            { "../outside.txt", "blocked" },
        }));

        ServerInstance server("pack-test", "Pack test");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        QVERIFY(writeFile(QDir(server.modsDirectory()).filePath("example.jar"), "old mod"));
        QVERIFY(writeFile(QDir(server.serverDirectory()).filePath("server.properties"),
                          "motd=old\n"));
        QVERIFY(writeFile(QDir(server.serverDirectory()).filePath("unrelated.txt"),
                          "untouched\n"));
        QVERIFY(writeFile(QDir(server.serverDirectory()).filePath("README.md"),
                          "keep README\n"));
        QVERIFY(writeFile(temporaryRoot.filePath("outside.txt"), "keep outside\n"));
        QString error;
        QVERIFY(server.importServerPack(archivePath, &error));
        QCOMPARE(readFile(QDir(server.modsDirectory()).filePath("example.jar")),
                 QByteArray("mod"));
        QVERIFY(QFileInfo::exists(QDir(server.modsDirectory()).filePath("example.jar")));
        QVERIFY(QFileInfo::exists(QDir(server.serverDirectory()).filePath("config/example.toml")));
        QVERIFY(QFileInfo::exists(QDir(server.serverDirectory()).filePath("scripts/startup.js")));
        QVERIFY(QFileInfo::exists(QDir(server.serverDirectory()).filePath(
            "datapacks/worldgen_removals/pack.mcmeta")));
        QVERIFY(QFileInfo::exists(QDir(server.serverDirectory()).filePath(
            "configureddefaults/config/common.snbt")));
        QVERIFY(QFileInfo::exists(QDir(server.serverDirectory()).filePath(
            "ftbteambases/structures/base.nbt")));
        QVERIFY(QFileInfo::exists(QDir(server.serverDirectory()).filePath(
            "server.properties")));
        QCOMPARE(readFile(QDir(server.serverDirectory()).filePath("server.properties")),
                 QByteArray("allow-flight=true\n"));
        QCOMPARE(readFile(QDir(server.serverDirectory()).filePath("unrelated.txt")),
                 QByteArray("untouched\n"));
        QCOMPARE(readFile(QDir(server.serverDirectory()).filePath("README.md")),
                 QByteArray("keep README\n"));
        QVERIFY(!QFileInfo::exists(QDir(server.serverDirectory()).filePath(
            "default-server.properties")));
        QCOMPARE(readFile(temporaryRoot.filePath("outside.txt")),
                 QByteArray("keep outside\n"));
    }

    void importsServerPackWrappedInOneFolder()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        const QString archivePath = temporaryRoot.filePath("wrapped-pack.zip");
        QVERIFY(writeArchive(archivePath, {
            { "MyPack-Server-1.0/mods/example.jar", "mod" },
            { "MyPack-Server-1.0/mods/Mod..Extras.jar", "dots" },
            { "MyPack-Server-1.0/config/example.toml", "config" },
            { "MyPack-Server-1.0/README.md", "ignored" },
            { "__MACOSX/MyPack-Server-1.0/mods/._example.jar", "ignored" },
        }));

        ServerInstance server("wrapped-pack-test", "Wrapped pack test");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        QVERIFY(QDir().mkpath(server.serverDirectory()));
        QString error;
        QVERIFY2(server.importServerPack(archivePath, &error), qPrintable(error));
        QCOMPARE(readFile(QDir(server.modsDirectory()).filePath("example.jar")),
                 QByteArray("mod"));
        QCOMPARE(readFile(QDir(server.modsDirectory()).filePath("Mod..Extras.jar")),
                 QByteArray("dots"));
        QVERIFY(QFileInfo::exists(QDir(server.serverDirectory()).filePath("config/example.toml")));
        QVERIFY(!QFileInfo::exists(QDir(server.serverDirectory()).filePath("README.md")));
        QVERIFY(!QFileInfo::exists(QDir(server.serverDirectory()).filePath("MyPack-Server-1.0")));
    }

    void serverWillNotStartDuringServerPackImport()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString archivePath = temporaryRoot.filePath("pack.zip");
        QVERIFY(writeArchive(archivePath, { { "mods/example.jar", "mod" } }));

        ServerInstance server("import-guard-test", "Import guard test");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        server.setLoaderType("fabric");
        QVERIFY(QDir().mkpath(server.serverDirectory()));

        QString error;
        QVERIFY(server.beginServerPackImport(archivePath, &error));
        QVERIFY(!server.beginServerPackImport(archivePath, &error));
        QVERIFY(!server.start());
        QVERIFY(!server.prepareServerSoftware());
        QCOMPARE(server.status(), ServerStatus::Stopped);

        // Mods can't be added or removed while the import is writing the same folders.
        const QString source = temporaryRoot.filePath("source/added.jar");
        QVERIFY(writeFile(source, "mod"));
        error.clear();
        QVERIFY(!server.addContentFiles({ source }, &error));
        QVERIFY2(error.contains("server pack import"), qPrintable(error));
        const QString existing = QDir(server.modsDirectory()).filePath("existing.jar");
        QVERIFY(writeFile(existing, "mod"));
        error.clear();
        QVERIFY(!server.removeContentFile(existing, &error));
        QVERIFY2(error.contains("server pack import"), qPrintable(error));
        QVERIFY(QFileInfo::exists(existing));

        QVERIFY(ServerInstance::importServerPackFiles(server.serverDirectory(), archivePath, &error));
        server.finishServerPackImport(true);
        QVERIFY(server.beginServerPackImport(archivePath, &error));
        server.finishServerPackImport(false);
    }

    void rejectsInvalidServerPackRoots()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        const QString archivePath = temporaryRoot.filePath("valid-pack.zip");
        QVERIFY(writeArchive(archivePath, { { "mods/valid.jar", "valid" } }));

        ServerInstance emptyRoot("empty-root-test", "Empty root test");
        emptyRoot.setServerDirectory(QString());
        QString error;
        QVERIFY(!emptyRoot.importServerPack(archivePath, &error));
        QVERIFY(error.contains("server directory", Qt::CaseInsensitive));

        ServerInstance missingRoot("missing-root-test", "Missing root test");
        const QString missingPath = temporaryRoot.filePath("missing-server");
        missingRoot.setServerDirectory(missingPath);
        error.clear();
        QVERIFY(!missingRoot.importServerPack(archivePath, &error));
        QVERIFY(error.contains("does not exist", Qt::CaseInsensitive));
        QVERIFY(!QFileInfo::exists(QDir(missingPath).filePath("mods/valid.jar")));

        const QString fileRootPath = temporaryRoot.filePath("server-file");
        QVERIFY(writeFile(fileRootPath, "not a directory"));
        ServerInstance fileRoot("file-root-test", "File root test");
        fileRoot.setServerDirectory(fileRootPath);
        error.clear();
        QVERIFY(!fileRoot.importServerPack(archivePath, &error));
        QVERIFY(error.contains("not a directory", Qt::CaseInsensitive));
        QCOMPARE(readFile(fileRootPath), QByteArray("not a directory"));
    }

    void rejectsInvalidServerPackTargetShape()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        const QString archivePath = temporaryRoot.filePath("invalid-target.zip");
        QVERIFY(writeArchive(archivePath, { { "config/new.toml", "new" } }));

        ServerInstance server("invalid-target-test", "Invalid target test");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        QVERIFY(writeFile(QDir(server.serverDirectory()).filePath("config"),
                          "blocking file"));
        QVERIFY(writeFile(QDir(server.serverDirectory()).filePath("unrelated.txt"),
                          "untouched"));

        QString error;
        QVERIFY(!server.importServerPack(archivePath, &error));
        QVERIFY(error.contains("not a directory", Qt::CaseInsensitive));
        QCOMPARE(readFile(QDir(server.serverDirectory()).filePath("config")),
                 QByteArray("blocking file"));
        QCOMPARE(readFile(QDir(server.serverDirectory()).filePath("unrelated.txt")),
                 QByteArray("untouched"));
        QVERIFY(!QFileInfo::exists(QDir(server.serverDirectory()).filePath(
            "config/new.toml")));
    }

    void rejectsWindowsUnsafeServerPackPaths()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        const QString archivePath = temporaryRoot.filePath("unsafe-paths.zip");
        QVERIFY(writeArchive(archivePath, {
            { "mods/valid.jar", "valid" },
            { "mods/unsafe:stream.jar", "ads" },
            { "config/CON.txt", "device" },
            { "config/aux", "device" },
        }));

        ServerInstance server("unsafe-path-test", "Unsafe path test");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        QVERIFY(writeFile(QDir(server.serverDirectory()).filePath("unrelated.txt"),
                          "untouched"));

        QString error;
        QVERIFY(!server.importServerPack(archivePath, &error));
        QVERIFY(error.contains("unsafe Windows path", Qt::CaseInsensitive));
        QVERIFY(!QFileInfo::exists(QDir(server.modsDirectory()).filePath("valid.jar")));
        QVERIFY(!QFileInfo::exists(QDir(server.modsDirectory()).filePath(
            "unsafe:stream.jar")));
        QVERIFY(!QFileInfo::exists(QDir(server.serverDirectory()).filePath("config/CON.txt")));
        QVERIFY(!QFileInfo::exists(QDir(server.serverDirectory()).filePath("config/aux")));
        QCOMPARE(readFile(QDir(server.serverDirectory()).filePath("unrelated.txt")),
                 QByteArray("untouched"));

        const QString validArchivePath = temporaryRoot.filePath("valid-only.zip");
        QVERIFY(writeArchive(validArchivePath, { { "mods/valid.jar", "valid" } }));
        error.clear();
        QVERIFY(server.importServerPack(validArchivePath, &error));
        QCOMPARE(readFile(QDir(server.modsDirectory()).filePath("valid.jar")),
                 QByteArray("valid"));
    }

    void rollsBackServerPackPublicationFailure()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        const QString archivePath = temporaryRoot.filePath("rollback-pack.zip");
        QVERIFY(writeArchive(archivePath, {
            { "mods/existing.jar", "replacement" },
            { "config/new.toml", "new file" },
            { "scripts/never-reached.js", "not published" },
        }));

        ServerInstance server("rollback-pack-test", "Rollback pack test");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        QVERIFY(writeFile(QDir(server.modsDirectory()).filePath("existing.jar"),
                          "original"));
        QVERIFY(writeFile(QDir(server.serverDirectory()).filePath("unrelated.txt"),
                          "untouched"));

        ScopedEnvironmentVariable failure(
            "JLAUNCHER_TEST_SERVER_PACK_IMPORT_FAIL_AFTER", "2");
        QString error;
        QVERIFY(!server.importServerPack(archivePath, &error));
        QVERIFY(error.contains("rolled back successfully"));
        QCOMPARE(readFile(QDir(server.modsDirectory()).filePath("existing.jar")),
                 QByteArray("original"));
        QVERIFY(!QFileInfo::exists(QDir(server.serverDirectory()).filePath(
            "config/new.toml")));
        QVERIFY(!QFileInfo::exists(QDir(server.serverDirectory()).filePath("config")));
        QVERIFY(!QFileInfo::exists(QDir(server.serverDirectory()).filePath(
            "scripts/never-reached.js")));
        QCOMPARE(readFile(QDir(server.serverDirectory()).filePath("unrelated.txt")),
                 QByteArray("untouched"));
    }

    void leavesServerUntouchedWhenServerPackStagingFails()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        const QString archivePath = temporaryRoot.filePath("staging-failure.zip");
        QVERIFY(writeArchive(archivePath, {
            { "mods/staged-before-failure.jar", "staged" },
            { "default-server.properties", "defaults" },
            { "server.properties", "duplicate" },
        }));

        ServerInstance server("staging-failure-test", "Staging failure test");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        QVERIFY(writeFile(QDir(server.serverDirectory()).filePath("existing.txt"),
                          "untouched"));

        QString error;
        QVERIFY(!server.importServerPack(archivePath, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(readFile(QDir(server.serverDirectory()).filePath("existing.txt")),
                 QByteArray("untouched"));
        QVERIFY(!QFileInfo::exists(QDir(server.modsDirectory()).filePath(
            "staged-before-failure.jar")));
        QVERIFY(!QFileInfo::exists(QDir(server.serverDirectory()).filePath(
            "server.properties")));
    }

    void rejectsServerPacksWithoutServerContent()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        const QString archivePath = temporaryRoot.filePath("empty-server-pack.zip");
        QVERIFY(writeArchive(archivePath, { { "README.md", "not server content" } }));

        ServerInstance server("empty-pack-test", "Empty pack test");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        QVERIFY(QDir().mkpath(server.serverDirectory()));
        QString error;
        QVERIFY(!server.importServerPack(archivePath, &error));
        QVERIFY(error.contains("no server files"));
    }
};

QTEST_GUILESS_MAIN(ServerContentTest)

#include "ServerContent_test.moc"
