// SPDX-License-Identifier: GPL-3.0-only

// Server basics: health and crash classification, provider lists, settings, properties and console parsing.

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

class ServerBasicsTest : public QObject {
    Q_OBJECT

   private slots:
    void init()
    {
        ServerInstance::clearJavaProbeCacheForTesting();
    }

    void classifiesServerHealthStatesAndThresholds()
    {
        ServerHealthInput input;
        QCOMPARE(ServerDiagnostics::assessHealth(input).state,
                 ServerHealthState::Inactive);

        input.running = true;
        QCOMPARE(ServerDiagnostics::assessHealth(input).state,
                 ServerHealthState::Unavailable);

        input.processMetricsAvailable = true;
        input.workingSetBytes = qint64(1024) * 1024 * 1024;
        input.configuredRamMiB = 2048;
        input.diskAvailable = true;
        input.diskBytesAvailable = qint64(20) * 1024 * 1024 * 1024;
        ServerHealthAssessment health = ServerDiagnostics::assessHealth(input);
        QCOMPARE(health.state, ServerHealthState::Measuring);
        QCOMPARE(health.cpu, ServerMetricLevel::Unavailable);
        QCOMPARE(health.ram, ServerMetricLevel::Normal);

        input.cpuPercent = 50.0;
        health = ServerDiagnostics::assessHealth(input);
        QCOMPARE(health.state, ServerHealthState::Healthy);
        QCOMPARE(health.cpu, ServerMetricLevel::Normal);
        QCOMPARE(health.disk, ServerMetricLevel::Normal);

        input.cpuPercent = 75.0;
        health = ServerDiagnostics::assessHealth(input);
        QCOMPARE(health.state, ServerHealthState::Healthy);
        QCOMPARE(health.cpu, ServerMetricLevel::Approaching);

        input.cpuPercent = 85.0;
        health = ServerDiagnostics::assessHealth(input);
        QCOMPARE(health.state, ServerHealthState::Warning);
        QCOMPARE(health.cpu, ServerMetricLevel::Warning);

        input.cpuPercent = 50.0;
        input.workingSetBytes = qint64(1900) * 1024 * 1024;
        health = ServerDiagnostics::assessHealth(input);
        QCOMPARE(health.state, ServerHealthState::Warning);
        QCOMPARE(health.ram, ServerMetricLevel::Warning);

        input.workingSetBytes = qint64(1024) * 1024 * 1024;
        input.diskBytesAvailable = qint64(2) * 1024 * 1024 * 1024;
        health = ServerDiagnostics::assessHealth(input);
        QCOMPARE(health.state, ServerHealthState::Warning);
        QCOMPARE(health.disk, ServerMetricLevel::Warning);
    }

    void classifiesKnownCrashCauses()
    {
        QCOMPARE(ServerDiagnostics::classifyCrash(
                     "java.lang.UnsupportedClassVersionError"),
                 ServerCrashCause::JavaVersion);
        QCOMPARE(ServerDiagnostics::classifyCrash("Unable to access jarfile server.jar"),
                 ServerCrashCause::LaunchFiles);
        QCOMPARE(ServerDiagnostics::classifyCrash("FAILED TO BIND TO PORT"),
                 ServerCrashCause::PortConflict);
        QCOMPARE(ServerDiagnostics::classifyCrash("You need to agree to the EULA"),
                 ServerCrashCause::Eula);
        QCOMPARE(ServerDiagnostics::classifyCrash("Mod resolution encountered an error"),
                 ServerCrashCause::Content);
        const QString missingDependencyLog = QStringLiteral(
            "[main/WARN]: Mod resolution failed\n"
            "[main/ERROR]: Incompatible mods found!\n"
            "Mod 'FancyMenu' (fancymenu) 3.3.5 requires version 1.0.6 or later of melody, which is missing!");
        QCOMPARE(ServerDiagnostics::classifyCrash(missingDependencyLog),
                 ServerCrashCause::Content);
        const QString relevantLine =
            ServerDiagnostics::crashRelevantLine(missingDependencyLog);
        QVERIFY(relevantLine.contains("FancyMenu"));
        QVERIFY(relevantLine.contains("melody"));
        QCOMPARE(ServerDiagnostics::classifyCrash("java.lang.OutOfMemoryError"),
                 ServerCrashCause::Memory);
        QCOMPARE(ServerDiagnostics::classifyCrash("Unexpected synthetic failure"),
                 ServerCrashCause::Unknown);
        QVERIFY(ServerDiagnostics::crashCauseExplanation(ServerCrashCause::Memory)
                    .contains("memory", Qt::CaseInsensitive));

        const QString wrongSideLog = QStringLiteral(
            "Caused by: java.lang.RuntimeException: Cannot load class "
            "com.example.ClientConfig in environment type SERVER\n"
            "at example.handler$abc$mr_toad_palladium$configure(example.java:1)");
        QCOMPARE(ServerDiagnostics::classifyCrash(wrongSideLog),
                 ServerCrashCause::Content);
        QCOMPARE(ServerDiagnostics::suspectedModIds(wrongSideLog),
                 QStringList({ "mr_toad_palladium" }));

        const QString forgeCrashReport = QStringLiteral(
            "-- MOD ruokmod --\n"
            "Failure message: RuOK Mod has class loading errors\n"
            "Attempted to load class team/teampotato/ruok/forge/RuOKModForge "
            "for invalid dist DEDICATED_SERVER\n");
        QCOMPARE(ServerDiagnostics::classifyCrash(forgeCrashReport),
                 ServerCrashCause::Content);
        QCOMPARE(ServerDiagnostics::suspectedModIds(forgeCrashReport),
                 QStringList({ "ruokmod" }));
    }

    void supportedServerTypesRemainAvailable()
    {
        QCOMPARE(ServerDownloader::supportedTypes(),
                 QStringList({ "Vanilla", "Paper", "Fabric", "Purpur", "Forge", "NeoForge" }));
    }

    void parsesProviderSpecificVersionResponses_data()
    {
        QTest::addColumn<QString>("provider");
        QTest::addColumn<QByteArray>("response");
        QTest::addColumn<QStringList>("expected");

        QTest::newRow("vanilla")
            << QString("vanilla")
            << QByteArray(R"({"versions":[{"id":"1.21.8","type":"release"},{"id":"25w01a","type":"snapshot"},{"id":"b1.7.3","type":"old_beta"},{"id":"1.20.6","type":"release"}]})")
            << QStringList({ "25w01a", "1.21.8", "1.20.6", "b1.7.3" });
        QTest::newRow("paper")
            << QString("paper")
            << QByteArray(R"({"versions":{"1.21":["1.21.8","1.21.7"],"1.20":["1.20.6"]}})")
            << QStringList({ "1.21.8", "1.21.7", "1.20.6" });
        QTest::newRow("fabric")
            << QString("fabric")
            << QByteArray(R"([{"version":"1.21.8","stable":true},{"version":"25w01a","stable":false},{"version":"1.20.6","stable":true}])")
            << QStringList({ "25w01a", "1.21.8", "1.20.6" });
        QTest::newRow("purpur")
            << QString("purpur")
            << QByteArray(R"({"versions":["1.20.6","1.21.8"]})")
            << QStringList({ "1.21.8", "1.20.6" });
        QTest::newRow("forge")
            << QString("forge")
            << QByteArray(R"({"promos":{"1.20.1-latest":"47.4.0","1.20.1-recommended":"47.3.0","1.21.1-latest":"52.0.1"}})")
            << QStringList({ "1.21.1", "1.20.1" });
        QTest::newRow("forge maven")
            << QString("forge")
            << QByteArray(R"(<?xml version="1.0"?><metadata><versioning><versions><version>1.12.2-14.23.5.2860</version><version>1.20.1-47.4.0</version><version>1.21.1-52.0.1</version><version>1.20.1-47.3.0</version></versions></versioning></metadata>)")
            << QStringList({ "1.21.1", "1.20.1", "1.12.2" });
        QTest::newRow("neoforge")
            << QString("neoforge")
            << QByteArray(R"({"versions":["20.4.100","21.1.50-beta","21.1.51"]})")
            << QStringList({ "1.21.1", "1.20.4", "1.20.1" });
    }

    void parsesProviderSpecificVersionResponses()
    {
        QFETCH(QString, provider);
        QFETCH(QByteArray, response);
        QFETCH(QStringList, expected);
        QString error;
        QCOMPARE(ServerDownloader::parseAvailableVersions(provider, response, &error), expected);
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void rejectsInvalidProviderVersionResponses()
    {
        QString error;
        QVERIFY(ServerDownloader::parseAvailableVersions("paper", "not json", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        error.clear();
        QVERIFY(ServerDownloader::parseAvailableVersions("unknown", "{}", &error).isEmpty());
        QVERIFY(error.contains("Unsupported"));
    }

    void parsesProviderSpecificBuildResponses_data()
    {
        QTest::addColumn<QString>("provider");
        QTest::addColumn<QString>("minecraftVersion");
        QTest::addColumn<QByteArray>("response");
        QTest::addColumn<QStringList>("expected");

        QTest::newRow("paper") << QString("paper") << QString("1.21.8")
            << QByteArray(R"([{"id":130,"channel":"STABLE"},{"id":128,"channel":"STABLE"}])")
            << QStringList({ "130", "128" });
        QTest::newRow("fabric") << QString("fabric") << QString("1.21.8")
            << QByteArray(R"([{"loader":{"version":"0.17.2"}},{"loader":{"version":"0.16.10"}}])")
            << QStringList({ "0.17.2", "0.16.10" });
        QTest::newRow("purpur") << QString("purpur") << QString("1.21.8")
            << QByteArray(R"({"builds":{"latest":"2412","all":[2400,2412,2408]}})")
            << QStringList({ "2412", "2408", "2400" });
        QTest::newRow("forge") << QString("forge") << QString("1.20.1")
            << QByteArray(R"(<?xml version="1.0"?><metadata><versioning><versions><version>1.20.1-47.3.0</version><version>1.21.1-52.0.1</version><version>1.20.1-47.4.0</version><version>1.20.1-47.2.6</version></versions></versioning></metadata>)")
            << QStringList({ "47.4.0", "47.3.0", "47.2.6" });
        QTest::newRow("neoforge") << QString("neoforge") << QString("1.21.1")
            << QByteArray(R"({"versions":["20.4.100","21.1.50-beta","21.1.51"]})")
            << QStringList({ "21.1.51", "21.1.50-beta" });
        QTest::newRow("legacy neoforge") << QString("neoforge") << QString("1.20.1")
            << QByteArray(R"(<?xml version="1.0"?><metadata><versioning><versions><version>1.20.1-47.1.106</version><version>1.20.1-47.1.105</version><version>1.21.1-52.0.1</version></versions></versioning></metadata>)")
            << QStringList({ "47.1.106", "47.1.105" });
    }

    void parsesProviderSpecificBuildResponses()
    {
        QFETCH(QString, provider);
        QFETCH(QString, minecraftVersion);
        QFETCH(QByteArray, response);
        QFETCH(QStringList, expected);
        QString error;
        QCOMPARE(ServerDownloader::parseAvailableBuilds(
                     provider, minecraftVersion, response, &error), expected);
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void classifiesServerVersionChannels_data()
    {
        QTest::addColumn<QString>("version");
        QTest::addColumn<int>("expectedChannel");

        QTest::newRow("release") << QString("1.21.8")
                                  << static_cast<int>(ServerDownloader::VersionChannel::Release);
        QTest::newRow("new release scheme") << QString("26.1")
                                             << static_cast<int>(ServerDownloader::VersionChannel::Release);
        QTest::newRow("weekly snapshot") << QString("25w14a")
                                         << static_cast<int>(ServerDownloader::VersionChannel::Snapshot);
        QTest::newRow("pre-release") << QString("1.21-pre2")
                                     << static_cast<int>(ServerDownloader::VersionChannel::Snapshot);
        QTest::newRow("release candidate") << QString("1.21-rc1")
                                           << static_cast<int>(ServerDownloader::VersionChannel::Snapshot);
        QTest::newRow("provider snapshot") << QString("1.22-snapshot-4")
                                           << static_cast<int>(ServerDownloader::VersionChannel::Snapshot);
        QTest::newRow("modern beta") << QString("1.21.4-beta")
                                     << static_cast<int>(ServerDownloader::VersionChannel::Beta);
        QTest::newRow("legacy beta") << QString("b1.7.3")
                                     << static_cast<int>(ServerDownloader::VersionChannel::Beta);
        QTest::newRow("legacy alpha") << QString("a1.2.6")
                                      << static_cast<int>(ServerDownloader::VersionChannel::Beta);
    }

    void classifiesServerVersionChannels()
    {
        QFETCH(QString, version);
        QFETCH(int, expectedChannel);
        QCOMPARE(static_cast<int>(ServerDownloader::versionChannel(version)), expectedChannel);
    }

    void choosesJavaByLoaderAndMinecraftVersion_data()
    {
        QTest::addColumn<QString>("loader");
        QTest::addColumn<QString>("minecraftVersion");
        QTest::addColumn<int>("javaMajor");

        QTest::newRow("vanilla 1.16.5") << QString("vanilla") << QString("1.16.5") << 8;
        QTest::newRow("paper 1.11") << QString("paper") << QString("1.11") << 8;
        QTest::newRow("paper 1.12.2") << QString("paper") << QString("1.12.2") << 11;
        QTest::newRow("purpur 1.16.4") << QString("purpur") << QString("1.16.4") << 11;
        QTest::newRow("paper 1.16.5") << QString("paper") << QString("1.16.5") << 16;
        QTest::newRow("paper 1.19.4") << QString("paper") << QString("1.19.4") << 17;
        QTest::newRow("paper 1.20.1") << QString("paper") << QString("1.20.1") << 21;
        QTest::newRow("forge 1.20.1") << QString("forge") << QString("1.20.1") << 17;
        QTest::newRow("neoforge 1.20.1") << QString("neoforge") << QString("1.20.1") << 17;
        QTest::newRow("fabric 1.20.6") << QString("fabric") << QString("1.20.6") << 21;
        QTest::newRow("future release") << QString("paper") << QString("26.1") << 25;
    }

    void choosesJavaByLoaderAndMinecraftVersion()
    {
        QFETCH(QString, loader);
        QFETCH(QString, minecraftVersion);
        QFETCH(int, javaMajor);
        QCOMPARE(ServerInstance::recommendedJavaMajor(minecraftVersion, loader), javaMajor);
    }

    void persistsAllServerSettings()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        ServerManager manager(temporaryRoot.path());
        const auto server = manager.createServer("Configured server", "1.21.8", "fabric", "0.16.10");
        QVERIFY(server);
        server->setPort(25570);
        server->setMinMemory(1536);
        server->setMaxMemory(6144);
        server->setJavaPath("C:/test/java.exe");
        server->setExtraJvmArguments("-Dexample=true -XX:+UseG1GC");
        server->setAutoRestartOnCrash(true);
        server->setEulaAccepted(true);
        server->setGracefulStopTimeoutSeconds(1);
        QVERIFY(manager.save());

        ServerManager reloaded(temporaryRoot.path());
        QVERIFY(reloaded.load());
        const auto restored = reloaded.getServer(server->id());
        QVERIFY(restored);
        QCOMPARE(restored->loaderType(), QString("fabric"));
        QCOMPARE(restored->loaderVersion(), QString("0.16.10"));
        QCOMPARE(restored->port(), 25570);
        QCOMPARE(restored->minMemory(), 1536);
        QCOMPARE(restored->maxMemory(), 6144);
        QCOMPARE(restored->javaPath(), QString("C:/test/java.exe"));
        QCOMPARE(restored->extraJvmArguments(), QString("-Dexample=true -XX:+UseG1GC"));
        QVERIFY(restored->autoRestartOnCrash());
        QVERIFY(restored->eulaAccepted());
        QCOMPARE(restored->gracefulStopTimeoutSeconds(), 5);
    }

    void requiresExplicitEulaBeforeStart()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());

        ServerInstance server("eula-test", "EULA test");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        QVERIFY(!server.eulaAccepted());
        QVERIFY(!server.start());
        QCOMPARE(server.status(), ServerStatus::Error);
        QVERIFY(!QFileInfo::exists(QDir(server.serverDirectory()).filePath("eula.txt")));
    }

    void recognizesMinecraftReadinessOutput()
    {
        QVERIFY(ServerInstance::isReadyOutput(
            R"([Server thread/INFO]: Done (1.234s)! For help, type "help")"));
        QVERIFY(ServerInstance::isReadyOutput(R"(For help, type 'help')"));
        QVERIFY(!ServerInstance::isReadyOutput("[Server thread/INFO]: Preparing spawn area: 85%"));
    }

    void roundTripsMinecraftServerPropertiesAtomically()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString path = temporaryRoot.filePath("server/server.properties");
        const QMap<QString, QString> expected{
            { "difficulty", "hard" },
            { "motd", "Phase 5 = safe: ready" },
            { "resource-pack", "https://example.invalid/pack.zip?x=1" },
            { "server-port", "25570" },
        };
        QString error;
        QVERIFY2(ServerProperties::save(path, expected, &error), qPrintable(error));
        QCOMPARE(ServerProperties::load(path, &error), expected);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QByteArray saved = file.readAll();
        file.close();
        QVERIFY(!saved.contains("[General]"));
        QVERIFY(saved.contains("motd=Phase 5 = safe: ready"));

        ServerInstance server("properties", "Properties");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        server.setPort(25571);
        const QMap<QString, QString> updated = ServerProperties::load(path, &error);
        QCOMPARE(updated.value("server-port"), QString("25571"));
        QCOMPARE(updated.value("motd"), expected.value("motd"));

        const QString duplicatePath = temporaryRoot.filePath("duplicates.properties");
        QVERIFY(writeFile(duplicatePath,
                          "# duplicate keys use Minecraft's final value\n"
                          "motd=first\n"
                          "motd=second\n"));
        const QMap<QString, QString> duplicates =
            ServerProperties::load(duplicatePath, &error);
        QCOMPARE(duplicates.value("motd"), QString("second"));
    }

    void preservesAndEscapesMinecraftServerProperties()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString path = temporaryRoot.filePath("server.properties");
        const QByteArray original =
            "# retained comment\n"
            "motd=\\u00A7aHi\n"
            "path=C:\\\\new\n"
            "server-port=25565\n"
            "removed=gone\n";
        QVERIFY(writeFile(path, original));

        QString error;
        QMap<QString, QString> properties = ServerProperties::load(path, &error);
        QCOMPARE(properties.value("motd"), QString::fromUtf8("\xC2\xA7") + "aHi");
        QCOMPARE(properties.value("path"), QStringLiteral("C:\\new"));
        properties["server-port"] = "25570";
        properties.remove("removed");
        properties.insert("z-new", QString::fromUtf8("\xC2\xA7") + "aWelcome");
        QVERIFY2(ServerProperties::save(path, properties, &error), qPrintable(error));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(saved.startsWith("# retained comment\nmotd=\\u00A7aHi\npath=C:\\\\new\n"));
        QVERIFY(saved.contains("server-port=25570\n"));
        QVERIFY(!saved.contains("removed="));
        QVERIFY(saved.endsWith("z-new=\\u00A7aWelcome\n"));
        const QMap<QString, QString> loaded = ServerProperties::load(path, &error);
        QCOMPARE(loaded.value("motd"), properties.value("motd"));
        QCOMPARE(loaded.value("path"), properties.value("path"));
        QCOMPARE(loaded.value("z-new"), properties.value("z-new"));

        const QString escapedValuePath = temporaryRoot.filePath("escaped-value.properties");
        QVERIFY2(ServerProperties::save(escapedValuePath,
                                        { { "motd", QString::fromUtf8("\xC2\xA7") + "x" },
                                          { "other", "first\nsecond" } },
                                        &error),
                 qPrintable(error));
        QFile escapedValueFile(escapedValuePath);
        QVERIFY(escapedValueFile.open(QIODevice::ReadOnly));
        const QByteArray escapedValueText = escapedValueFile.readAll();
        QVERIFY(escapedValueText.contains("motd=\\u00A7x"));
        QVERIFY(escapedValueText.contains("other=first\\nsecond"));
        QCOMPARE(ServerProperties::load(escapedValuePath, &error).value("other"),
                 QString("first\nsecond"));

        const QString backslashPath = temporaryRoot.filePath("backslash.properties");
        QVERIFY(writeFile(backslashPath, "path=C:\\\\new\n"));
        QCOMPARE(ServerProperties::load(backslashPath, &error).value("path"),
                 QStringLiteral("C:\\new"));

        const QString continuationPath = temporaryRoot.filePath("continuation.properties");
        QVERIFY(writeFile(continuationPath, "motd=joined\\\n  value\n"));
        QCOMPARE(ServerProperties::load(continuationPath, &error).value("motd"),
                 QStringLiteral("joinedvalue"));
    }

    void takesCompleteConsoleLines()
    {
        QByteArray buffer("readiness prefix ");
        QVERIFY(ServerInstance::takeCompleteLines(buffer).isEmpty());
        buffer.append("readiness suffix\r\npartial tail");
        const QStringList lines = ServerInstance::takeCompleteLines(buffer);
        QCOMPARE(lines, QStringList{ "readiness prefix readiness suffix" });
        QCOMPARE(buffer, QByteArray("partial tail"));
    }

    void parsesOnlyLoggerPlayerActivityLines()
    {
        const QList<QPair<QString, QPair<QString, bool>>> matchingLines{
            { "[12:00:00] [Server thread/INFO]: Steve joined the game", { "Steve", true } },
            { "[12:00:00] [Server thread/INFO] [minecraft/MinecraftServer]: Steve left the game", { "Steve", false } },
            { "[12:00:00 INFO]: Steve joined the game", { "Steve", true } },
        };
        for (const auto &entry : matchingLines) {
            QString player;
            bool joined = false;
            QVERIFY(ServerInstance::parsePlayerActivity(entry.first, &player, &joined));
            QCOMPARE(player, entry.second.first);
            QCOMPARE(joined, entry.second.second);
        }
        const QStringList rejectedLines{
            "[12:00:00] [Server thread/INFO]: <Steve> Notch joined the game",
            "[12:00:00] [Server thread/INFO]: [Server] Notch joined the game",
            "[12:00:00] [Server thread/INFO]: Steve joined the game and left",
        };
        for (const QString &line : rejectedLines) {
            QVERIFY(!ServerInstance::parsePlayerActivity(line, nullptr, nullptr));
        }
    }

    void rejectsUnsafeServerProperties()
    {
        QString error;
        QVERIFY(!ServerProperties::validate({ { "bad=key", "value" } }, &error));
        QVERIFY(error.contains("option name"));
        error.clear();
        QVERIFY(ServerProperties::validate({ { "motd", "first\nsecond" } }, &error));
        error.clear();
        QVERIFY(!ServerProperties::validate({ { "server-port", "70000" } }, &error));
        QVERIFY(error.contains("between 1 and 65535"));
    }

    void mapsServerTypesToCompatibleContent_data()
    {
        QTest::addColumn<QString>("loader");
        QTest::addColumn<int>("contentType");

        QTest::newRow("vanilla") << QString("vanilla")
                                  << static_cast<int>(ServerContentType::None);
        QTest::newRow("fabric") << QString("fabric")
                                 << static_cast<int>(ServerContentType::Mod);
        QTest::newRow("forge") << QString("forge")
                                << static_cast<int>(ServerContentType::Mod);
        QTest::newRow("neoforge") << QString("neoforge")
                                   << static_cast<int>(ServerContentType::Mod);
        QTest::newRow("paper") << QString("paper")
                                << static_cast<int>(ServerContentType::Plugin);
        QTest::newRow("purpur") << QString("purpur")
                                 << static_cast<int>(ServerContentType::Plugin);
        QTest::newRow("unknown") << QString("unknown")
                                  << static_cast<int>(ServerContentType::None);
    }

    void mapsServerTypesToCompatibleContent()
    {
        QFETCH(QString, loader);
        QFETCH(int, contentType);
        QCOMPARE(static_cast<int>(ServerInstance::contentTypeForLoader(loader)), contentType);
    }
};

QTEST_GUILESS_MAIN(ServerBasicsTest)

#include "ServerBasics_test.moc"
