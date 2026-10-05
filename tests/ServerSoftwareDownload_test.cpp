// SPDX-License-Identifier: GPL-3.0-only

// Downloading server software: verified jars, ranged downloads, upgrades and every provider chain.

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
#include <tuple>
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

class ServerSoftwareDownloadTest : public QObject {
    Q_OBJECT

   private slots:
    void init()
    {
        ServerInstance::clearJavaProbeCacheForTesting();
    }

    void rejectsUnverifiedVanillaDownloadWithoutCommittingJar()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString payloadPath = temporaryRoot.filePath("provider/server.jar");
        const QString metadataPath = temporaryRoot.filePath("provider/version.json");
        const QString manifestPath = temporaryRoot.filePath("provider/manifest.json");
        QVERIFY(writeFile(payloadPath, "corrupted server payload"));

        const QJsonObject serverDownload{
            { "url", QUrl::fromLocalFile(payloadPath).toString() },
            { "sha1", QString(40, '0') },
        };
        const QJsonObject metadata{
            { "downloads", QJsonObject{ { "server", serverDownload } } },
        };
        QVERIFY(writeFile(metadataPath, QJsonDocument(metadata).toJson(QJsonDocument::Compact)));

        const QJsonObject version{
            { "id", "1.21.8" },
            { "type", "release" },
            { "url", QUrl::fromLocalFile(metadataPath).toString() },
        };
        const QJsonObject manifest{
            { "versions", QJsonArray{ version } },
        };
        QVERIFY(writeFile(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Compact)));

        ServerDownloader downloader(QUrl::fromLocalFile(manifestPath));
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        const QString destination = temporaryRoot.filePath("installed-server");
        const QString installedJar = QDir(destination).filePath("server.jar");
        const QByteArray workingJar("existing verified working server");
        QVERIFY(writeFile(installedJar, workingJar));
        downloader.startDownload("1.21.8", "vanilla", destination);

        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QCOMPARE(finished.last().at(0).toBool(), false);
        QVERIFY(finished.last().at(1).toString().contains("hash"));
        QFile preserved(installedJar);
        QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), workingJar);
    }

    void startingAnInstallCancelsAVersionListStillLoading()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString payloadPath = temporaryRoot.filePath("provider/server.jar");
        const QString metadataPath = temporaryRoot.filePath("provider/version.json");
        const QString manifestPath = temporaryRoot.filePath("provider/manifest.json");
        QVERIFY(writeFile(payloadPath, "server payload"));
        const QJsonObject metadata{
            { "downloads", QJsonObject{ { "server", QJsonObject{ { "url", QUrl::fromLocalFile(payloadPath).toString() },
                                                                 { "sha1", QString(40, '0') } } } } },
        };
        QVERIFY(writeFile(metadataPath, QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
        const QJsonObject version{
            { "id", "1.21.8" },
            { "type", "release" },
            { "url", QUrl::fromLocalFile(metadataPath).toString() },
        };
        QVERIFY(writeFile(manifestPath, QJsonDocument(QJsonObject{ { "versions", QJsonArray{ version } } })
                                            .toJson(QJsonDocument::Compact)));

        ServerDownloader downloader(QUrl::fromLocalFile(manifestPath));
        QSignalSpy versionsReady(&downloader, &ServerDownloader::versionsReady);
        QSignalSpy versionsFailed(&downloader, &ServerDownloader::versionsFailed);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        downloader.fetchAvailableVersions("vanilla");
        downloader.startDownload("1.21.8", "vanilla", temporaryRoot.filePath("installed-server"));

        QCOMPARE(versionsFailed.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QTest::qWait(100);
        QCOMPARE(finished.size(), 1);
        // The install went through its own steps: it reached the jar and rejected its hash.
        QVERIFY2(finished.last().at(1).toString().contains("hash"), qPrintable(finished.last().at(1).toString()));
        QCOMPARE(versionsReady.size(), 0);
    }

    void serverJarNetworkDownloadsVerifyBothHashAlgorithms()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        RangeHttpServer server;
        QVERIFY(server.start());

        const QByteArray vanillaJar("verified vanilla jar from the local server");
        const QString vanillaSha1 = QString::fromLatin1(
            QCryptographicHash::hash(vanillaJar, QCryptographicHash::Sha1).toHex()).toUpper();
        addVanillaHttpFixture(server, vanillaJar, vanillaSha1);

        const QByteArray paperJar("verified Paper jar from the local server");
        const QString paperSha256 = QString::fromLatin1(
            QCryptographicHash::hash(paperJar, QCryptographicHash::Sha256).toHex());
        server.serve("/paper/projects/paper/versions/1.21.8/builds",
                     httpResource(QJsonDocument(QJsonArray{ QJsonObject{
                         { "id", 130 }, { "channel", "STABLE" },
                         { "downloads", QJsonObject{{ "server:default", QJsonObject{
                             { "url", server.url("/paper-server.jar").toString() },
                             { "checksums", QJsonObject{{ "sha256", paperSha256 }} },
                         } }} },
                     } }).toJson(QJsonDocument::Compact)));
        server.serve("/paper-server.jar", httpResource(paperJar));

        ServerProviderEndpoints endpoints = vanillaHttpEndpoints(server);
        endpoints.paperApiBase = server.url("/paper/");

        {
            ServerDownloader downloader(endpoints);
            QSignalSpy finished(&downloader, &ServerDownloader::finished);
            const QString destination = temporaryRoot.filePath("vanilla");
            downloader.startDownload("1.21.8", "vanilla", destination);

            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
            QVERIFY2(finished.constFirst().at(0).toBool(),
                     qPrintable(finished.constFirst().at(1).toString()));
            QCOMPARE(readFile(QDir(destination).filePath("server.jar")), vanillaJar);
        }

        {
            ServerDownloader downloader(endpoints);
            QSignalSpy finished(&downloader, &ServerDownloader::finished);
            const QString destination = temporaryRoot.filePath("paper");
            downloader.startDownload("1.21.8", "paper", destination);

            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
            QVERIFY2(finished.constFirst().at(0).toBool(),
                     qPrintable(finished.constFirst().at(1).toString()));
            QCOMPARE(readFile(QDir(destination).filePath("server.jar")), paperJar);
        }
    }

    void serverJarHashMismatchLeavesNoFile()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        RangeHttpServer server;
        QVERIFY(server.start());
        const QByteArray jar("corrupt server jar");
        addVanillaHttpFixture(server, jar, QString(40, '0'));

        ServerDownloader downloader(vanillaHttpEndpoints(server));
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        const QString destination = temporaryRoot.filePath("bad-hash");
        const QString serverJar = QDir(destination).filePath("server.jar");
        downloader.startDownload("1.21.8", "vanilla", destination);

        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY(!finished.constFirst().at(0).toBool());
        QVERIFY(finished.constFirst().at(1).toString().contains(
            "Download verification failed: the server file hash did not match"));
        QVERIFY(!QFileInfo::exists(serverJar));
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(serverJar)));
        QCOMPARE(QDir(destination).entryList(QDir::Files | QDir::Hidden | QDir::System).size(), 0);
    }

    void largeServerJarUsesRangedSegments()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        RangeHttpServer server;
        QVERIFY(server.start());
        const QByteArray jar(40 * 1024 * 1024, 's');
        const QString sha1 = QString::fromLatin1(
            QCryptographicHash::hash(jar, QCryptographicHash::Sha1).toHex());
        addVanillaHttpFixture(server, jar, sha1);

        ServerDownloader downloader(vanillaHttpEndpoints(server));
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        const QString destination = temporaryRoot.filePath("large-server");
        const QString serverJar = QDir(destination).filePath("server.jar");
        downloader.startDownload("1.21.8", "vanilla", destination);

        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 30000);
        QVERIFY2(finished.constFirst().at(0).toBool(),
                 qPrintable(finished.constFirst().at(1).toString()));
        QVERIFY(server.rangeRequestCount() > 1);
        QVERIFY(server.peakConcurrency() > 1);
        QCOMPARE(readFile(serverJar), jar);
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(serverJar)));
        QCOMPARE(Net::HostScheduler::global()->outstandingPermits(), 0);
    }

    void cancellingServerJarDownloadRemovesPartialFilesAndFinishesOnce()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        RangeHttpServer server;
        QVERIFY(server.start());
        const QByteArray jar(40 * 1024 * 1024, 'c');
        const QString sha1 = QString::fromLatin1(
            QCryptographicHash::hash(jar, QCryptographicHash::Sha1).toHex());
        addVanillaHttpFixture(server, jar, sha1, 2 * 1024 * 1024);

        ServerDownloader downloader(vanillaHttpEndpoints(server));
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        QSignalSpy progress(&downloader, &ServerDownloader::progress);
        const QString destination = temporaryRoot.filePath("cancelled-server");
        const QString serverJar = QDir(destination).filePath("server.jar");
        downloader.startDownload("1.21.8", "vanilla", destination);

        QTRY_VERIFY_WITH_TIMEOUT(server.rangeRequestCount() > 1, 10000);
        const auto hasProgress = [&progress]() {
            for (const auto& item : progress) {
                if (item.constFirst().toInt() > 0)
                    return true;
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(hasProgress(), 10000);
        downloader.cancel();

        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(finished.constFirst().at(0).toBool(), false);
        QCOMPARE(finished.constFirst().at(1).toString(), QStringLiteral("Download cancelled."));
        QTest::qWait(100);
        QCOMPARE(finished.size(), 1);
        QVERIFY(!QFileInfo::exists(serverJar));
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(serverJar)));
        QCOMPARE(QDir(destination).entryList(QDir::Files | QDir::Hidden | QDir::System).size(), 0);
        QCOMPARE(Net::HostScheduler::global()->outstandingPermits(), 0);
    }

    void preparesMissingServerSoftwareWithoutStartingMinecraft()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QByteArray payload("verified prepared server jar");
        const QString payloadPath = temporaryRoot.filePath("provider/server.jar");
        const QString metadataPath = temporaryRoot.filePath("provider/version.json");
        const QString manifestPath = temporaryRoot.filePath("provider/manifest.json");
        QVERIFY(writeFile(payloadPath, payload));
        const QString sha1 = QString::fromLatin1(
            QCryptographicHash::hash(payload, QCryptographicHash::Sha1).toHex());
        QVERIFY(writeFile(metadataPath, QJsonDocument(QJsonObject{
            { "downloads", QJsonObject{{ "server", QJsonObject{
                { "url", QUrl::fromLocalFile(payloadPath).toString() },
                { "sha1", sha1 },
            } }} },
        }).toJson(QJsonDocument::Compact)));
        QVERIFY(writeFile(manifestPath, QJsonDocument(QJsonObject{
            { "versions", QJsonArray{ QJsonObject{
                { "id", "1.21.8" }, { "type", "release" },
                { "url", QUrl::fromLocalFile(metadataPath).toString() },
            } } },
        }).toJson(QJsonDocument::Compact)));

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.vanillaManifest = QUrl::fromLocalFile(manifestPath);
        ServerInstance server("prepare-runtime", "Prepare runtime", endpoints);
        server.setServerDirectory(temporaryRoot.filePath("server"));
        server.setVersion("1.21.8");
        server.setLoaderType("vanilla");
        server.setJavaPath(fakeMinecraftServerPath());
        QSignalSpy finished(&server, &ServerInstance::serverSoftwareDownloadFinished);

        QVERIFY(server.prepareServerSoftware());
        QCOMPARE(server.status(), ServerStatus::Downloading);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(finished.constFirst().at(1).toBool());
        QCOMPARE(server.status(), ServerStatus::Stopped);
        QCOMPARE(server.processId(), qint64(0));
        QCOMPARE(readFile(server.serverJarPath()), payload);

        // An already prepared target is retained without another download.
        QVERIFY(server.prepareServerSoftware());
        QCOMPARE(finished.size(), 1);
        QCOMPARE(readFile(server.serverJarPath()), payload);
    }

    void commitsVersionOnlyAfterVerifiedServerUpgradeDownload()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QByteArray upgradedJar("verified upgraded server jar");
        const QString upgradedJarPath = temporaryRoot.filePath("provider/upgraded.jar");
        const QString upgradedMetadataPath = temporaryRoot.filePath("provider/upgraded.json");
        const QString badMetadataPath = temporaryRoot.filePath("provider/bad.json");
        const QString manifestPath = temporaryRoot.filePath("provider/manifest.json");
        QVERIFY(writeFile(upgradedJarPath, upgradedJar));

        const QString upgradedSha1 = QString::fromLatin1(
            QCryptographicHash::hash(upgradedJar, QCryptographicHash::Sha1).toHex());
        const QJsonObject upgradedMetadata{
            { "downloads", QJsonObject{ { "server", QJsonObject{
                { "url", QUrl::fromLocalFile(upgradedJarPath).toString() },
                { "sha1", upgradedSha1 },
            } } } },
        };
        const QJsonObject badMetadata{
            { "downloads", QJsonObject{ { "server", QJsonObject{
                { "url", QUrl::fromLocalFile(upgradedJarPath).toString() },
                { "sha1", QString(40, '0') },
            } } } },
        };
        QVERIFY(writeFile(upgradedMetadataPath,
                          QJsonDocument(upgradedMetadata).toJson(QJsonDocument::Compact)));
        QVERIFY(writeFile(badMetadataPath,
                          QJsonDocument(badMetadata).toJson(QJsonDocument::Compact)));
        const QJsonObject manifest{
            { "versions", QJsonArray{
                QJsonObject{ { "id", "1.22" }, { "type", "release" },
                             { "url", QUrl::fromLocalFile(upgradedMetadataPath).toString() } },
                QJsonObject{ { "id", "1.23" }, { "type", "release" },
                             { "url", QUrl::fromLocalFile(badMetadataPath).toString() } },
            } },
        };
        QVERIFY(writeFile(manifestPath,
                          QJsonDocument(manifest).toJson(QJsonDocument::Compact)));

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.vanillaManifest = QUrl::fromLocalFile(manifestPath);
        ServerInstance server("version-upgrade", "Version upgrade", endpoints);
        server.setServerDirectory(temporaryRoot.filePath("server"));
        server.setLoaderType("vanilla");
        server.setVersion("1.21.8");
        const QByteArray originalJar("original working server jar");
        QVERIFY(writeFile(server.serverJarPath(), originalJar));
        QSignalSpy finished(&server, &ServerInstance::serverSoftwareDownloadFinished);

        QVERIFY(server.downloadServerJarForVersion("1.22", QString(), false));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(finished.last().at(0).toString(), QString("1.22"));
        QCOMPARE(finished.last().at(1).toBool(), true);
        QCOMPARE(server.version(), QString("1.22"));
        QFile installed(server.serverJarPath());
        QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(), upgradedJar);
        installed.close();

        QVERIFY(server.downloadServerJarForVersion("1.23", QString(), false));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
        QCOMPARE(finished.last().at(0).toString(), QString("1.23"));
        QCOMPARE(finished.last().at(1).toBool(), false);
        QCOMPARE(finished.last().at(2).toBool(), false);
        QCOMPARE(server.version(), QString("1.22"));
        QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(), upgradedJar);
    }

    void commitsSelectedBuildOnlyAfterVerifiedDownload()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        FixtureHttpServer fixtureHttp;
        QVERIFY(fixtureHttp.start());
        const QByteArray selectedJar("verified selected Paper build");
        fixtureHttp.addRoute(
            "/paper/projects/paper/versions/1.21.8/builds",
            QJsonDocument(QJsonArray{ QJsonObject{
                { "id", 130 }, { "channel", "STABLE" },
                { "downloads", QJsonObject{{ "server:default", QJsonObject{
                    { "url", fixtureHttp.url("/paper-130.jar").toString() },
                    { "checksums", QJsonObject{{ "sha256", QString::fromLatin1(
                        QCryptographicHash::hash(selectedJar, QCryptographicHash::Sha256).toHex()) }} },
                } }} },
            } }).toJson(QJsonDocument::Compact));
        fixtureHttp.addRoute("/paper-130.jar", selectedJar);

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.paperApiBase = fixtureHttp.baseUrl("paper");
        ServerInstance server("build-update", "Build update", endpoints);
        server.setServerDirectory(temporaryRoot.filePath("server"));
        server.setLoaderType("paper");
        server.setVersion("1.21.8");
        server.setLoaderVersion("120");
        const QByteArray originalJar("original Paper build");
        QVERIFY(writeFile(server.serverJarPath(), originalJar));
        QSignalSpy finished(&server, &ServerInstance::serverSoftwareDownloadFinished);

        QVERIFY(server.downloadServerBuild("130", QString(), false));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(finished.last().at(1).toBool());
        QCOMPARE(server.loaderVersion(), QString("130"));
        QFile installed(server.serverJarPath());
        QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(), selectedJar);
        installed.close();

        QVERIFY(server.downloadServerBuild("999", QString(), false));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
        QVERIFY(!finished.last().at(1).toBool());
        QCOMPARE(server.loaderVersion(), QString("130"));
        QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(), selectedJar);
    }

    void completesEveryProviderInstallChainFromOfflineFixtures()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QByteArray serverPayload("certified phase-five server payload");
        const QString directJar = root.filePath("fixtures/direct-server.jar");
        QVERIFY(writeFile(directJar, serverPayload));

        const QString vanillaMetadata = root.filePath("fixtures/vanilla/version.json");
        const QJsonObject vanillaDownload{
            { "url", QUrl::fromLocalFile(directJar).toString() },
            { "sha1", QString::fromLatin1(
                QCryptographicHash::hash(serverPayload, QCryptographicHash::Sha1).toHex()) },
        };
        QVERIFY(writeFile(vanillaMetadata, QJsonDocument(QJsonObject{
            { "downloads", QJsonObject{ { "server", vanillaDownload } } },
        }).toJson(QJsonDocument::Compact)));
        const QString vanillaManifest = root.filePath("fixtures/vanilla/manifest.json");
        QVERIFY(writeFile(vanillaManifest, QJsonDocument(QJsonObject{
            { "versions", QJsonArray{ QJsonObject{
                { "id", "1.21.8" },
                { "url", QUrl::fromLocalFile(vanillaMetadata).toString() },
            } } },
        }).toJson(QJsonDocument::Compact)));

        const QString paperBuilds =
            root.filePath("fixtures/paper/projects/paper/versions/1.21.8/builds");
        QVERIFY(writeFile(paperBuilds, QJsonDocument(QJsonArray{ QJsonObject{
            { "id", 123 },
            { "channel", "STABLE" },
            { "downloads", QJsonObject{ { "server:default", QJsonObject{
                { "url", QUrl::fromLocalFile(directJar).toString() },
                { "checksums", QJsonObject{ { "sha256", QString::fromLatin1(
                    QCryptographicHash::hash(serverPayload, QCryptographicHash::Sha256).toHex()) } } },
            } } } },
        } }).toJson(QJsonDocument::Compact)));

        FixtureHttpServer fixtureHttp;
        QVERIFY(fixtureHttp.start());
        const QByteArray fabricServerJar = fabricServerJarFixture(
            root.filePath("fixtures/fabric-server.jar"));
        QVERIFY(!fabricServerJar.isEmpty());
        fixtureHttp.addRoute("/fabric/versions/installer",
                             R"([{"version":"1.0.0","stable":true}])");
        fixtureHttp.addRoute("/fabric/versions/loader/1.21.8",
                             R"([{"loader":{"version":"0.16.10"}}])");
        fixtureHttp.addRoute(
            "/fabric/versions/loader/1.21.8/0.16.10/1.0.0/server/jar",
            fabricServerJar);
        fixtureHttp.addRoute("/purpur/purpur/1.21.8",
                             R"({"builds":{"latest":"2412"}})");
        fixtureHttp.addRoute("/purpur/purpur/1.21.8/2412",
                             R"({"md5":"00000000000000000000000000000000"})");
        fixtureHttp.addRoute("/purpur/purpur/1.21.8/2412",
            QJsonDocument(QJsonObject{{ "md5", QString::fromLatin1(
                QCryptographicHash::hash(serverPayload, QCryptographicHash::Md5).toHex()) }})
                .toJson(QJsonDocument::Compact));
        fixtureHttp.addRoute("/purpur/purpur/1.21.8/2412/download",
                             serverPayload);

        const QString forgeMetadata = root.filePath(
            "fixtures/forge-maven/net/minecraftforge/forge/maven-metadata.xml");
        QVERIFY(writeFile(forgeMetadata,
                          R"(<?xml version="1.0"?><metadata><versioning><versions><version>1.21.1-52.0.1</version></versions></versioning></metadata>)"));
        QVERIFY(writeFile(
            root.filePath(
                "fixtures/forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
                "forge-1.21.1-52.0.1-installer.jar"),
            "synthetic forge installer"));
        QVERIFY(writeInstallerChecksum(root.filePath(
            "fixtures/forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
            "forge-1.21.1-52.0.1-installer.jar")));

        const QString neoForgeVersions = root.filePath("fixtures/neoforge-versions.json");
        QVERIFY(writeFile(neoForgeVersions, R"({"versions":["21.1.50"]})"));
        QVERIFY(writeFile(
            root.filePath(
                "fixtures/neoforge-maven/net/neoforged/neoforge/21.1.50/"
                "neoforge-21.1.50-installer.jar"),
            "synthetic neoforge installer"));
        QVERIFY(writeInstallerChecksum(root.filePath(
            "fixtures/neoforge-maven/net/neoforged/neoforge/21.1.50/"
            "neoforge-21.1.50-installer.jar")));

        ServerProviderEndpoints endpoints{
            QUrl::fromLocalFile(vanillaManifest),
            directoryUrl(root.filePath("fixtures/paper")),
            fixtureHttp.baseUrl("fabric"),
            fixtureHttp.baseUrl("purpur"),
            QUrl::fromLocalFile(forgeMetadata),
            directoryUrl(root.filePath("fixtures/forge-maven")),
            QUrl::fromLocalFile(neoForgeVersions),
            directoryUrl(root.filePath("fixtures/neoforge-maven")),
        };

        const QList<QPair<QString, QString>> providers{
            { "vanilla", "1.21.8" },
            { "paper", "1.21.8" },
            { "fabric", "1.21.8" },
            { "purpur", "1.21.8" },
            { "forge", "1.21.1" },
            { "neoforge", "1.21.1" },
        };
        for (const auto& provider : providers) {
            ServerDownloader downloader(endpoints);
            QSignalSpy finished(&downloader, &ServerDownloader::finished);
            const QString destination = root.filePath("installed/" + provider.first);
            downloader.startDownload(provider.second, provider.first, destination,
                                     fakeMinecraftServerPath());
            QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
            QVERIFY2(finished.last().at(0).toBool(),
                     qPrintable(provider.first + ": " + finished.last().at(1).toString()));
            if (provider.first == "forge" || provider.first == "neoforge") {
                QVERIFY(QFileInfo::exists(QDir(destination).filePath("run.bat")));
                QVERIFY(!QFileInfo::exists(
                    QDir(destination).filePath(provider.first + "-installer.jar")));
            } else {
                QFile installed(QDir(destination).filePath("server.jar"));
                QVERIFY(installed.open(QIODevice::ReadOnly));
                QCOMPARE(installed.readAll(), provider.first == "fabric"
                    ? fabricServerJar : serverPayload);
            }
        }
    }

    void rejectsEveryProviderDownloadFailureWithoutPublishingLaunchTargets()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        FixtureHttpServer fixtureHttp;
        QVERIFY(fixtureHttp.start());

        fixtureHttp.addRoute(
            "/paper/projects/paper/versions/1.21.8/builds",
            QJsonDocument(QJsonArray{ QJsonObject{
                { "id", 123 },
                { "channel", "STABLE" },
                { "downloads", QJsonObject{ { "server:default", QJsonObject{
                    { "url", fixtureHttp.url("/missing/paper.jar").toString() },
                    { "checksums", QJsonObject{ { "sha256", QString(64, '0') } } },
                } } } },
            } }).toJson(QJsonDocument::Compact));
        fixtureHttp.addRoute("/fabric/versions/installer",
                             R"([{"version":"1.0.0","stable":true}])");
        fixtureHttp.addRoute("/fabric/versions/loader/1.21.8",
                             R"([{"loader":{"version":"0.16.10"}}])");
        fixtureHttp.addRoute("/purpur/purpur/1.21.8",
                             R"({"builds":{"latest":"2412"}})");
        fixtureHttp.addRoute("/purpur/purpur/1.21.8/2412",
                             R"({"md5":"00000000000000000000000000000000"})");
        fixtureHttp.addRoute(
            "/forge-maven/net/minecraftforge/forge/maven-metadata.xml",
            R"(<?xml version="1.0"?><metadata><versioning><versions><version>1.21.1-52.0.1</version></versions></versioning></metadata>)");
        fixtureHttp.addRoute("/neoforge/versions",
                             R"({"versions":["21.1.50"]})");

        ServerProviderEndpoints endpoints{
            fixtureHttp.url("/unused/vanilla"),
            fixtureHttp.baseUrl("paper"),
            fixtureHttp.baseUrl("fabric"),
            fixtureHttp.baseUrl("purpur"),
            fixtureHttp.url("/forge/promotions"),
            fixtureHttp.baseUrl("forge-maven"),
            fixtureHttp.url("/neoforge/versions"),
            fixtureHttp.baseUrl("neoforge-maven"),
        };
        const QList<QPair<QString, QString>> providers{
            { "paper", "1.21.8" },
            { "fabric", "1.21.8" },
            { "purpur", "1.21.8" },
            { "forge", "1.21.1" },
            { "neoforge", "1.21.1" },
        };

        for (const auto& provider : providers) {
            ServerDownloader downloader(endpoints);
            QSignalSpy finished(&downloader, &ServerDownloader::finished);
            const QString destination =
                QDir(temporaryRoot.path()).filePath("failed/" + provider.first);
            downloader.startDownload(provider.second, provider.first, destination,
                                     fakeMinecraftServerPath());
            QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
            QVERIFY(!finished.last().at(0).toBool());
            QVERIFY2(finished.last().at(1).toString().contains(
                         provider.first, Qt::CaseInsensitive),
                     qPrintable(finished.last().at(1).toString()));
            QVERIFY(!QFileInfo::exists(QDir(destination).filePath("server.jar")));
            QVERIFY(!QFileInfo::exists(QDir(destination).filePath("run.bat")));
            QVERIFY(!QFileInfo::exists(
                QDir(destination).filePath(provider.first + "-installer.jar")));
        }
    }

    void failedInstallStepsSayWhatWasBeingFetched()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        FixtureHttpServer fixtureHttp;
        QVERIFY(fixtureHttp.start());
        // Each install gets partway, then asks for an address the fixture server does not know.
        fixtureHttp.addRoute("/fabric/versions/installer", R"([{"version":"1.0.0","stable":true}])");
        fixtureHttp.addRoute("/purpur/purpur/1.21.8", R"({"builds":{"latest":"2412"}})");

        ServerProviderEndpoints endpoints{
            fixtureHttp.url("/missing/vanilla-manifest"),
            fixtureHttp.baseUrl("paper"),
            fixtureHttp.baseUrl("fabric"),
            fixtureHttp.baseUrl("purpur"),
            fixtureHttp.url("/forge/promotions"),
            fixtureHttp.baseUrl("forge-maven"),
            fixtureHttp.url("/neoforge/versions"),
            fixtureHttp.baseUrl("neoforge-maven"),
        };
        const QList<std::tuple<QString, QString, QString>> cases{
            { "vanilla", "1.21.8", "Vanilla fetching the version manifest failed" },
            { "fabric", "1.21.8", "Fabric fetching Fabric loader versions failed" },
            { "forge", "1.21.1", "Forge resolving the loader installer failed" },
            { "purpur", "1.21.8", "Failed to fetch the Purpur checksum for build 2412" },
        };

        for (const auto& [type, version, expected] : cases) {
            ServerDownloader downloader(endpoints);
            QSignalSpy finished(&downloader, &ServerDownloader::finished);
            downloader.startDownload(version, type, temporaryRoot.filePath("failed-step/" + type));
            QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
            QVERIFY(!finished.constFirst().at(0).toBool());
            const QString message = finished.constFirst().at(1).toString();
            QVERIFY2(message.contains(expected), qPrintable(type + ": " + message));
        }
    }

    void rejectsPurpurJarWithMismatchedMd5()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        FixtureHttpServer fixtureHttp;
        QVERIFY(fixtureHttp.start());
        fixtureHttp.addRoute("/purpur/purpur/1.21.8",
                             R"({"builds":{"latest":"2412"}})");
        fixtureHttp.addRoute("/purpur/purpur/1.21.8/2412", R"({"md5":"00000000000000000000000000000000"})");
        fixtureHttp.addRoute("/purpur/purpur/1.21.8/2412/download", "wrong bytes");

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.purpurApiBase = fixtureHttp.baseUrl("purpur");
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        const QString destination = temporaryRoot.filePath("purpur-server");
        downloader.startDownload("1.21.8", "purpur", destination);

        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(!finished.constFirst().at(0).toBool());
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath("server.jar")));
    }

    void rejectsForgeInstallerWithMismatchedSha1()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        FixtureHttpServer fixtureHttp;
        QVERIFY(fixtureHttp.start());
        fixtureHttp.addRoute(
            "/forge-maven/net/minecraftforge/forge/maven-metadata.xml",
            R"(<metadata><versioning><versions><version>1.21.1-52.0.1</version></versions></versioning></metadata>)");
        fixtureHttp.addRoute(
            "/forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/forge-1.21.1-52.0.1-installer.jar",
            "synthetic forge installer");
        fixtureHttp.addRoute(
            "/forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/forge-1.21.1-52.0.1-installer.jar.sha1",
            QByteArray(40, '0'));

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.forgeMavenBase = fixtureHttp.baseUrl("forge-maven");
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        const QString destination = temporaryRoot.filePath("forge-server");
        downloader.startDownload("1.21.1", "forge", destination,
                                 fakeMinecraftServerPath(), "52.0.1");

        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY(!finished.constFirst().at(0).toBool());
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath("server.jar")));
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath("forge-installer.jar")));
    }

    void installsForgeInstallerWhenSha1ReturnsHttp404()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        FixtureHttpServer fixtureHttp;
        QVERIFY(fixtureHttp.start());
        fixtureHttp.addRoute(
            "/forge-maven/net/minecraftforge/forge/maven-metadata.xml",
            R"(<metadata><versioning><versions><version>1.21.1-52.0.1</version></versions></versioning></metadata>)");
        fixtureHttp.addRoute(
            "/forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/forge-1.21.1-52.0.1-installer.jar",
            "synthetic forge installer");

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.forgeMavenBase = fixtureHttp.baseUrl("forge-maven");
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        QSignalSpy statuses(&downloader, &ServerDownloader::statusMessage);
        const QString destination = temporaryRoot.filePath("forge-server");
        downloader.startDownload("1.21.1", "forge", destination,
                                 fakeMinecraftServerPath(), "52.0.1");

        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished.constFirst().at(0).toBool(),
                 qPrintable(finished.constFirst().at(1).toString()));
        bool reportedMissingChecksum = false;
        for (const QList<QVariant>& status : statuses) {
            if (status.constFirst().toString()
                == "No checksum is published for this installer; continuing without verification.") {
                reportedMissingChecksum = true;
                break;
            }
        }
        QVERIFY(reportedMissingChecksum);
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("run.bat"))
                || QFileInfo::exists(QDir(destination).filePath("run.sh")));
    }

    void rejectsFabricDownloadThatIsNotAJar()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        FixtureHttpServer fixtureHttp;
        QVERIFY(fixtureHttp.start());
        fixtureHttp.addRoute("/fabric/versions/installer",
                             R"([{"version":"1.0.0","stable":true}])");
        fixtureHttp.addRoute("/fabric/versions/loader/1.21.8",
                             R"([{"loader":{"version":"0.16.10"}}])");
        fixtureHttp.addRoute(
            "/fabric/versions/loader/1.21.8/0.16.10/1.0.0/server/jar",
            "not a zip archive");

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.fabricApiBase = fixtureHttp.baseUrl("fabric");
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        const QString destination = temporaryRoot.filePath("fabric-server");
        downloader.startDownload("1.21.8", "fabric", destination);

        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(!finished.constFirst().at(0).toBool());
        QCOMPARE(finished.constFirst().at(1).toString(),
                 QString("The downloaded Fabric server launcher is not a valid jar file."));
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath("server.jar")));
    }
};

QTEST_GUILESS_MAIN(ServerSoftwareDownloadTest)

#include "ServerSoftwareDownload_test.moc"
