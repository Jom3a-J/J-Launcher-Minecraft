/* Copyright 2013-2024 MultiMC Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The server's files: paths, launch scripts, installed content, server packs, server.properties and the EULA.

#include "ServerInstance.h"
#include "ServerDownloader.h"
#include "ServerPackImportTransaction.h"
#include "ServerProperties.h"
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QTextStream>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

QString ServerInstance::serverJarPath() const
{
    const QDir dir(m_serverDirectory);
    const QString directJar = dir.filePath(QStringLiteral("server.jar"));
    if (QFileInfo(directJar).isFile()) {
        return directJar;
    }

    const QString loader = m_loaderType.trimmed().toLower();
    QStringList filters;
    if (loader == QStringLiteral("forge")) {
        filters << QStringLiteral("forge-*.jar");
    } else if (loader == QStringLiteral("neoforge")) {
        filters << QStringLiteral("neoforge-*.jar");
    } else if (loader == QStringLiteral("fabric")) {
        filters << QStringLiteral("fabric-server*.jar");
    } else if (loader == QStringLiteral("paper")) {
        filters << QStringLiteral("paper-*.jar");
    } else if (loader == QStringLiteral("purpur")) {
        filters << QStringLiteral("purpur-*.jar");
    }

    if (filters.isEmpty()) {
        return directJar;
    }

    const QFileInfoList jars = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const QFileInfo &jar : jars) {
        if (!jar.fileName().contains(QStringLiteral("installer"), Qt::CaseInsensitive)) {
            return jar.absoluteFilePath();
        }
    }

    // Modern Forge and NeoForge installations can launch through run scripts
    // without a standalone server JAR in the server root. Keep the conventional
    // path for diagnostics and the Java version floor in that case.
    return directJar;
}

QString ServerInstance::serverPropertiesPath() const
{
    return QDir(m_serverDirectory).filePath("server.properties");
}

QString ServerInstance::modsDirectory() const
{
    return QDir(m_serverDirectory).filePath("mods");
}

QString ServerInstance::pluginsDirectory() const
{
    return QDir(m_serverDirectory).filePath("plugins");
}

ServerContentType ServerInstance::contentTypeForLoader(const QString &loaderType)
{
    const QString loader = loaderType.trimmed().toLower();
    if (loader == QStringLiteral("fabric")
        || loader == QStringLiteral("forge")
        || loader == QStringLiteral("neoforge")) {
        return ServerContentType::Mod;
    }
    if (loader == QStringLiteral("paper") || loader == QStringLiteral("purpur")) {
        return ServerContentType::Plugin;
    }
    return ServerContentType::None;
}

ServerContentType ServerInstance::contentType() const
{
    return contentTypeForLoader(m_loaderType);
}

QString ServerInstance::contentDirectory() const
{
    switch (contentType()) {
        case ServerContentType::Mod:
            return modsDirectory();
        case ServerContentType::Plugin:
            return pluginsDirectory();
        case ServerContentType::None:
            return QString();
    }
    return QString();
}

bool ServerInstance::hasLaunchTarget() const
{
    if (QFileInfo::exists(serverLoaderInstallIncompleteMarkerPath(m_serverDirectory))) {
        return false;
    }

    const QString loader = m_loaderType.trimmed().toLower();
    if ((loader == QStringLiteral("forge") || loader == QStringLiteral("neoforge")) &&
        QFileInfo(loaderScriptPath()).isFile()) {
        return true;
    }
    return QFileInfo(serverJarPath()).isFile();
}

QString ServerInstance::loaderScriptPath() const
{
#ifdef Q_OS_WIN
    const QString preferredName = QStringLiteral("run.bat");
    const QString fallbackName = QStringLiteral("start.bat");
#else
    const QString preferredName = QStringLiteral("run.sh");
    const QString fallbackName = QStringLiteral("start.sh");
#endif
    const QDir serverDir(m_serverDirectory);
    const QString preferred = serverDir.filePath(preferredName);
    if (QFileInfo(preferred).isFile()) {
        return preferred;
    }
    const QString fallback = serverDir.filePath(fallbackName);
    return QFileInfo(fallback).isFile() ? fallback : preferred;
}

QString ServerInstance::loaderArgumentsFile() const
{
#ifdef Q_OS_WIN
    const QString argumentsFileName = QStringLiteral("win_args.txt");
#else
    const QString argumentsFileName = QStringLiteral("unix_args.txt");
#endif

    const QDir serverDir(m_serverDirectory);
    QFile script(loaderScriptPath());
    if (script.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString contents = QString::fromUtf8(script.readAll());
        const QRegularExpression argumentReference(
            QStringLiteral("@(?:\\\"([^\\\"\\r\\n]+%1)\\\"|([^\\s\\\"\\r\\n]+%1))")
                .arg(QRegularExpression::escape(argumentsFileName)),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = argumentReference.match(contents);
        if (match.hasMatch()) {
            const QString reference = QDir::fromNativeSeparators(
                match.captured(1).isEmpty() ? match.captured(2) : match.captured(1));
            const QString path = QFileInfo(reference).isAbsolute()
                ? reference
                : serverDir.filePath(reference);
            if (QFileInfo::exists(path)) {
                return QFileInfo(path).absoluteFilePath();
            }
        }
    }

    QStringList conventionalPaths;
    const QString loader = m_loaderType.trimmed().toLower();
    if (loader == QStringLiteral("forge")) {
        QStringList coordinates;
        if (!m_loaderVersion.isEmpty()) {
            coordinates << m_loaderVersion;
            if (!m_version.isEmpty() && !m_loaderVersion.startsWith(m_version + '-')) {
                coordinates.prepend(m_version + '-' + m_loaderVersion);
            }
        }
        for (const QString& coordinate : coordinates) {
            conventionalPaths << serverDir.filePath(
                QStringLiteral("libraries/net/minecraftforge/forge/%1/%2")
                    .arg(coordinate, argumentsFileName));
        }
    } else if (loader == QStringLiteral("neoforge") && !m_loaderVersion.isEmpty()) {
        conventionalPaths << serverDir.filePath(
            QStringLiteral("libraries/net/neoforged/neoforge/%1/%2")
                .arg(m_loaderVersion, argumentsFileName));
        if (m_version == QStringLiteral("1.20.1")
            || m_loaderVersion.startsWith(QStringLiteral("47."))) {
            conventionalPaths.prepend(serverDir.filePath(
                QStringLiteral("libraries/net/neoforged/forge/%1-%2/%3")
                    .arg(m_version, m_loaderVersion, argumentsFileName)));
        }
    }
    for (const QString& path : conventionalPaths) {
        if (QFileInfo::exists(path)) {
            return QFileInfo(path).absoluteFilePath();
        }
    }

    // Imported packs may omit version metadata. A single generated argument
    // file is unambiguous; multiple files must be left to the pack's script.
    QStringList discoveredPaths;
    QDirIterator iterator(serverDir.filePath(QStringLiteral("libraries")),
                          { argumentsFileName }, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        discoveredPaths << QFileInfo(iterator.next()).absoluteFilePath();
    }
    return discoveredPaths.size() == 1 ? discoveredPaths.first() : QString();
}

bool ServerInstance::addMods(const QStringList &paths, QString *error)
{
    return addContentFiles(paths, error);
}

bool ServerInstance::invalidateContentCaches(QString *error) const
{
    if (contentType() != ServerContentType::Mod) {
        return true;
    }
    const QStringList caches{
        QDir(modsDirectory()).filePath(QStringLiteral(".connector")),
        QDir(m_serverDirectory).filePath(QStringLiteral(".fabric/processedMods")),
    };
    for (const QString &path : caches) {
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink()) {
            continue;
        }
        const bool removed = info.isSymLink() || !info.isDir()
            ? QFile::remove(path)
            : QDir(path).removeRecursively();
        if (!removed) {
            if (error) {
                *error = tr("Could not clear the generated mod cache '%1'.")
                             .arg(QFileInfo(path).fileName());
            }
            return false;
        }
    }
    return true;
}

bool ServerInstance::addContentFiles(const QStringList &paths, QString *error)
{
    const ServerContentType type = contentType();
    const QString contentName = type == ServerContentType::Plugin ? tr("plugin") : tr("mod");
    if (type == ServerContentType::None) {
        if (error) {
            *error = tr("This server type does not support launcher-managed mods or plugins.");
        }
        return false;
    }
    if (isActive()) {
        if (error) {
            *error = tr("Stop the server before adding %1 files.").arg(contentName);
        }
        return false;
    }
    if (paths.isEmpty()) {
        if (error) {
            *error = tr("No %1 files were selected.").arg(contentName);
        }
        return false;
    }

    for (const QString &path : paths) {
        const QFileInfo source(path);
        if (!source.exists() || !source.isFile() || source.suffix().compare("jar", Qt::CaseInsensitive) != 0) {
            if (error) {
                *error = tr("'%1' is not a readable .jar %2 file.").arg(path, contentName);
            }
            return false;
        }
    }

    if (!invalidateContentCaches(error)) {
        return false;
    }

    const QString destinationDirectory = contentDirectory();
    if (!QDir().mkpath(destinationDirectory)) {
        if (error) {
            *error = tr("Could not create the server %1 folder.").arg(
                type == ServerContentType::Plugin ? tr("plugins") : tr("mods"));
        }
        return false;
    }

    for (const QString &path : paths) {
        const QFileInfo source(path);
        const QString destination = QDir(destinationDirectory).filePath(source.fileName());
        if (QFileInfo(destination).absoluteFilePath() == source.absoluteFilePath()) {
            continue;
        }
        QFile input(source.absoluteFilePath());
        QSaveFile output(destination);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
            if (error) {
                *error = tr("Could not prepare '%1' for installation.").arg(source.fileName());
            }
            return false;
        }
        const QByteArray data = input.readAll();
        if (output.write(data) != data.size() || !output.commit()) {
            output.cancelWriting();
            if (error) {
                *error = tr("Could not install '%1' in the server %2 folder.")
                             .arg(source.fileName(),
                                  type == ServerContentType::Plugin ? tr("plugins") : tr("mods"));
            }
            return false;
        }
    }
    return true;
}

bool ServerInstance::importServerPack(const QString &archivePath, QString *error)
{
    if (!beginServerPackImport(archivePath, error)) {
        return false;
    }
    const bool imported = importServerPackFiles(m_serverDirectory, archivePath, error);
    finishServerPackImport(imported);
    return imported;
}

bool ServerInstance::beginServerPackImport(const QString &archivePath, QString *error)
{
    if (isBusy()) {
        if (error) {
            *error = m_serverPackImportInProgress
                ? tr("A server pack is already being imported.")
                : tr("Stop the server before importing a server pack.");
        }
        return false;
    }
    if (!QFileInfo(archivePath).isFile()) {
        if (error) {
            *error = tr("The selected server pack does not exist.");
        }
        return false;
    }
    cancelPendingCrashRestart();
    m_serverPackImportInProgress = true;
    return true;
}

bool ServerInstance::importServerPackFiles(const QString &serverDirectory,
                                           const QString &archivePath, QString *error)
{
    ServerPackImportTransaction transaction(serverDirectory);
    if (!transaction.stage(archivePath, error)) {
        return false;
    }
    return transaction.publish(error);
}

void ServerInstance::finishServerPackImport(bool imported)
{
    m_serverPackImportInProgress = false;
    if (imported) {
        syncPortFromServerProperties();
    }
}

bool ServerInstance::createServerProperties()
{
    QFile file(serverPropertiesPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    stream << "#Minecraft server properties\n";
    stream << "server-port=" << m_port << "\n";
    stream << "online-mode=true\n";
    stream << "max-players=20\n";
    stream << "level-name=world\n";
    stream << "gamemode=survival\n";
    stream << "difficulty=easy\n";
    stream << "allow-nether=true\n";
    stream << "spawn-protection=16\n";
    stream << "view-distance=10\n";
    stream << "white-list=false\n";
    stream << "enable-command-block=false\n";
    stream << "spawn-monsters=true\n";
    stream << "spawn-animals=true\n";
    stream << "spawn-npcs=true\n";
    stream << "generate-structures=true\n";
    stream << "pvp=true\n";
    stream << "allow-flight=false\n";
    stream << "max-world-size=29999984\n";
    stream << "motd=A Minecraft Server\n";

    file.close();
    return true;
}

void ServerInstance::syncPortFromServerProperties()
{
    if (m_serverDirectory.trimmed().isEmpty()) {
        return;
    }
    const QFileInfo propertiesFile(serverPropertiesPath());
    if (!propertiesFile.isFile()) {
        return;
    }
    const QMap<QString, QString> properties = ServerProperties::load(serverPropertiesPath());
    bool validPort = false;
    const int configuredPort = properties.value(QStringLiteral("server-port"))
                                   .toInt(&validPort);
    if (validPort && configuredPort >= 1 && configuredPort <= 65535) {
        m_port = configuredPort;
    }
}

bool ServerInstance::acceptEULA()
{
    QFile file(QDir(m_serverDirectory).filePath("eula.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    stream << "eula=true\n";

    file.close();
    return true;
}
