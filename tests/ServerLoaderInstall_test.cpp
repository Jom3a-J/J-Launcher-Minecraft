// SPDX-License-Identifier: GPL-3.0-only

// Installing Forge and NeoForge servers: installer downloads, library prefetch and failure handling.

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

class ServerLoaderInstallTest : public QObject {
    Q_OBJECT

   private slots:
    void init()
    {
        ServerInstance::clearJavaProbeCacheForTesting();
    }

    void installsExactLoaderVersionsRequestedByAModpack()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QUrl legacyVanillaManifest = writeVanillaFileFixture(
            root.path(), QStringLiteral("1.12.2"), QByteArrayLiteral("exact legacy vanilla server"));
        QVERIFY(!legacyVanillaManifest.isEmpty());

        FixtureHttpServer fixtureHttp;
        QVERIFY(fixtureHttp.start());
        const QByteArray exactPaperServer("exact paper server");
        fixtureHttp.addRoute(
            "/paper/projects/paper/versions/1.21.1/builds",
            QJsonDocument(QJsonArray{
                QJsonObject{
                    { "id", 130 }, { "channel", "STABLE" },
                    { "downloads", QJsonObject{{ "server:default", QJsonObject{
                        { "url", fixtureHttp.url("/downloads/paper-130.jar").toString() },
                        { "checksums", QJsonObject{{ "sha256", QString::fromLatin1(
                            QCryptographicHash::hash(exactPaperServer, QCryptographicHash::Sha256).toHex()) }} },
                    } }} },
                },
                QJsonObject{
                    { "id", 128 }, { "channel", "STABLE" },
                    { "downloads", QJsonObject{{ "server:default", QJsonObject{
                        { "url", fixtureHttp.url("/downloads/paper-128.jar").toString() },
                        { "checksums", QJsonObject{{ "sha256", QString::fromLatin1(
                            QCryptographicHash::hash(exactPaperServer, QCryptographicHash::Sha256).toHex()) }} },
                    } }} },
                },
            }).toJson(QJsonDocument::Compact));
        fixtureHttp.addRoute("/downloads/paper-128.jar", exactPaperServer);
        fixtureHttp.addRoute("/fabric/versions/installer",
                             R"([{"version":"1.0.0","stable":true}])");
        fixtureHttp.addRoute(
            "/fabric/versions/loader/1.21.1",
            R"([{"loader":{"version":"0.17.0"}},{"loader":{"version":"0.16.10"}}])");
        fixtureHttp.addRoute(
            "/fabric/versions/loader/1.21.1/0.16.10/1.0.0/server/jar",
            fabricServerJarFixture(root.filePath("fixtures/exact-fabric-server.jar")));
        fixtureHttp.addRoute("/purpur/purpur/1.21.1",
                             R"({"builds":{"latest":"2412","all":[2400,2412]}})");
        const QByteArray exactPurpurServer("exact purpur server");
        fixtureHttp.addRoute("/purpur/purpur/1.21.1/2400",
            QJsonDocument(QJsonObject{{ "md5", QString::fromLatin1(
                QCryptographicHash::hash(exactPurpurServer, QCryptographicHash::Md5).toHex()) }})
                .toJson(QJsonDocument::Compact));
        fixtureHttp.addRoute("/purpur/purpur/1.21.1/2400/download",
                             exactPurpurServer);

        QVERIFY(writeFile(
            root.filePath(
                "forge-maven/net/minecraftforge/forge/1.12.2-14.23.5.2860/"
                "forge-1.12.2-14.23.5.2860-installer.jar"),
            "exact forge installer"));
        QVERIFY(writeInstallerChecksum(root.filePath(
            "forge-maven/net/minecraftforge/forge/1.12.2-14.23.5.2860/"
            "forge-1.12.2-14.23.5.2860-installer.jar")));
        QVERIFY(!QFileInfo::exists(
            root.filePath("forge-maven/net/minecraftforge/forge/maven-metadata.xml")));
        QVERIFY(writeFile(
            root.filePath(
                "neoforge-maven/net/neoforged/neoforge/21.1.233/"
                "neoforge-21.1.233-installer.jar"),
            "exact neoforge installer"));
        QVERIFY(writeInstallerChecksum(root.filePath(
            "neoforge-maven/net/neoforged/neoforge/21.1.233/"
            "neoforge-21.1.233-installer.jar")));

        ServerProviderEndpoints endpoints{
            legacyVanillaManifest,
            fixtureHttp.baseUrl("paper"),
            fixtureHttp.baseUrl("fabric"),
            fixtureHttp.baseUrl("purpur"),
            fixtureHttp.url("/missing/forge-promotions"),
            directoryUrl(root.filePath("forge-maven")),
            fixtureHttp.url("/missing/neoforge-versions"),
            directoryUrl(root.filePath("neoforge-maven")),
        };

        const QList<QStringList> providers{
            { "paper", "1.21.1", "128" },
            { "fabric", "1.21.1", "0.16.10" },
            { "purpur", "1.21.1", "2400" },
            { "forge", "1.12.2", "14.23.5.2860" },
            { "neoforge", "1.21.1", "21.1.233" },
        };
        for (const QStringList &provider : providers) {
            ServerDownloader downloader(endpoints);
            QSignalSpy finished(&downloader, &ServerDownloader::finished);
            const QString destination = root.filePath("exact/" + provider.at(0));
            downloader.startDownload(provider.at(1), provider.at(0), destination,
                                     fakeMinecraftServerPath(), provider.at(2));
            QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
            QVERIFY2(finished.last().at(0).toBool(),
                     qPrintable(provider.at(0) + ": "
                                + finished.last().at(1).toString()));
            QCOMPARE(downloader.resolvedLoaderVersion(), provider.at(2));
            if (provider.at(0) == QStringLiteral("forge")
                || provider.at(0) == QStringLiteral("neoforge")) {
                QVERIFY(!QFileInfo::exists(
                    serverLoaderInstallIncompleteMarkerPath(destination)));
            }
        }
    }

    void resolvesForgeMavenCoordinatesAndPrefetchesLegacyServerJar()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());

        const QString legacyBuild = QStringLiteral("10.13.4.1614");
        const QString legacyMavenVersion = QStringLiteral("1.7.10-10.13.4.1614-1.7.10");
        const QString legacyInstaller = root.filePath(
            QStringLiteral("legacy-forge-maven/net/minecraftforge/forge/%1/forge-%1-installer.jar")
                .arg(legacyMavenVersion));
        QVERIFY(writeFile(legacyInstaller, "suffixed legacy Forge installer"));
        QVERIFY(writeInstallerChecksum(legacyInstaller));
        QVERIFY(writeFile(
            root.filePath("legacy-forge-maven/net/minecraftforge/forge/maven-metadata.xml"),
            QStringLiteral("<metadata><versioning><versions><version>%1</version>"
                           "</versions></versioning></metadata>").arg(legacyMavenVersion).toUtf8()));

        const QByteArray legacyVanillaServer("verified 1.7.10 vanilla server payload");
        const QUrl legacyManifest = writeVanillaFileFixture(
            root.path(), QStringLiteral("1.7.10"), legacyVanillaServer);
        QVERIFY(!legacyManifest.isEmpty());
        const QString legacyDestination = root.filePath("legacy-forge-server");
        const QString legacyServerJar = QDir(legacyDestination).filePath(
            "minecraft_server.1.7.10.jar");
        const QString legacyObserved = root.filePath("legacy-installer-server-jar-state");

        ServerProviderEndpoints legacyEndpoints = ServerProviderEndpoints::production();
        legacyEndpoints.vanillaManifest = legacyManifest;
        legacyEndpoints.forgeMavenBase = directoryUrl(root.filePath("legacy-forge-maven"));
        ServerDownloader legacyDownloader(legacyEndpoints);
        QSignalSpy legacyFinished(&legacyDownloader, &ServerDownloader::finished);
        ScopedEnvironmentVariable legacyServerJarPath(
            "JLAUNCHER_TEST_INSTALLER_SERVER_JAR_PATH", legacyServerJar.toLocal8Bit());
        ScopedEnvironmentVariable legacyObservedPath(
            "JLAUNCHER_TEST_INSTALLER_SERVER_JAR_OBSERVED_FILE", legacyObserved.toLocal8Bit());

        legacyDownloader.startDownload(QStringLiteral("1.7.10"), QStringLiteral("forge"),
                                       legacyDestination, fakeMinecraftServerPath(), legacyBuild);
        QTRY_COMPARE_WITH_TIMEOUT(legacyFinished.size(), 1, 10000);
        QVERIFY2(legacyFinished.constFirst().at(0).toBool(),
                 qPrintable(legacyFinished.constFirst().at(1).toString()));
        QCOMPARE(legacyDownloader.resolvedLoaderVersion(), legacyBuild);
        QCOMPARE(readFile(legacyServerJar), legacyVanillaServer);
        QCOMPARE(readFile(legacyObserved), QByteArray("present"));

        const QString modernVersion = QStringLiteral("1.13.2");
        const QString modernBuild = QStringLiteral("25.0.223");
        const QString modernMavenVersion = modernVersion + QLatin1Char('-') + modernBuild;
        const QString modernMavenRoot = root.filePath("modern-forge-maven");
        QVERIFY(writeFile(
            QDir(modernMavenRoot).filePath(
                QStringLiteral("net/minecraftforge/forge/%1/forge-%1-installer.jar")
                    .arg(modernMavenVersion)),
            "unsuffixed modern Forge installer"));
        QVERIFY(writeInstallerChecksum(QDir(modernMavenRoot).filePath(
            QStringLiteral("net/minecraftforge/forge/%1/forge-%1-installer.jar")
                .arg(modernMavenVersion))));
        QVERIFY(writeFile(
            QDir(modernMavenRoot).filePath("net/minecraftforge/forge/maven-metadata.xml"),
            QStringLiteral("<metadata><versioning><versions><version>%1</version>"
                           "</versions></versioning></metadata>").arg(modernMavenVersion).toUtf8()));

        const QByteArray modernVanillaServer("modern vanilla payload should stay unused");
        const QUrl modernManifest = writeVanillaFileFixture(
            root.path(), modernVersion, modernVanillaServer);
        QVERIFY(!modernManifest.isEmpty());
        const QString modernDestination = root.filePath("modern-forge-server");
        const QString modernServerJar = QDir(modernDestination).filePath(
            QStringLiteral("minecraft_server.%1.jar").arg(modernVersion));
        const QString modernObserved = root.filePath("modern-installer-server-jar-state");

        ServerProviderEndpoints modernEndpoints = ServerProviderEndpoints::production();
        modernEndpoints.vanillaManifest = modernManifest;
        modernEndpoints.forgeMavenBase = directoryUrl(modernMavenRoot);
        ServerDownloader modernDownloader(modernEndpoints);
        QSignalSpy modernFinished(&modernDownloader, &ServerDownloader::finished);
        ScopedEnvironmentVariable modernServerJarPath(
            "JLAUNCHER_TEST_INSTALLER_SERVER_JAR_PATH", modernServerJar.toLocal8Bit());
        ScopedEnvironmentVariable modernObservedPath(
            "JLAUNCHER_TEST_INSTALLER_SERVER_JAR_OBSERVED_FILE", modernObserved.toLocal8Bit());

        modernDownloader.startDownload(modernVersion, QStringLiteral("forge"),
                                       modernDestination, fakeMinecraftServerPath(), modernBuild);
        QTRY_COMPARE_WITH_TIMEOUT(modernFinished.size(), 1, 10000);
        QVERIFY2(modernFinished.constFirst().at(0).toBool(),
                 qPrintable(modernFinished.constFirst().at(1).toString()));
        QCOMPARE(modernDownloader.resolvedLoaderVersion(), modernBuild);
        QCOMPARE(readFile(modernObserved), QByteArray("missing"));
        QVERIFY(!QFileInfo::exists(modernServerJar));
    }

    void prefetchesModernForgeLibrariesBeforeInstallerAndReusesVerifiedFiles()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        RangeHttpServer http;
        QVERIFY(http.start());
        const QByteArray profileBytes("profile processor library");
        const QByteArray versionBytes("version library");
        const QByteArray cachedBytes("cached library");
        const QByteArray vanillaBytes("vanilla server library");
        http.serve("/libraries/profile.jar", httpResource(profileBytes));
        http.serve("/libraries/version.jar", httpResource(versionBytes));
        http.serve("/libraries/vanilla-server.jar", httpResource(vanillaBytes));

        const QString installerPath = root.filePath(
            "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/forge-1.21.1-52.0.1-installer.jar");
        QVERIFY(writeModernInstaller(installerPath,
            QJsonArray{ installerLibrary("test:profile:1", "test/profile/1/profile-1.jar",
                                         http.url("/libraries/profile.jar"), profileBytes) },
            QJsonArray{
                installerLibrary("test:version:1", "test/version/1/version-1.jar",
                                 http.url("/libraries/version.jar"), versionBytes),
                installerLibrary("test:cached:1", "test/cached/1/cached-1.jar",
                                 http.url("/libraries/cached.jar"), cachedBytes),
                installerLibrary("net.minecraft:server:1.21.1",
                                 "net/minecraft/server/1.21.1/server-1.21.1.jar",
                                 http.url("/libraries/vanilla-server.jar"), vanillaBytes),
            }));

        const QString destination = root.filePath("forge-server");
        const QString profilePath = QDir(destination).filePath(
            "libraries/test/profile/1/profile-1.jar");
        const QString versionPath = QDir(destination).filePath(
            "libraries/test/version/1/version-1.jar");
        const QString cachedPath = QDir(destination).filePath(
            "libraries/test/cached/1/cached-1.jar");
        const QString vanillaPath = QDir(destination).filePath(
            "libraries/net/minecraft/server/1.21.1/server-1.21.1.jar");
        QVERIFY(writeFile(cachedPath, cachedBytes));
        const QString observedPath = root.filePath("installer-libraries");
        const QString countPath = root.filePath("installer-count");

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.forgeMavenBase = directoryUrl(root.filePath("forge-maven"));
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        ScopedEnvironmentVariable requiredLibraries(
            "JLAUNCHER_TEST_INSTALLER_REQUIRED_LIBRARIES",
            QStringList{ profilePath, versionPath, cachedPath, vanillaPath }.join(';').toLocal8Bit());
        ScopedEnvironmentVariable observedLibraries(
            "JLAUNCHER_TEST_INSTALLER_LIBRARIES_OBSERVED_FILE", observedPath.toLocal8Bit());
        ScopedEnvironmentVariable installerCount(
            "JLAUNCHER_TEST_INSTALLER_COUNT_FILE", countPath.toLocal8Bit());

        downloader.startDownload("1.21.1", "forge", destination,
                                 fakeMinecraftServerPath(), "52.0.1");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished.constFirst().at(0).toBool(),
                 qPrintable(finished.constFirst().at(1).toString()));
        QCOMPARE(readFile(profilePath), profileBytes);
        QCOMPARE(readFile(versionPath), versionBytes);
        QCOMPARE(readFile(cachedPath), cachedBytes);
        QCOMPARE(readFile(vanillaPath), vanillaBytes);
        QCOMPARE(readFile(observedPath), QByteArray("present\npresent\npresent\npresent\n"));
        QCOMPARE(readFile(countPath), QByteArray("1"));
        QCOMPARE(http.requestCount(), 3);
    }

    void prefetchesModernNeoForgeLibrariesBeforeInstaller()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        RangeHttpServer http;
        QVERIFY(http.start());
        const QByteArray libraryBytes("NeoForge processor library");
        http.serve("/libraries/neoforge.jar", httpResource(libraryBytes));
        const QString installerPath = root.filePath(
            "neoforge-maven/net/neoforged/neoforge/21.1.50/neoforge-21.1.50-installer.jar");
        QVERIFY(writeModernInstaller(installerPath,
            QJsonArray{ installerLibrary("test:neoforge:1", "test/neoforge/1/neoforge-1.jar",
                                         http.url("/libraries/neoforge.jar"), libraryBytes) }, {}));

        const QString destination = root.filePath("neoforge-server");
        const QString libraryPath = QDir(destination).filePath(
            "libraries/test/neoforge/1/neoforge-1.jar");
        const QString observedPath = root.filePath("installer-libraries");
        const QString countPath = root.filePath("installer-count");
        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.neoForgeMavenBase = directoryUrl(root.filePath("neoforge-maven"));
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        ScopedEnvironmentVariable requiredLibraries(
            "JLAUNCHER_TEST_INSTALLER_REQUIRED_LIBRARIES", libraryPath.toLocal8Bit());
        ScopedEnvironmentVariable observedLibraries(
            "JLAUNCHER_TEST_INSTALLER_LIBRARIES_OBSERVED_FILE", observedPath.toLocal8Bit());
        ScopedEnvironmentVariable installerCount(
            "JLAUNCHER_TEST_INSTALLER_COUNT_FILE", countPath.toLocal8Bit());

        downloader.startDownload("1.21.1", "neoforge", destination,
                                 fakeMinecraftServerPath(), "21.1.50");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished.constFirst().at(0).toBool(),
                 qPrintable(finished.constFirst().at(1).toString()));
        QCOMPARE(readFile(libraryPath), libraryBytes);
        QCOMPARE(readFile(observedPath), QByteArray("present\n"));
        QCOMPARE(readFile(countPath), QByteArray("1"));
        QCOMPARE(http.requestCount(), 1);
    }

    void continuesForgeInstallWhenModernLibraryPrefetchHas404()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        RangeHttpServer http;
        QVERIFY(http.start());
        const QByteArray availableBytes("available library");
        http.serve("/libraries/available.jar", httpResource(availableBytes));
        const QString installerPath = root.filePath(
            "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/forge-1.21.1-52.0.1-installer.jar");
        QVERIFY(writeModernInstaller(installerPath, {}, QJsonArray{
            installerLibrary("test:available:1", "test/available/1/available-1.jar",
                             http.url("/libraries/available.jar"), availableBytes),
            installerLibrary("test:missing:1", "test/missing/1/missing-1.jar",
                             http.url("/libraries/missing.jar"), QByteArray("missing library")),
        }));

        const QString destination = root.filePath("forge-server");
        const QString availablePath = QDir(destination).filePath(
            "libraries/test/available/1/available-1.jar");
        const QString missingPath = QDir(destination).filePath(
            "libraries/test/missing/1/missing-1.jar");
        const QString observedPath = root.filePath("installer-libraries");
        const QString countPath = root.filePath("installer-count");
        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.forgeMavenBase = directoryUrl(root.filePath("forge-maven"));
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        ScopedEnvironmentVariable requiredLibraries(
            "JLAUNCHER_TEST_INSTALLER_REQUIRED_LIBRARIES",
            QStringList{ availablePath, missingPath }.join(';').toLocal8Bit());
        ScopedEnvironmentVariable observedLibraries(
            "JLAUNCHER_TEST_INSTALLER_LIBRARIES_OBSERVED_FILE", observedPath.toLocal8Bit());
        ScopedEnvironmentVariable installerCount(
            "JLAUNCHER_TEST_INSTALLER_COUNT_FILE", countPath.toLocal8Bit());
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression("Forge library prefetch failed for 1 file.*"));

        downloader.startDownload("1.21.1", "forge", destination,
                                 fakeMinecraftServerPath(), "52.0.1");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished.constFirst().at(0).toBool(),
                 qPrintable(finished.constFirst().at(1).toString()));
        QCOMPARE(readFile(availablePath), availableBytes);
        QVERIFY(!QFileInfo::exists(missingPath));
        QCOMPARE(readFile(observedPath), QByteArray("present\nmissing\n"));
        QCOMPARE(readFile(countPath), QByteArray("1"));
    }

    void skipsModernLibraryPrefetchForInstallerWithoutModernProfile()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        RangeHttpServer http;
        QVERIFY(http.start());
        const QByteArray libraryBytes("must not prefetch");
        const QString installerPath = root.filePath(
            "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/forge-1.21.1-52.0.1-installer.jar");
        const QByteArray profile = QJsonDocument(QJsonObject{
            { "libraries", QJsonArray{ installerLibrary(
                "test:legacy:1", "test/legacy/1/legacy-1.jar",
                http.url("/libraries/legacy.jar"), libraryBytes) } },
        }).toJson(QJsonDocument::Compact);
        QVERIFY(writeArchive(installerPath, { { "install_profile.json", profile } }));
        QVERIFY(writeInstallerChecksum(installerPath));

        const QString destination = root.filePath("forge-server");
        const QString countPath = root.filePath("installer-count");
        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.forgeMavenBase = directoryUrl(root.filePath("forge-maven"));
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        ScopedEnvironmentVariable installerCount(
            "JLAUNCHER_TEST_INSTALLER_COUNT_FILE", countPath.toLocal8Bit());

        downloader.startDownload("1.21.1", "forge", destination,
                                 fakeMinecraftServerPath(), "52.0.1");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished.constFirst().at(0).toBool(),
                 qPrintable(finished.constFirst().at(1).toString()));
        QCOMPARE(http.requestCount(), 0);
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath(
            "libraries/test/legacy/1/legacy-1.jar")));
        QCOMPARE(readFile(countPath), QByteArray("1"));
    }

    void cancellingModernLibraryPrefetchLeavesMarkerAndSkipsInstaller()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        RangeHttpServer http;
        QVERIFY(http.start());
        const QByteArray libraryBytes("stalled library");
        RangeHttpServer::Resource stalled = httpResource(libraryBytes);
        stalled.stallAfter = 0;
        http.serve("/libraries/stalled.jar", stalled);
        const QString installerPath = root.filePath(
            "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/forge-1.21.1-52.0.1-installer.jar");
        QVERIFY(writeModernInstaller(installerPath, {}, QJsonArray{
            installerLibrary("test:stalled:1", "test/stalled/1/stalled-1.jar",
                             http.url("/libraries/stalled.jar"), libraryBytes),
        }));

        const QString destination = root.filePath("forge-server");
        const QString countPath = root.filePath("installer-count");
        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.forgeMavenBase = directoryUrl(root.filePath("forge-maven"));
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        ScopedEnvironmentVariable installerCount(
            "JLAUNCHER_TEST_INSTALLER_COUNT_FILE", countPath.toLocal8Bit());

        downloader.startDownload("1.21.1", "forge", destination,
                                 fakeMinecraftServerPath(), "52.0.1");
        QTRY_VERIFY_WITH_TIMEOUT(http.requestCount() > 0, 10000);
        QVERIFY(QFileInfo::exists(serverLoaderInstallIncompleteMarkerPath(destination)));
        downloader.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(finished.constFirst().at(0).toBool(), false);
        QCOMPARE(finished.constFirst().at(1).toString(), QString("Download cancelled."));
        QVERIFY(QFileInfo::exists(serverLoaderInstallIncompleteMarkerPath(destination)));
        QVERIFY(!QFileInfo::exists(countPath));
    }

    void failedForgeInstallerMarksTheServerUnlaunchable()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString destination = root.filePath("failed-forge");
        const QString loaderJar = QDir(destination).filePath(
            "forge-26.3-66.0.4-shim.jar");
        QVERIFY(writeFile(
            root.filePath(
                "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
                "forge-1.21.1-52.0.1-installer.jar"),
            "synthetic forge installer"));
        QVERIFY(writeInstallerChecksum(root.filePath(
            "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
            "forge-1.21.1-52.0.1-installer.jar")));

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.forgeMavenBase = directoryUrl(root.filePath("forge-maven"));
        ServerInstance server("failed-forge", "Failed Forge", endpoints);
        server.setServerDirectory(destination);
        server.setVersion("1.21.1");
        server.setLoaderType("forge");
        server.setLoaderVersion("52.0.1");
        server.setJavaPath(fakeMinecraftServerPath());
        QSignalSpy finished(&server, &ServerInstance::serverSoftwareDownloadFinished);
        ScopedEnvironmentVariable fakeLoaderJar(
            "JLAUNCHER_TEST_INSTALLER_LOADER_JAR", loaderJar.toLocal8Bit());
        const QString countPath = root.filePath("installer-count");
        ScopedEnvironmentVariable installerCount(
            "JLAUNCHER_TEST_INSTALLER_COUNT_FILE", countPath.toLocal8Bit());

        {
            ScopedEnvironmentVariable exitCode("JLAUNCHER_TEST_INSTALLER_EXIT_CODE", "7");
            QVERIFY(server.prepareServerSoftware());
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
            QVERIFY(!finished.constFirst().at(1).toBool());
        }

        QVERIFY(QFileInfo::exists(loaderJar));
        QVERIFY(QFileInfo::exists(
            serverLoaderInstallIncompleteMarkerPath(destination)));
        QCOMPARE(server.serverJarPath(), QFileInfo(loaderJar).absoluteFilePath());
        QVERIFY(!server.hasInstalledLaunchTarget());
        QCOMPARE(readFile(countPath), QByteArray("1"));

        {
            ScopedEnvironmentVariable exitCode("JLAUNCHER_TEST_INSTALLER_EXIT_CODE", "0");
            QVERIFY(server.prepareServerSoftware());
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
            QVERIFY(finished.at(1).at(1).toBool());
        }
        QCOMPARE(readFile(countPath), QByteArray("2"));
        QVERIFY(!QFileInfo::exists(
            serverLoaderInstallIncompleteMarkerPath(destination)));
    }

    void cancellingForgeInstallerStopsItAndPrepareRetries()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString destination = root.filePath("cancelled-forge");
        const QString loaderJar = QDir(destination).filePath(
            "forge-26.3-66.0.4-shim.jar");
        const QString installerPath = QDir(destination).filePath("forge-installer.jar");
        const QString installerLogPath = installerPath + QStringLiteral(".log");
        const QString readyPath = root.filePath("installer-ready");
        const QString completedPath = root.filePath("installer-completed");
        const QString countPath = root.filePath("installer-count");
        QVERIFY(writeFile(
            root.filePath(
                "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
                "forge-1.21.1-52.0.1-installer.jar"),
            "synthetic forge installer"));
        QVERIFY(writeInstallerChecksum(root.filePath(
            "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
            "forge-1.21.1-52.0.1-installer.jar")));

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.forgeMavenBase = directoryUrl(root.filePath("forge-maven"));
        ServerInstance server("cancelled-forge", "Cancelled Forge", endpoints);
        server.setServerDirectory(destination);
        server.setVersion("1.21.1");
        server.setLoaderType("forge");
        server.setLoaderVersion("52.0.1");
        server.setJavaPath(fakeMinecraftServerPath());
        QSignalSpy finished(&server, &ServerInstance::serverSoftwareDownloadFinished);
        ScopedEnvironmentVariable fakeLoaderJar(
            "JLAUNCHER_TEST_INSTALLER_LOADER_JAR", loaderJar.toLocal8Bit());
        ScopedEnvironmentVariable installerCount(
            "JLAUNCHER_TEST_INSTALLER_COUNT_FILE", countPath.toLocal8Bit());
        {
            ScopedEnvironmentVariable delay("JLAUNCHER_TEST_INSTALLER_DELAY_MS", "2000");
            ScopedEnvironmentVariable ready(
                "JLAUNCHER_TEST_INSTALLER_READY_FILE", readyPath.toLocal8Bit());
            ScopedEnvironmentVariable completed(
                "JLAUNCHER_TEST_INSTALLER_COMPLETED_FILE", completedPath.toLocal8Bit());
            ScopedEnvironmentVariable createLog("JLAUNCHER_TEST_INSTALLER_CREATE_LOG", "1");

            QVERIFY(server.prepareServerSoftware());
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(readyPath), 10000);
            QVERIFY(QFileInfo::exists(installerLogPath));
            QVERIFY(server.cancelDownload());
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
            QCOMPARE(finished.constFirst().at(1).toBool(), false);
            QCOMPARE(finished.constFirst().at(2).toBool(), true);
        }

        QTest::qWait(2200);
        QVERIFY(!QFileInfo::exists(completedPath));
        QVERIFY(QFileInfo::exists(
            serverLoaderInstallIncompleteMarkerPath(destination)));
        QVERIFY(!QFileInfo::exists(installerPath));
        QVERIFY(!QFileInfo::exists(installerLogPath));
        QVERIFY(QFileInfo::exists(loaderJar));
        QCOMPARE(server.serverJarPath(), QFileInfo(loaderJar).absoluteFilePath());
        QCOMPARE(readFile(countPath), QByteArray("1"));

        QVERIFY(!server.hasInstalledLaunchTarget());
        QVERIFY(server.prepareServerSoftware());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
        QCOMPARE(finished.at(1).at(1).toBool(), true);
        QCOMPARE(readFile(countPath), QByteArray("2"));
        QVERIFY(!QFileInfo::exists(
            serverLoaderInstallIncompleteMarkerPath(destination)));
        QVERIFY(server.hasInstalledLaunchTarget());
    }

    void existingForgeJarWithoutIncompleteMarkerRemainsReady()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString destination = temporaryRoot.filePath("existing-forge");
        const QString loaderJar = QDir(destination).filePath(
            "forge-1.21.1-52.0.1-shim.jar");
        QVERIFY(writeFile(loaderJar, "existing forge server jar"));

        ServerInstance server("existing-forge", "Existing Forge");
        server.setServerDirectory(destination);
        server.setLoaderType("forge");
        QVERIFY(!QFileInfo::exists(
            serverLoaderInstallIncompleteMarkerPath(destination)));
        QVERIFY(server.hasInstalledLaunchTarget());
        QVERIFY(server.prepareServerSoftware());
    }

    void rejectsForgeInstallWhenMinecraftServerPayloadIsMissing()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        QVERIFY(writeFile(
            root.filePath(
                "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
                "forge-1.21.1-52.0.1-installer.jar"),
            "synthetic incomplete forge installer"));
        QVERIFY(writeInstallerChecksum(root.filePath(
            "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
            "forge-1.21.1-52.0.1-installer.jar")));

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.forgeMavenBase = directoryUrl(root.filePath("forge-maven"));
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        const QString destination = root.filePath("incomplete-forge");
        ScopedEnvironmentVariable omitPayload(
            "JLAUNCHER_TEST_OMIT_SERVER_PAYLOAD", "1");

        downloader.startDownload("1.21.1", "forge", destination,
                                 fakeMinecraftServerPath(), "52.0.1");
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QVERIFY(!finished.last().at(0).toBool());
        QVERIFY(finished.last().at(1).toString().contains(
            "did not download the Minecraft server files"));
        QVERIFY(QFileInfo::exists(
            serverLoaderInstallIncompleteMarkerPath(destination)));
        QVERIFY(QFileInfo::exists(
            serverLoaderInstallIncompleteMarkerPath(destination)));
#ifdef Q_OS_WIN
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("run.bat")));
#else
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("run.sh")));
#endif
    }

    void rejectsForgeUpdateThatOnlyLeavesStaleLoaderArguments()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        QVERIFY(writeFile(
            root.filePath(
                "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
                "forge-1.21.1-52.0.1-installer.jar"),
            "synthetic incomplete forge update installer"));
        QVERIFY(writeInstallerChecksum(root.filePath(
            "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
            "forge-1.21.1-52.0.1-installer.jar")));

        const QString destination = root.filePath("existing-forge");
#ifdef Q_OS_WIN
        QVERIFY(writeFile(QDir(destination).filePath("run.bat"),
                          "java @libraries/old/win_args.txt %*\r\n"));
        QVERIFY(writeFile(QDir(destination).filePath("libraries/old/win_args.txt"),
                          "-jar old-loader.jar\r\n"));
#else
        QVERIFY(writeFile(QDir(destination).filePath("run.sh"),
                          "java @libraries/old/unix_args.txt \"$@\"\n"));
        QVERIFY(writeFile(QDir(destination).filePath("libraries/old/unix_args.txt"),
                          "-jar old-loader.jar\n"));
#endif
        QVERIFY(writeFile(
            QDir(destination).filePath(
                "libraries/net/minecraft/server/1.21.1/server-1.21.1-bundled.jar"),
            "existing minecraft server payload"));

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.forgeMavenBase = directoryUrl(root.filePath("forge-maven"));
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);

        downloader.startDownload("1.21.1", "forge", destination,
                                 fakeMinecraftServerPath(), "52.0.1");
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QVERIFY(!finished.last().at(0).toBool());
        QVERIFY(finished.last().at(1).toString().contains(
            "did not create its loader argument file"));
    }

    void rejectsForgeUpdateWhoseScriptStillTargetsTheOldBuild()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        QVERIFY(writeFile(
            root.filePath(
                "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
                "forge-1.21.1-52.0.1-installer.jar"),
            "synthetic incomplete forge update installer"));
        QVERIFY(writeInstallerChecksum(root.filePath(
            "forge-maven/net/minecraftforge/forge/1.21.1-52.0.1/"
            "forge-1.21.1-52.0.1-installer.jar")));

        const QString destination = root.filePath("partially-updated-forge");
#ifdef Q_OS_WIN
        QVERIFY(writeFile(QDir(destination).filePath("run.bat"),
                          "java @libraries/old/win_args.txt %*\r\n"));
        QVERIFY(writeFile(
            QDir(destination).filePath(
                "libraries/net/minecraftforge/forge/1.21.1-52.0.1/win_args.txt"),
            "-jar new-loader.jar\r\n"));
#else
        QVERIFY(writeFile(QDir(destination).filePath("run.sh"),
                          "java @libraries/old/unix_args.txt \"$@\"\n"));
        QVERIFY(writeFile(
            QDir(destination).filePath(
                "libraries/net/minecraftforge/forge/1.21.1-52.0.1/unix_args.txt"),
            "-jar new-loader.jar\n"));
#endif
        QVERIFY(writeFile(
            QDir(destination).filePath(
                "libraries/net/minecraft/server/1.21.1/server-1.21.1-bundled.jar"),
            "existing minecraft server payload"));

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.forgeMavenBase = directoryUrl(root.filePath("forge-maven"));
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);

        downloader.startDownload("1.21.1", "forge", destination,
                                 fakeMinecraftServerPath(), "52.0.1");
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QVERIFY(!finished.last().at(0).toBool());
        QVERIFY(finished.last().at(1).toString().contains(
            "did not connect", Qt::CaseInsensitive));
    }

    void installsLegacyNeoForgeServerFromForgeStyleCoordinates()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        QVERIFY(writeFile(
            root.filePath(
                "neoforge-maven/net/neoforged/forge/1.20.1-47.1.106/"
                "forge-1.20.1-47.1.106-installer.jar"),
            "legacy neoforge installer"));
        QVERIFY(writeInstallerChecksum(root.filePath(
            "neoforge-maven/net/neoforged/forge/1.20.1-47.1.106/"
            "forge-1.20.1-47.1.106-installer.jar")));

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.neoForgeMavenBase = directoryUrl(
            root.filePath("neoforge-maven"));
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        const QString destination = root.filePath("legacy-neoforge-server");

        downloader.startDownload("1.20.1", "neoforge", destination,
                                 fakeMinecraftServerPath(), "47.1.106");
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QVERIFY2(finished.last().at(0).toBool(),
                 qPrintable(finished.last().at(1).toString()));
        QCOMPARE(downloader.resolvedLoaderVersion(), QString("47.1.106"));
#ifdef Q_OS_WIN
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("run.bat")));
#else
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("run.sh")));
#endif
        QVERIFY(QFileInfo::exists(QDir(destination).filePath(
            "libraries/net/minecraft/server/1.20.1/server-1.20.1-bundled.jar")));
    }

    void resolvesAndInstallsLatestLegacyNeoForgeBuild()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        QVERIFY(writeFile(
            root.filePath("neoforge-maven/net/neoforged/forge/maven-metadata.xml"),
            R"(<?xml version="1.0"?><metadata><versioning><versions><version>1.20.1-47.1.105</version><version>1.20.1-47.1.106</version><version>1.21.1-52.0.1</version></versions></versioning></metadata>)"));
        QVERIFY(writeFile(
            root.filePath(
                "neoforge-maven/net/neoforged/forge/1.20.1-47.1.106/"
                "forge-1.20.1-47.1.106-installer.jar"),
            "latest legacy neoforge installer"));
        QVERIFY(writeInstallerChecksum(root.filePath(
            "neoforge-maven/net/neoforged/forge/1.20.1-47.1.106/"
            "forge-1.20.1-47.1.106-installer.jar")));

        ServerProviderEndpoints endpoints = ServerProviderEndpoints::production();
        endpoints.neoForgeMavenBase = directoryUrl(
            root.filePath("neoforge-maven"));
        ServerDownloader downloader(endpoints);
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        const QString destination = root.filePath("resolved-legacy-neoforge-server");

        // The New Server flow intentionally leaves the loader build blank;
        // the downloader must resolve and persist the newest published build.
        downloader.startDownload("1.20.1", "neoforge", destination,
                                 fakeMinecraftServerPath());
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QVERIFY2(finished.last().at(0).toBool(),
                 qPrintable(finished.last().at(1).toString()));
        QCOMPARE(downloader.resolvedLoaderVersion(), QString("47.1.106"));
#ifdef Q_OS_WIN
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("run.bat")));
#else
        QVERIFY(QFileInfo::exists(QDir(destination).filePath("run.sh")));
#endif
    }

    void installsLiveLoaderServer_data()
    {
        QTest::addColumn<QString>("provider");
        QTest::addColumn<QString>("minecraftVersion");

        QTest::newRow("forge 1.20.1")
            << QString("forge") << QString("1.20.1");
        QTest::newRow("legacy neoforge 1.20.1")
            << QString("neoforge") << QString("1.20.1");
    }

    void installsLiveLoaderServer()
    {
        const QString javaPath = qEnvironmentVariable(
            "JLAUNCHER_LIVE_SERVER_JAVA").trimmed();
        if (javaPath.isEmpty()) {
            QSKIP("Set JLAUNCHER_LIVE_SERVER_JAVA to run the live Forge/NeoForge installer gate.");
        }
        QVERIFY2(QFileInfo(javaPath).isFile(), qPrintable(javaPath));

        QFETCH(QString, provider);
        QFETCH(QString, minecraftVersion);
        QTemporaryDir destination;
        QVERIFY(destination.isValid());

        ServerDownloader downloader;
        QSignalSpy finished(&downloader, &ServerDownloader::finished);
        downloader.startDownload(minecraftVersion, provider, destination.path(),
                                 javaPath);
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 300000);
        QVERIFY2(finished.last().at(0).toBool(),
                 qPrintable(finished.last().at(1).toString()));
        QVERIFY(!downloader.resolvedLoaderVersion().isEmpty());
#ifdef Q_OS_WIN
        QVERIFY(QFileInfo::exists(QDir(destination.path()).filePath("run.bat")));
#else
        QVERIFY(QFileInfo::exists(QDir(destination.path()).filePath("run.sh")));
#endif
    }
};

QTEST_GUILESS_MAIN(ServerLoaderInstallTest)

#include "ServerLoaderInstall_test.moc"
