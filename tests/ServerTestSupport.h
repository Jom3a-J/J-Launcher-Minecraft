// SPDX-License-Identifier: GPL-3.0-only

// Helpers shared by the server test programs: files and archives, fixture HTTP servers,
// the fake Minecraft server, and synthetic mod jars.

#pragma once

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include "RangeHttpServer.h"
#include <FileSystem.h>
#include <algorithm>
#include <archive/ArchiveWriter.h>
#include <java/JavaRuntimeInstallTask.h>
#include <java/JavaUtils.h>
#include <modplatform/helpers/OverrideUtils.h>
#include <net/HostScheduler.h>
#include <net/PartFile.h>
#include <net/SegmentedDownload.h>
#include <server/ServerContentUpdater.h>
#include <server/ServerDiagnostics.h>
#include <server/ServerDownloader.h>
#include <server/ServerInstance.h>
#include <server/ServerJvmArgs.h>
#include <server/ServerManager.h>
#include <server/ServerModpackInstaller.h>
#include <server/ServerPackCompatibility.h>
#include <server/ServerPlayerAccess.h>
#include <server/ServerProperties.h>
#include <utility>

namespace ServerTestSupport {
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

bool writeInstallerChecksum(const QString& installerPath, const QByteArray& checksum = {})
{
    const QByteArray installer = readFile(installerPath);
    const QByteArray sha1 = checksum.isEmpty()
        ? QCryptographicHash::hash(installer, QCryptographicHash::Sha1).toHex()
        : checksum;
    return !installer.isEmpty()
        && writeFile(installerPath + QStringLiteral(".sha1"),
                     sha1 + "  " + QFileInfo(installerPath).fileName().toLatin1());
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
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    MMCZip::ArchiveWriter archive(path);
    if (!archive.open()) {
        return false;
    }
    for (const auto& entry : entries) {
        if (!archive.addFile(entry.first, entry.second)) {
            return false;
        }
    }
    return archive.close().has_value();
}

QByteArray fabricServerJarFixture(const QString& path)
{
    return writeArchive(path, {
        { "META-INF/MANIFEST.MF", "Main-Class: net.fabricmc.loader.impl.launch.server.FabricServerLauncher\n" },
    }) ? readFile(path) : QByteArray();
}

QJsonObject installerLibrary(const QString& name, const QString& path,
                             const QUrl& url, const QByteArray& contents)
{
    return {
        { "name", name },
        { "downloads", QJsonObject{
            { "artifact", QJsonObject{
                { "path", path },
                { "url", url.toString() },
                { "sha1", QString::fromLatin1(
                    QCryptographicHash::hash(contents, QCryptographicHash::Sha1).toHex()) },
                { "size", contents.size() },
            } },
        } },
    };
}

bool writeModernInstaller(const QString& path, const QJsonArray& profileLibraries,
                          const QJsonArray& versionLibraries)
{
    const QByteArray profile = QJsonDocument(QJsonObject{
        { "spec", 1 },
        { "libraries", profileLibraries },
        { "data", QJsonObject{} },
    }).toJson(QJsonDocument::Compact);
    const QByteArray version = QJsonDocument(QJsonObject{
        { "spec", 1 },
        { "libraries", versionLibraries },
    }).toJson(QJsonDocument::Compact);
    return writeArchive(path, {
        { "install_profile.json", profile },
        { "version.json", version },
    }) && writeInstallerChecksum(path);
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
    return archive.close().has_value();
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
    return archive.close().has_value();
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
}  // namespace ServerTestSupport
