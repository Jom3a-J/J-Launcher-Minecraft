// SPDX-License-Identifier: GPL-3.0-only

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

namespace {
bool writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QUrl writeVanillaFileFixture(const QString& rootPath, const QString& version,
                             const QByteArray& serverJar)
{
    const QDir root(rootPath);
    const QString jarPath = root.filePath(QStringLiteral("vanilla/%1/server.jar").arg(version));
    const QString versionPath = root.filePath(QStringLiteral("vanilla/%1/version.json").arg(version));
    const QString manifestPath = root.filePath(QStringLiteral("vanilla/manifest.json"));
    if (!writeFile(jarPath, serverJar)
        || !writeFile(versionPath, QJsonDocument(QJsonObject{
            { "downloads", QJsonObject{ { "server", QJsonObject{
                { "url", QUrl::fromLocalFile(jarPath).toString() },
                { "sha1", QString::fromLatin1(
                    QCryptographicHash::hash(serverJar, QCryptographicHash::Sha1).toHex()) },
            } } } },
        }).toJson(QJsonDocument::Compact))
        || !writeFile(manifestPath, QJsonDocument(QJsonObject{
            { "versions", QJsonArray{ QJsonObject{
                { "id", version },
                { "url", QUrl::fromLocalFile(versionPath).toString() },
            } } },
        }).toJson(QJsonDocument::Compact))) {
        return {};
    }
    return QUrl::fromLocalFile(manifestPath);
}

bool writeArchive(const QString& path, const QList<QPair<QString, QByteArray>>& entries)
{
    MMCZip::ArchiveWriter archive(path);
    if (!archive.open()) {
        return false;
    }
    for (const auto& entry : entries) {
        if (!archive.addFile(entry.first, entry.second)) {
            return false;
        }
    }
    return archive.close();
}

class ScopedEnvironmentVariable
{
  public:
    ScopedEnvironmentVariable(const char* name, const QByteArray& value)
        : m_name(name)
        , m_wasSet(qEnvironmentVariableIsSet(name))
        , m_previousValue(qgetenv(name))
    {
        qputenv(m_name.constData(), value);
    }

    ~ScopedEnvironmentVariable()
    {
        if (m_wasSet) {
            qputenv(m_name.constData(), m_previousValue);
        } else {
            qunsetenv(m_name.constData());
        }
    }

  private:
    QByteArray m_name;
    bool m_wasSet = false;
    QByteArray m_previousValue;
};

QString fakeMinecraftServerPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        "FakeMinecraftServer.exe"
#else
        "FakeMinecraftServer"
#endif
    );
}

quint16 unusedPort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    return probe.serverPort();
}

bool prepareSyntheticServer(ServerInstance& server, const QString& directory,
                            const QString& extraArguments = QString())
{
    QDir().mkpath(directory);
    server.setServerDirectory(directory);
    server.setVersion("1.21.8");
    server.setLoaderType("vanilla");
    server.setPort(unusedPort());
    server.setEulaAccepted(true);
    server.setJavaPath(fakeMinecraftServerPath());
    server.setExtraJvmArguments(extraArguments);
    return server.port() != 0
        && QFileInfo::exists(server.javaPath())
        && writeArchive(server.serverJarPath(), {
            { "META-INF/MANIFEST.MF", "Main-Class: net.minecraft.bundler.Main\n" },
        });
}

QUrl directoryUrl(const QString& path)
{
    return QUrl::fromLocalFile(
        QDir::fromNativeSeparators(QDir(path).absolutePath()) + '/');
}

RangeHttpServer::Resource httpResource(QByteArray body)
{
    RangeHttpServer::Resource resource;
    resource.body = std::move(body);
    resource.etag = QByteArrayLiteral("\"server-v1\"");
    return resource;
}

void addVanillaHttpFixture(RangeHttpServer& server, const QByteArray& jar,
                           const QString& expectedSha1, qint64 stallAfter = -1)
{
    const QString versionJsonPath = QStringLiteral("/version.json");
    const QString jarPath = QStringLiteral("/server.jar");
    server.serve("/manifest.json", httpResource(QJsonDocument(QJsonObject{
        { "versions", QJsonArray{ QJsonObject{
            { "id", "1.21.8" },
            { "type", "release" },
            { "url", server.url(versionJsonPath.toUtf8()).toString() },
        } } },
    }).toJson(QJsonDocument::Compact)));
    server.serve(versionJsonPath.toUtf8(), httpResource(QJsonDocument(QJsonObject{
        { "downloads", QJsonObject{ { "server", QJsonObject{
            { "url", server.url(jarPath.toUtf8()).toString() },
            { "sha1", expectedSha1 },
        } } } },
    }).toJson(QJsonDocument::Compact)));
    auto jarResource = httpResource(jar);
    jarResource.stallAfter = stallAfter;
    server.serve(jarPath.toUtf8(), std::move(jarResource));
}

ServerProviderEndpoints vanillaHttpEndpoints(const RangeHttpServer& server)
{
    auto endpoints = ServerProviderEndpoints::production();
    endpoints.vanillaManifest = server.url("/manifest.json");
    return endpoints;
}

class FixtureHttpServer
{
  public:
    FixtureHttpServer()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                auto request = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket, request]() {
                    request->append(socket->readAll());
                    if (!request->contains("\r\n\r\n")) {
                        return;
                    }
                    const QByteArray requestTarget =
                        request->split('\n').first().split(' ').value(1);
                    const bool found = m_routes.contains(requestTarget);
                    const QByteArray body = found ? m_routes.value(requestTarget)
                                                  : QByteArray("not found");
                    socket->write("HTTP/1.1 " +
                                  QByteArray(found ? "200 OK\r\n" : "404 Not Found\r\n"));
                    socket->write("Content-Type: application/octet-stream\r\n");
                    socket->write("Content-Length: " +
                                  QByteArray::number(body.size()) + "\r\n");
                    socket->write("Connection: close\r\n\r\n");
                    socket->write(body);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    bool start()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    void addRoute(const QByteArray& path, const QByteArray& response)
    {
        m_routes.insert(path, response);
    }

    QUrl baseUrl(const QString& prefix) const
    {
        return QUrl(QString("http://127.0.0.1:%1/%2/")
                        .arg(m_server.serverPort())
                        .arg(prefix));
    }

    QUrl url(const QString& path) const
    {
        const QString normalizedPath = path.startsWith('/') ? path : '/' + path;
        return QUrl(QString("http://127.0.0.1:%1%2")
                        .arg(m_server.serverPort())
                        .arg(normalizedPath));
    }

  private:
    QTcpServer m_server;
    QHash<QByteArray, QByteArray> m_routes;
};
}  // namespace

class ServerInstanceTest : public QObject {
    Q_OBJECT

   private slots:
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

    void rejectsUnsafeServerProperties()
    {
        QString error;
        QVERIFY(!ServerProperties::validate({ { "bad=key", "value" } }, &error));
        QVERIFY(error.contains("option name"));
        error.clear();
        QVERIFY(!ServerProperties::validate({ { "motd", "first\nsecond" } }, &error));
        QVERIFY(error.contains("line break"));
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

    void rejectsAnOccupiedPortBeforeStartingJava()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        QTcpServer occupiedPort;
        QVERIFY(occupiedPort.listen(QHostAddress::LocalHost, 0));

        ServerInstance server("occupied-port", "Occupied port");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        server.setEulaAccepted(true);
        server.setPort(occupiedPort.serverPort());
        server.setJavaPath(temporaryRoot.filePath("java-that-must-not-run.exe"));
        QSignalSpy errors(&server, &ServerInstance::serverError);

        QVERIFY(!server.start());
        QCOMPARE(server.status(), ServerStatus::Error);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains(QString::number(occupiedPort.serverPort())));
        QVERIFY(server.consoleLog().contains("already in use"));
    }

    void runsCommandsRestartsAndStopsSyntheticServer()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("lifecycle", "Lifecycle");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server")));
        QSignalSpy started(&server, &ServerInstance::started);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QCOMPARE(started.size(), 1);

        server.writeStdin("say phase-four");
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:say phase-four"), 5000);

        QVERIFY(server.restart());
        QTRY_VERIFY_WITH_TIMEOUT(started.size() >= 2, 5000);
        QCOMPARE(server.status(), ServerStatus::Running);

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void sendsValidatedPlayerAdministrationCommandsWhileRunning()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("live-player-admin", "Live player admin");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server")));

        QString error;
        QVERIFY(!server.setPlayerWhitelistedLive("TestPlayer", true, &error));
        QVERIFY(error.contains("running"));
        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);

        error.clear();
        QVERIFY2(server.setPlayerWhitelistedLive("TestPlayer", true, &error), qPrintable(error));
        QVERIFY2(server.setPlayerOperatorLive("TestPlayer", true, &error), qPrintable(error));
        QVERIFY2(server.setPlayerBannedLive("TestPlayer", true, "Testing ban", &error), qPrintable(error));
        QVERIFY2(server.clearPlayerAccessLive("TestPlayer", &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:whitelist add TestPlayer"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:op TestPlayer"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:ban TestPlayer Testing ban"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:whitelist remove TestPlayer"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:deop TestPlayer"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:pardon TestPlayer"), 5000);

        error.clear();
        QVERIFY(!server.setPlayerOperatorLive("bad player", true, &error));
        QVERIFY(error.contains("invalid"));
        QVERIFY(!server.consoleLog().contains("COMMAND:op bad player"));

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void appliesConfiguredJavaMemoryAndArgumentsToGeneratedNeoForgeLaunch()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString serverDirectory = temporaryRoot.filePath("server");
#ifdef Q_OS_WIN
        const QString scriptName = QStringLiteral("run.bat");
        const QString loaderArgumentsRelative = QStringLiteral(
            "libraries/net/neoforged/neoforge/21.1.233/win_args.txt");
        const QByteArray launchScript =
            "@echo off\r\njava @user_jvm_args.txt "
            "@libraries/net/neoforged/neoforge/21.1.233/win_args.txt %*\r\n";
#else
        const QString scriptName = QStringLiteral("run.sh");
        const QString loaderArgumentsRelative = QStringLiteral(
            "libraries/net/neoforged/neoforge/21.1.233/unix_args.txt");
        const QByteArray launchScript =
            "#!/bin/sh\njava @user_jvm_args.txt "
            "@libraries/net/neoforged/neoforge/21.1.233/unix_args.txt \"$@\"\n";
#endif
        const QString loaderArguments =
            QDir(serverDirectory).filePath(loaderArgumentsRelative);

        ServerInstance server("neoforge-launch", "NeoForge launch");
        server.setServerDirectory(serverDirectory);
        server.setVersion("1.21.1");
        server.setLoaderType("neoforge");
        server.setLoaderVersion("21.1.233");
        server.setPort(unusedPort());
        server.setEulaAccepted(true);
        server.setJavaPath(fakeMinecraftServerPath());
        server.setMinMemory(1536);
        server.setMaxMemory(6144);
        server.setExtraJvmArguments("-Dexample=true -XX:+UseG1GC");

        QVERIFY(writeFile(QDir(serverDirectory).filePath(scriptName), launchScript));
        QVERIFY(writeFile(QDir(serverDirectory).filePath("user_jvm_args.txt"), "# pack options\n"));
        QVERIFY(writeFile(loaderArguments, "# synthetic loader arguments\n"));
        QVERIFY(writeArchive(QDir(serverDirectory).filePath("server.jar"), {
            { "META-INF/MANIFEST.MF", "Main-Class: net.minecraft.bundler.Main\n" },
        }));

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        // The supplied file is preserved and never passed directly; the
        // launcher-owned effective file carries the filtered options.
        QTRY_VERIFY_WITH_TIMEOUT(
            server.consoleLog().contains("ARG:@" + ServerJvmArgs::effectiveFileName()), 5000);
        QVERIFY(!server.consoleLog().contains("ARG:@user_jvm_args.txt"));
        QVERIFY(server.consoleLog().contains("ARG:-Xmx6144M"));
        QVERIFY(server.consoleLog().contains("ARG:-Xms1536M"));
        QVERIFY(server.consoleLog().contains("ARG:-Dexample=true"));
        QVERIFY(server.consoleLog().contains("ARG:-XX:+UseG1GC"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.language=en"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.country=US"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.language.format=en"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.country.format=US"));
        QVERIFY(server.consoleLog().contains("ARG:@" + loaderArgumentsRelative));
        QVERIFY(server.consoleLog().contains("[JVM] Using dedicated-server locale en_US"));
        QCOMPARE(readFile(QDir(serverDirectory).filePath("user_jvm_args.txt")),
                 QByteArray("# pack options\n"));
        QVERIFY(QFileInfo::exists(
            QDir(serverDirectory).filePath(ServerJvmArgs::effectiveFileName())));

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void preservesCustomForgeScriptAndInjectsConfiguredJvmSettings_data()
    {
        QTest::addColumn<bool>("providerStartScript");
        QTest::newRow("generated run script") << false;
        QTest::newRow("ServerPackCreator start script") << true;
    }

    void preservesCustomForgeScriptAndInjectsConfiguredJvmSettings()
    {
        QFETCH(bool, providerStartScript);
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString serverDirectory = temporaryRoot.filePath("server");
        const QString fakeServer = QDir::toNativeSeparators(fakeMinecraftServerPath());

        ServerInstance server("custom-forge-launch", "Custom Forge launch");
        server.setServerDirectory(serverDirectory);
        server.setVersion("1.21.1");
        server.setLoaderType("forge");
        server.setPort(unusedPort());
        server.setEulaAccepted(true);
        server.setJavaPath(fakeMinecraftServerPath());
        server.setMinMemory(2048);
        server.setMaxMemory(4096);
        server.setExtraJvmArguments("-Dcustom=true");

        const QByteArray script = QString(
#ifdef Q_OS_WIN
            "@echo off\r\necho JVM_OPTIONS:%JAVA_TOOL_OPTIONS%\r\n\"%1\" %*\r\n")
#else
            "#!/bin/sh\nprintf 'JVM_OPTIONS:%s\\n' \"$JAVA_TOOL_OPTIONS\"\n\"%1\" \"$@\"\n")
#endif
                                      .arg(fakeServer)
                                      .toUtf8();
#ifdef Q_OS_WIN
        const QString scriptName = providerStartScript
            ? QStringLiteral("start.bat") : QStringLiteral("run.bat");
#else
        const QString scriptName = providerStartScript
            ? QStringLiteral("start.sh") : QStringLiteral("run.sh");
#endif
        QVERIFY(writeFile(QDir(serverDirectory).filePath(scriptName), script));

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("JVM_OPTIONS:"), 5000);
        QVERIFY(server.consoleLog().contains("-Xmx4096M"));
        QVERIFY(server.consoleLog().contains("-Xms2048M"));
        QVERIFY(server.consoleLog().contains("-Dcustom=true"));
        // Opaque wrappers cannot be filtered safely, so locale plus the
        // narrow IgnoreUnrecognizedVMOptions fallback travel via the
        // environment. The wrapper script itself must remain untouched.
        QVERIFY(server.consoleLog().contains("-Duser.language=en"));
        QVERIFY(server.consoleLog().contains("-Duser.country=US"));
        QVERIFY(server.consoleLog().contains("-Duser.language.format=en"));
        QVERIFY(server.consoleLog().contains("-XX:+IgnoreUnrecognizedVMOptions"));
        QVERIFY(server.consoleLog().contains("[JVM] Using dedicated-server locale en_US"));
        QVERIFY(server.consoleLog().contains("opaque wrapper"));
        QCOMPARE(readFile(QDir(serverDirectory).filePath(scriptName)), script);

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void prefersGeneratedRunScriptWhenBothWrapperNamesExist()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString serverDirectory = temporaryRoot.filePath("server");
        const QString fakeServer = QDir::toNativeSeparators(fakeMinecraftServerPath());

        ServerInstance server("preferred-forge-wrapper", "Preferred Forge wrapper");
        server.setServerDirectory(serverDirectory);
        server.setVersion("1.21.1");
        server.setLoaderType("forge");
        server.setPort(unusedPort());
        server.setEulaAccepted(true);
        server.setJavaPath(fakeMinecraftServerPath());

#ifdef Q_OS_WIN
        const QString runName = QStringLiteral("run.bat");
        const QString startName = QStringLiteral("start.bat");
        const QByteArray runScript = QString(
            "@echo off\r\necho SELECTED:run\r\n\"%1\" %*\r\n").arg(fakeServer).toUtf8();
        const QByteArray startScript = QString(
            "@echo off\r\necho SELECTED:start\r\n\"%1\" %*\r\n").arg(fakeServer).toUtf8();
#else
        const QString runName = QStringLiteral("run.sh");
        const QString startName = QStringLiteral("start.sh");
        const QByteArray runScript = QString(
            "#!/bin/sh\nprintf 'SELECTED:run\\n'\n\"%1\" \"$@\"\n").arg(fakeServer).toUtf8();
        const QByteArray startScript = QString(
            "#!/bin/sh\nprintf 'SELECTED:start\\n'\n\"%1\" \"$@\"\n").arg(fakeServer).toUtf8();
#endif
        QVERIFY(writeFile(QDir(serverDirectory).filePath(runName), runScript));
        QVERIFY(writeFile(QDir(serverDirectory).filePath(startName), startScript));

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("SELECTED:run"), 5000);
        QVERIFY(!server.consoleLog().contains("SELECTED:start"));
        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void filtersJava21OnlyOptionsByDetectedRuntime()
    {
        const QString content = QStringLiteral(
            "# Requiem-style supplied options\n"
            "-Xms9G\n"
            "-XX:+ZGenerational\n"
            "-XX:+UseG1GC\n"
            "-Dfile.encoding=UTF-8\n");
        const ServerJvmFilterResult java17 = ServerJvmArgs::prepareContent(content, 17);
        QVERIFY(java17.ok);
        QVERIFY(java17.keptTokens.contains("-XX:+UseG1GC"));
        QVERIFY(java17.keptTokens.contains("-Dfile.encoding=UTF-8"));
        QVERIFY(!java17.keptTokens.contains("-Xms9G"));
        QVERIFY(!java17.keptTokens.contains("-XX:+ZGenerational"));
        QVERIFY(java17.removedMemoryOptions.contains("-Xms9G"));
        QVERIFY(java17.removedUnsupportedOptions.contains("-XX:+ZGenerational"));

        const ServerJvmFilterResult java21 = ServerJvmArgs::prepareContent(content, 21);
        QVERIFY(java21.ok);
        QVERIFY(java21.keptTokens.contains("-XX:+ZGenerational"));
        QVERIFY(java21.keptTokens.contains("-XX:+UseG1GC"));
        QVERIFY(java21.removedMemoryOptions.contains("-Xms9G"));
        QVERIFY(java21.removedUnsupportedOptions.isEmpty());
    }

    void removesMemoryOptionsInAllAcceptedSpellings()
    {
        const QStringList tokens{
            QStringLiteral("-Xms9G"),           QStringLiteral("-Xmx4G"),
            QStringLiteral("-Xms512M"),         QStringLiteral("-Xmx2048m"),
            QStringLiteral("-Xms1024k"),        QStringLiteral("-Xmx1G"),
            QStringLiteral("-Xms"),             QStringLiteral("2G"),
            QStringLiteral("-Xmx"),             QStringLiteral("4G"),
            QStringLiteral("-XX:InitialHeapSize=1G"), QStringLiteral("-XX:MaxHeapSize=2G"),
            QStringLiteral("-XX:+UseG1GC"),     QStringLiteral("-Dexample=true"),
        };
        for (int javaMajor : { 17, 21 }) {
            const ServerJvmFilterResult filtered = ServerJvmArgs::filterTokens(tokens, javaMajor);
            QVERIFY(filtered.ok);
            QCOMPARE(filtered.keptTokens,
                     QStringList({ "-XX:+UseG1GC", "-Dexample=true" }));
            QVERIFY(!filtered.keptTokens.join(' ').contains("-Xms"));
            QVERIFY(!filtered.keptTokens.join(' ').contains("-Xmx"));
            QVERIFY(!filtered.keptTokens.join(' ').contains("HeapSize"));
            QVERIFY(filtered.removedMemoryOptions.contains("-Xms9G"));
            QVERIFY(filtered.removedMemoryOptions.contains("-Xmx4G"));
            // Bare flags with a valid size are recorded as one combined
            // expression, not as two separate removed options.
            QVERIFY(filtered.removedMemoryOptions.contains("-Xms 2G"));
            QVERIFY(filtered.removedMemoryOptions.contains("-Xmx 4G"));
            QVERIFY(!filtered.removedMemoryOptions.contains("2G"));
            QVERIFY(!filtered.removedMemoryOptions.contains("4G"));
            QVERIFY(filtered.removedMemoryOptions.contains("-XX:InitialHeapSize=1G"));
            QVERIFY(filtered.removedMemoryOptions.contains("-XX:MaxHeapSize=2G"));
        }
    }

    void bareMemoryFlagsPreserveFollowingJvmOptions()
    {
        QVERIFY(ServerJvmArgs::isHeapSizeValue("2G"));
        QVERIFY(ServerJvmArgs::isHeapSizeValue("2048M"));
        QVERIFY(ServerJvmArgs::isHeapSizeValue("512k"));
        QVERIFY(ServerJvmArgs::isHeapSizeValue("1024"));
        QVERIFY(!ServerJvmArgs::isHeapSizeValue("-XX:+UseG1GC"));
        QVERIFY(!ServerJvmArgs::isHeapSizeValue("-Dfoo=bar"));
        QVERIFY(!ServerJvmArgs::isHeapSizeValue(""));

        // "-Xms" followed by another option must not swallow that option.
        const ServerJvmFilterResult followedByOption = ServerJvmArgs::filterTokens(
            { "-Xms", "-XX:+UseG1GC", "-Dfoo=bar" }, 17);
        QVERIFY(followedByOption.ok);
        QVERIFY(followedByOption.removedMemoryOptions.contains("-Xms"));
        QVERIFY(!followedByOption.removedMemoryOptions.join('|').contains("UseG1GC"));
        QVERIFY(followedByOption.keptTokens.contains("-XX:+UseG1GC"));
        QVERIFY(followedByOption.keptTokens.contains("-Dfoo=bar"));

        const ServerJvmFilterResult trailingBare = ServerJvmArgs::filterTokens({ "-Xmx" }, 17);
        QVERIFY(trailingBare.ok);
        QVERIFY(trailingBare.keptTokens.isEmpty());
        QCOMPARE(trailingBare.removedMemoryOptions, QStringList({ "-Xmx" }));

        const ServerJvmFilterResult validPair = ServerJvmArgs::filterTokens(
            { "-Xms", "2048M", "-Dkeep=true" }, 17);
        QVERIFY(validPair.ok);
        QCOMPARE(validPair.removedMemoryOptions, QStringList({ "-Xms 2048M" }));
        QCOMPARE(validPair.keptTokens, QStringList({ "-Dkeep=true" }));
    }

    void preservesWindowsPathBackslashesExactly()
    {
        QString error;
        QStringList tokens;
        // Unquoted Windows separators stay literal (no escape consumption).
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "-Dpath=C:\\mods -Dother=X", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dpath=C:\\mods", "-Dother=X" }));

        // Java requires escaped backslashes inside quotes.
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "\"-Dpath=C:\\\\Program Files\\\\Server\"", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dpath=C:\\Program Files\\Server" }));

        // Doubled backslashes inside double quotes collapse to one (the only
        // way to represent them); outside quotes they stay literal.
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "\"-Dpath=C:\\\\mods\"", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dpath=C:\\mods" }));

        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "-Dpath=C:\\\\share", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dpath=C:\\\\share" }));

        // Embedded quotes via supported \" escape.
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "\"-Dmsg=a\\\"b\"", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dmsg=a\"b" }));

        // Backslash before ordinary characters (\m, \n in C:\new) is preserved.
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "-Dpath=C:\\new\\temp", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dpath=C:\\new\\temp" }));

        // Unquoted backslashes do not join lines in Java argument files.
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "-Dfoo=bar\\\nBaz -Dkeep=true", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dfoo=bar\\", "Baz", "-Dkeep=true" }));
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "-Dfoo=bar\\\r\nBaz", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dfoo=bar\\", "Baz" }));

        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "\"-Dfoo=bar\\\r\n  \tBaz\"", &tokens, &error), qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dfoo=barBaz" }));
    }

    void roundTripsWindowsPathsAndQuotesThroughEffectiveFile()
    {
        const QStringList original{
            QStringLiteral("-Dpath=C:\\mods"),
            QStringLiteral("-Dpath=C:\\Program Files\\Server"),
            QStringLiteral("-Dpath=C:\\\\share"),
            QStringLiteral("-Dmsg=a\"b"),
            QStringLiteral("-XX:+UseG1GC"),
        };
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString effective = temporaryRoot.filePath("effective.txt");
        QString error;
        QVERIFY2(ServerJvmArgs::writeEffectiveArgfile(effective, original, &error),
                 qPrintable(error));
        QStringList reparsed;
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     QString::fromUtf8(readFile(effective)), &reparsed, &error),
                 qPrintable(error));
        QCOMPARE(reparsed, original);
    }

    void preservesCompatibleOptionsCommentsQuotesAndSpacing()
    {
        const QString content = QStringLiteral(
            "# pack header\n"
            "\n"
            "-XX:+UseG1GC\n"
            "   # inline comment\n"
            "-Dfile.encoding=UTF-8\n"
            "\"-Dquoted=value with spaces\"\n"
            "'-Dsingle=value with spaces'\n"
            "\"-Dhash=bar#baz\"\n");
        const ServerJvmFilterResult filtered = ServerJvmArgs::prepareContent(content, 17);
        QVERIFY2(filtered.ok, qPrintable(filtered.errorMessage));
        QVERIFY(filtered.keptTokens.contains("-XX:+UseG1GC"));
        QVERIFY(filtered.keptTokens.contains("-Dfile.encoding=UTF-8"));
        QVERIFY(filtered.keptTokens.contains("-Dquoted=value with spaces"));
        QVERIFY(filtered.keptTokens.contains("-Dsingle=value with spaces"));
        QVERIFY(filtered.keptTokens.contains("-Dhash=bar#baz"));
        QVERIFY(filtered.removedMemoryOptions.isEmpty());
        QVERIFY(filtered.removedUnsupportedOptions.isEmpty());

        // Quoted tokens with spaces must round-trip through the effective
        // file without being split or executed.
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString effective = temporaryRoot.filePath("effective.txt");
        QString error;
        QVERIFY2(ServerJvmArgs::writeEffectiveArgfile(effective, filtered.keptTokens, &error),
                 qPrintable(error));
        const QByteArray written = readFile(effective);
        QVERIFY(written.contains("\"-Dquoted=value with spaces\""));
        QStringList reparsed;
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(QString::fromUtf8(written), &reparsed, &error),
                 qPrintable(error));
        // Header comment is skipped; quoted values survive intact.
        QVERIFY(reparsed.contains("-Dquoted=value with spaces"));
        QVERIFY(reparsed.contains("-Dsingle=value with spaces"));
    }

    void failsSafelyOnMalformedOrUnreadableArgfiles()
    {
        QString error;
        QStringList tokens;
        QVERIFY(!ServerJvmArgs::tokenizeArgfile("-XX:+UseG1GC \"unclosed\n", &tokens, &error));
        QVERIFY(error.contains("Unclosed quote"));

        const ServerJvmFilterResult unclosed =
            ServerJvmArgs::prepareContent("-Dfoo=\"unclosed\n", 17);
        QVERIFY(!unclosed.ok);
        QVERIFY(unclosed.errorMessage.contains("Unclosed quote"));

        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const ServerJvmFilterResult missing =
            ServerJvmArgs::prepareFile(temporaryRoot.filePath("missing/user_jvm_args.txt"), 17);
        QVERIFY(!missing.ok);
        QVERIFY(missing.errorMessage.contains("Could not read"));

        // NUL (including NUL.txt) is a reserved Windows device name.
        const QString nulPath = temporaryRoot.filePath("embedded-nul-args.txt");
        QVERIFY(writeFile(nulPath, QByteArray("ok\0bad", 6)));
        const ServerJvmFilterResult nul = ServerJvmArgs::prepareFile(nulPath, 17);
        QVERIFY(!nul.ok);
        QVERIFY(nul.errorMessage.contains("NUL"));
    }

    void preservesArgumentsInRealJava()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString java = qEnvironmentVariable(
            "JLAUNCHER_TEST_JAVA", QStringLiteral(JLAUNCHER_TEST_JAVA_PATH));
        QVERIFY2(QFileInfo(java).isFile(), qPrintable(java));
        const QString probe = root.filePath("ArgumentProbe.java");
        QVERIFY(writeFile(probe, R"(import java.nio.charset.StandardCharsets;
import java.util.Base64;
class ArgumentProbe {
    public static void main(String[] keys) {
        for (String key : keys) {
            String value = String.valueOf(System.getProperty(key));
            System.out.println(Base64.getEncoder().encodeToString(value.getBytes(StandardCharsets.UTF_8)));
        }
    }
}
)"));
        QString diagnostics;
        auto run = [&](const QStringList &options, const QStringList &keys,
                       const QString &environmentOptions, QByteArray *output) {
            QProcess process;
            auto environment = QProcessEnvironment::systemEnvironment();
            for (const QString &key : { QStringLiteral("JAVA_TOOL_OPTIONS"),
                                       QStringLiteral("_JAVA_OPTIONS"), QStringLiteral("JDK_JAVA_OPTIONS") }) {
                environment.remove(key);
            }
            if (!environmentOptions.isEmpty()) {
                environment.insert("JAVA_TOOL_OPTIONS", environmentOptions);
            }
            process.setProcessEnvironment(environment);
            process.start(java, options + QStringList{ probe } + keys);
            const bool finished = process.waitForFinished(30000);
            if (!finished) {
                process.kill();
                process.waitForFinished();
            }
            diagnostics = process.errorString() + '\n' + QString::fromUtf8(process.readAllStandardError());
            *output = process.readAllStandardOutput().replace("\r\n", "\n");
            return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        };
        auto encodedValues = [](const QStringList &values) {
            QByteArray output;
            for (const QString &value : values) {
                output += value.toUtf8().toBase64() + '\n';
            }
            return output;
        };

        const QString original = root.filePath("provider-args.txt");
        const QString effective = root.filePath("effective-args.txt");
        QByteArray provider =
            "-Dprobe.path=C:\\mods\n"
            "\"-Dprobe.spaces=C:\\\\Program Files\\\\Server\"\n"
            "'-Dprobe.single=C:\\\\mods'\n"
            "\"-Dprobe.control=a\\nb\\rc\\td\\fe\"\n"
            "\"-Dprobe.join=first\\\r\n  \tsecond\"\n"
            "-Dprobe.dropped=ignored#comment\n"
            "\"-Dprobe.hash=kept#hash\"\n"
            "# CR-only comment\r-Dprobe.after=present\n";
        provider += QStringLiteral("-Dprobe.native=a\u00a0b\n").toLocal8Bit();
        const QStringList keys{ "probe.path", "probe.spaces", "probe.single", "probe.control",
                                "probe.join", "probe.dropped", "probe.hash", "probe.after", "probe.native" };
        const QByteArray expected = encodedValues({ "C:\\mods", "C:\\Program Files\\Server", "C:\\mods",
                                                     "a\nb\rc\td\fe", "firstsecond", "null", "kept#hash", "present",
                                                     QStringLiteral("a\u00a0b") });
        QVERIFY(writeFile(original, provider));
        ServerJvmFilterResult prepared;
        QVERIFY2(ServerJvmArgs::writeEffectiveFileForSource(original, effective, 17, &prepared),
                 qPrintable(prepared.errorMessage));
        QByteArray actual;
        QVERIFY2(run({ "@" + original }, keys, {}, &actual), qPrintable(diagnostics));
        QCOMPARE(actual, expected);
        QVERIFY2(run({ "@" + effective }, keys, {}, &actual), qPrintable(diagnostics));
        QCOMPARE(actual, expected);
        QCOMPARE(readFile(original), provider);

        // Check the serializer independently of our parser, using values that
        // require different escaping in @-files and JAVA_TOOL_OPTIONS.
        const QStringList values{ "C:\\Program Files\\Server\\", "a\"b'c", "line\nrow\rcol\tend\f",
                                  "a#b", QStringLiteral("a\u00a0b") };
        QStringList valueKeys;
        QStringList tokens;
        QStringList environmentTokens;
        for (int i = 0; i < values.size(); ++i) {
            const QString key = QStringLiteral("probe.value%1").arg(i);
            valueKeys << key;
            tokens << ("-D" + key + '=' + values.at(i));
            environmentTokens << ServerJvmArgs::quoteEnvToken(tokens.constLast());
        }
        QString error;
        QVERIFY2(ServerJvmArgs::writeEffectiveArgfile(effective, tokens, &error), qPrintable(error));
        QVERIFY2(run({ "@" + effective }, valueKeys, {}, &actual), qPrintable(diagnostics));
        QCOMPARE(actual, encodedValues(values));
        QVERIFY2(run({}, valueKeys, environmentTokens.join(' '), &actual), qPrintable(diagnostics));
        QCOMPARE(actual, encodedValues(values));

        const QString wrapper = ServerJvmArgs::wrapperEnvironmentArgs(
            32, 64, QStringLiteral("\"-Dprobe.path=C:\\Program Files\\Server\" -Dprobe.quote=a\"\"\"b'c"));
        QVERIFY2(run({}, { "probe.path", "probe.quote", "user.language", "user.country.format" },
                     wrapper, &actual), qPrintable(diagnostics));
        QCOMPARE(actual, encodedValues({ "C:\\Program Files\\Server", "a\"b'c", "en", "US" }));
    }

    void generatesEffectiveFileWithoutMutatingOriginal()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QByteArray original(
            "# Requiem 1.20.1 supplied options\n-Xms9G\n-XX:+ZGenerational\n"
            "-XX:+UseG1GC\n-Dfile.encoding=UTF-8\n");
        const QString source = temporaryRoot.filePath("user_jvm_args.txt");
        const QString dest = temporaryRoot.filePath("jlauncher_effective_jvm_args.txt");
        QVERIFY(writeFile(source, original));

        ServerJvmFilterResult prepared;
        QVERIFY2(ServerJvmArgs::writeEffectiveFileForSource(source, dest, 17, &prepared),
                 qPrintable(prepared.errorMessage));
        QVERIFY(prepared.ok);
        QCOMPARE(readFile(source), original);
        QVERIFY(prepared.keptTokens.contains("-XX:+UseG1GC"));
        QVERIFY(!prepared.keptTokens.contains("-Xms9G"));
        QVERIFY(!prepared.keptTokens.contains("-XX:+ZGenerational"));
        const QByteArray effective = readFile(dest);
        QVERIFY(!effective.isEmpty());
        QVERIFY(effective.contains("-XX:+UseG1GC"));
        QVERIFY(!effective.contains("-Xms9G"));
        QVERIFY(!effective.contains("ZGenerational"));
        QVERIFY(effective.contains("Generated by J Launcher"));
    }

    void pinsStableLocaleOnDirectJarLaunch()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("direct-locale", "Direct locale");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server")));
        server.setMinMemory(1024);
        server.setMaxMemory(2048);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            server.consoleLog().contains("ARG:-Duser.language=en"), 5000);
        QVERIFY(server.consoleLog().contains("ARG:-Duser.country=US"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.language.format=en"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.country.format=US"));
        QVERIFY(server.consoleLog().contains("ARG:-Xmx2048M"));
        QVERIFY(server.consoleLog().contains("ARG:-Xms1024M"));
        QVERIFY(server.consoleLog().contains("[JVM] Using dedicated-server locale en_US"));

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void filtersRequiemStylePackOnJava17Launch()
    {
        ScopedEnvironmentVariable fakeJava("JLAUNCHER_FAKE_JAVA_MAJOR", "17");
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString serverDirectory = temporaryRoot.filePath("server");
#ifdef Q_OS_WIN
        const QString scriptName = QStringLiteral("run.bat");
        const QString loaderRelative = QStringLiteral(
            "libraries/net/minecraftforge/forge/1.20.1-47.4.20/win_args.txt");
        const QByteArray launchScript =
            "@echo off\r\njava @user_jvm_args.txt "
            "@libraries/net/minecraftforge/forge/1.20.1-47.4.20/win_args.txt %*\r\n";
#else
        const QString scriptName = QStringLiteral("run.sh");
        const QString loaderRelative = QStringLiteral(
            "libraries/net/minecraftforge/forge/1.20.1-47.4.20/unix_args.txt");
        const QByteArray launchScript =
            "#!/bin/sh\njava @user_jvm_args.txt "
            "@libraries/net/minecraftforge/forge/1.20.1-47.4.20/unix_args.txt \"$@\"\n";
#endif
        const QByteArray supplied =
            "# Requiem 1.20.1 / Forge 47.4.20 supplied options\n"
            "-Xms9G\n"
            "-XX:+ZGenerational\n"
            "-XX:+UseG1GC\n"
            "-Dfile.encoding=UTF-8\n";

        ServerInstance server("requiem-style", "Requiem style");
        server.setServerDirectory(serverDirectory);
        server.setVersion("1.20.1");
        server.setLoaderType("forge");
        server.setLoaderVersion("47.4.20");
        server.setPort(unusedPort());
        server.setEulaAccepted(true);
        server.setJavaPath(fakeMinecraftServerPath());
        server.setMinMemory(2048);
        server.setMaxMemory(4096);

        QVERIFY(writeFile(QDir(serverDirectory).filePath(scriptName), launchScript));
        QVERIFY(writeFile(QDir(serverDirectory).filePath("user_jvm_args.txt"), supplied));
        QVERIFY(writeFile(QDir(serverDirectory).filePath(loaderRelative),
                          "# synthetic loader arguments\n"));
        QVERIFY(writeArchive(QDir(serverDirectory).filePath("server.jar"), {
            { "META-INF/MANIFEST.MF", "Main-Class: net.minecraft.bundler.Main\n" },
        }));

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            server.consoleLog().contains("ARG:@" + ServerJvmArgs::effectiveFileName()), 5000);
        QVERIFY(!server.consoleLog().contains("ARG:@user_jvm_args.txt"));
        // Launcher memory stays authoritative after pack -Xms removal.
        QVERIFY(server.consoleLog().contains("ARG:-Xmx4096M"));
        QVERIFY(server.consoleLog().contains("ARG:-Xms2048M"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.language=en"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.country.format=US"));
        QVERIFY(server.consoleLog().contains("[JVM] Removed pack memory option '-Xms9G'"));
        QVERIFY(server.consoleLog().contains("[JVM] Removed option '-XX:+ZGenerational'"));
        QCOMPARE(readFile(QDir(serverDirectory).filePath("user_jvm_args.txt")), supplied);
        const QByteArray effective =
            readFile(QDir(serverDirectory).filePath(ServerJvmArgs::effectiveFileName()));
        QVERIFY(effective.contains("-XX:+UseG1GC"));
        QVERIFY(effective.contains("-Dfile.encoding=UTF-8"));
        QVERIFY(!effective.contains("-Xms9G"));
        QVERIFY(!effective.contains("ZGenerational"));

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void classifiesUnrecognizedVmOptionsAsJavaRuntimeCause()
    {
        QCOMPARE(ServerDiagnostics::classifyCrash("Error: Unrecognized VM option 'ZGenerational'"),
                 ServerCrashCause::JavaVersion);
        QVERIFY(ServerDiagnostics::crashCauseExplanation(ServerCrashCause::JavaVersion)
                    .contains("Java", Qt::CaseInsensitive));
    }

    void reportsProcessExitBeforeReadiness()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("startup-failure", "Startup failure");
        QVERIFY(prepareSyntheticServer(
            server, temporaryRoot.filePath("server"), "-Dfake.exit-before-ready"));
        QSignalSpy errors(&server, &ServerInstance::serverError);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Error, 5000);
        QVERIFY(!errors.isEmpty());
        QVERIFY(errors.last().first().toString().contains("before reporting"));
    }

    void emitsControlledCrashDetailsAfterReadiness()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("controlled-crash", "Controlled crash");
        QVERIFY(prepareSyntheticServer(
            server, temporaryRoot.filePath("server"), "-Dfake.crash-memory"));
        QSignalSpy crashes(&server, &ServerInstance::serverCrashed);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Error, 5000);
        QCOMPARE(crashes.size(), 1);
        QVERIFY(crashes.first().at(0).toString().contains("exit code 1"));
        QVERIFY(crashes.first().at(1).toString().contains("OutOfMemoryError"));
    }

    void reportsReadinessTimeoutAndStopsProcess()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("startup-timeout", "Startup timeout");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server"), "-Dfake.no-ready"));
        server.setStartupTimeoutSeconds(1);
        QSignalSpy errors(&server, &ServerInstance::serverError);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Error, 3000);
        QVERIFY(!errors.isEmpty());
        QVERIFY(errors.last().first().toString().contains("did not report readiness"));
        QTRY_COMPARE_WITH_TIMEOUT(server.processId(), qint64(0), 5000);
    }

    void reportsIncompatibleJavaRequirement()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("java-requirement", "Java requirement");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        server.setVersion("1.21.8");
        server.setLoaderType("vanilla");
        server.setPort(unusedPort());
        server.setEulaAccepted(true);
        server.setJavaPath(fakeMinecraftServerPath());

        const QByteArray classHeader = QByteArray::fromHex("cafebabe0000008f");
        QVERIFY(QDir().mkpath(server.serverDirectory()));
        QVERIFY(writeArchive(server.serverJarPath(), {
            { "META-INF/MANIFEST.MF", "Main-Class: net.minecraft.bundler.Main\n" },
            { "net/minecraft/bundler/Main.class", classHeader },
        }));
        QSignalSpy errors(&server, &ServerInstance::serverError);

        QVERIFY(!server.start());
        QCOMPARE(server.status(), ServerStatus::Error);
        QVERIFY(!errors.isEmpty());
        QVERIFY(errors.last().first().toString().contains("Java 99"));
    }

    void requiresTheCompatibleJavaMajorInsteadOfAnyNewerRuntime()
    {
        QVERIFY(ServerInstance::isJavaMajorCompatible(8, 8));
        QVERIFY(ServerInstance::isJavaMajorCompatible(17, 17));
        QVERIFY(!ServerInstance::isJavaMajorCompatible(8, 25));
        QVERIFY(!ServerInstance::isJavaMajorCompatible(17, 21));
        QVERIFY(ServerInstance::isJavaMajorCompatible(0, 25));
    }

    void acceptsLegacyAndModularManagedJavaLayouts()
    {
#ifndef Q_OS_WIN
        QSKIP("Managed Java layout validation is Windows-specific.");
#endif
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());

        const QString legacyJava = root.filePath("legacy/bin/java.exe");
        QVERIFY(writeFile(legacyJava, "java"));
        QVERIFY(writeFile(root.filePath("legacy/lib/amd64/jvm.cfg"), "-server KNOWN"));
        QVERIFY(!Java::JavaRuntimeInstallTask::isUsableJava(legacyJava));
        QVERIFY(!JavaUtils::isJavaPathSafeToProbe(legacyJava, root.path()));
        QVERIFY(writeFile(root.filePath("legacy/lib/rt.jar"), "runtime"));
        QVERIFY(writeFile(root.filePath("legacy/bin/server/jvm.dll"), "vm"));
        QVERIFY(Java::JavaRuntimeInstallTask::isUsableJava(legacyJava));
        QVERIFY(JavaUtils::isJavaPathSafeToProbe(legacyJava, root.path()));

        const QString modularJava = root.filePath("modular/bin/java.exe");
        QVERIFY(writeFile(modularJava, "java"));
        QVERIFY(writeFile(root.filePath("modular/lib/jvm.cfg"), "-server KNOWN"));
        QVERIFY(writeFile(root.filePath("modular/lib/modules"), "runtime"));
        QVERIFY(writeFile(root.filePath("modular/bin/server/jvm.dll"), "vm"));
        QVERIFY(Java::JavaRuntimeInstallTask::isUsableJava(modularJava));
        QVERIFY(JavaUtils::isJavaPathSafeToProbe(modularJava, root.path()));
        QVERIFY(JavaUtils::isJavaPathSafeToProbe(QStringLiteral("javaw"), root.path()));
        QVERIFY(!JavaUtils::isJavaPathSafeToProbe(root.filePath("missing/bin/javaw.exe"), root.path()));
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
            "exact fabric server");
        fixtureHttp.addRoute("/purpur/purpur/1.21.1",
                             R"({"builds":{"latest":"2412","all":[2400,2412]}})");
        fixtureHttp.addRoute("/purpur/purpur/1.21.1/2400/download",
                             "exact purpur server");

        QVERIFY(writeFile(
            root.filePath(
                "forge-maven/net/minecraftforge/forge/1.12.2-14.23.5.2860/"
                "forge-1.12.2-14.23.5.2860-installer.jar"),
            "exact forge installer"));
        QVERIFY(!QFileInfo::exists(
            root.filePath("forge-maven/net/minecraftforge/forge/maven-metadata.xml")));
        QVERIFY(writeFile(
            root.filePath(
                "neoforge-maven/net/neoforged/neoforge/21.1.233/"
                "neoforge-21.1.233-installer.jar"),
            "exact neoforge installer"));

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
        fixtureHttp.addRoute("/fabric/versions/installer",
                             R"([{"version":"1.0.0","stable":true}])");
        fixtureHttp.addRoute("/fabric/versions/loader/1.21.8",
                             R"([{"loader":{"version":"0.16.10"}}])");
        fixtureHttp.addRoute(
            "/fabric/versions/loader/1.21.8/0.16.10/1.0.0/server/jar",
            serverPayload);
        fixtureHttp.addRoute("/purpur/purpur/1.21.8",
                             R"({"builds":{"latest":"2412"}})");
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

        const QString neoForgeVersions = root.filePath("fixtures/neoforge-versions.json");
        QVERIFY(writeFile(neoForgeVersions, R"({"versions":["21.1.50"]})"));
        QVERIFY(writeFile(
            root.filePath(
                "fixtures/neoforge-maven/net/neoforged/neoforge/21.1.50/"
                "neoforge-21.1.50-installer.jar"),
            "synthetic neoforge installer"));

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
                QCOMPARE(installed.readAll(), serverPayload);
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

        const QString pack = temporaryRoot.filePath("active-pack.zip");
        QVERIFY(writeArchive(pack, { { "mods/from-pack.jar", "blocked" } }));
        error.clear();
        QVERIFY(!server.importServerPack(pack, &error));
        QVERIFY(error.contains("Stop the server"));
        QVERIFY(!QFileInfo::exists(QDir(server.modsDirectory()).filePath("from-pack.jar")));

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
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

QTEST_GUILESS_MAIN(ServerInstanceTest)

#include "ServerInstance_test.moc"
