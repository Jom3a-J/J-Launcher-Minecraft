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

#include "ServerManager.h"
#include "ServerInstance.h"
#include "FileSystem.h"
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QDateTime>
#include <QLocale>
#include <algorithm>

namespace {
const QString BACKUP_MANIFEST = QStringLiteral(".jlauncher-backup.json");
constexpr int BACKUP_FORMAT_VERSION = 1;

bool copyDirectoryContents(const QString &sourcePath, const QString &destinationPath,
                           QString *error, const QSet<QString> &excludedTopLevels = {})
{
    const QDir source(sourcePath);
    if (!source.exists() || !QDir().mkpath(destinationPath)) {
        if (error) {
            *error = QObject::tr("Could not prepare the backup staging directory.");
        }
        return false;
    }

    const QFileInfoList entries =
        source.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        bool excluded = false;
        for (const QString &name : excludedTopLevels) {
            if (entry.fileName().compare(name, Qt::CaseInsensitive) == 0) {
                excluded = true;
                break;
            }
        }
        if (excluded) {
            continue;
        }
        if (entry.isSymLink()) {
            if (error) {
                *error = QObject::tr("Unsafe symbolic link found in backup data: %1").arg(entry.fileName());
            }
            return false;
        }
        const QString destination = QDir(destinationPath).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirectoryContents(entry.absoluteFilePath(), destination, error)) {
                return false;
            }
        } else if (entry.isFile()) {
            if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
                if (error) {
                    *error = QObject::tr("Could not replace %1.").arg(entry.fileName());
                }
                return false;
            }
            if (!QFile::copy(entry.absoluteFilePath(), destination)) {
                if (error) {
                    *error = QObject::tr("Could not copy %1.").arg(entry.fileName());
                }
                return false;
            }
        }
    }
    return true;
}

qint64 directorySize(const QString &directoryPath)
{
    qint64 total = 0;
    QDirIterator iterator(directoryPath, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        total += iterator.fileInfo().size();
    }
    return total;
}

QStringList includedBackupCategories(const QString &serverDirectory)
{
    const QDir root(serverDirectory);
    QStringList categories;
    const auto hasAny = [&root](const QStringList &paths) {
        for (const QString &path : paths) {
            if (QFileInfo::exists(root.filePath(path))) {
                return true;
            }
        }
        return false;
    };
    if (hasAny({ "world", "world_nether", "world_the_end" })) {
        categories.append(QStringLiteral("world-data"));
    }
    if (hasAny({ "world/playerdata", "world/advancements", "world/stats",
                 "usercache.json", "whitelist.json", "ops.json",
                 "banned-players.json", "banned-ips.json" })) {
        categories.append(QStringLiteral("player-data"));
    }
    if (hasAny({ "server.properties", "eula.txt", "config", "configureddefaults",
                 "datapacks", "defaultconfigs", "ftbteambases", "global_packs",
                 "kubejs", "openloader", "patchouli_books", "resources", "scripts",
                 "structures" })) {
        categories.append(QStringLiteral("configuration"));
    }
    if (hasAny({ "mods" })) {
        categories.append(QStringLiteral("mods"));
    }
    if (hasAny({ "plugins" })) {
        categories.append(QStringLiteral("plugins"));
    }
    if (hasAny({ "server.jar", "run.bat", "libraries", "versions" })) {
        categories.append(QStringLiteral("server-runtime"));
    }
    return categories;
}

bool pathIsWithin(const QString &rootPath, const QString &candidatePath)
{
    const QString canonicalRoot = QFileInfo(rootPath).canonicalFilePath();
    const QString canonicalCandidate = QFileInfo(candidatePath).canonicalFilePath();
    if (canonicalRoot.isEmpty() || canonicalCandidate.isEmpty()) {
        return false;
    }
    const QString rootPrefix =
        QDir::fromNativeSeparators(QDir::cleanPath(canonicalRoot)) + QLatin1Char('/');
    return QDir::fromNativeSeparators(QDir::cleanPath(canonicalCandidate)).startsWith(
        rootPrefix,
#ifdef Q_OS_WIN
        Qt::CaseInsensitive
#else
        Qt::CaseSensitive
#endif
    );
}

QString sanitizedBackupName(const QString &requestedName)
{
    QString safeName;
    const QString invalidCharacters = QStringLiteral("\\/:*?\"<>|");
    for (const QChar character : requestedName.trimmed()) {
        safeName.append(invalidCharacters.contains(character) ? QChar('_') : character);
    }
    safeName = safeName.trimmed();
    if (safeName.size() > 80) {
        safeName.truncate(80);
    }
    return safeName;
}

QJsonObject backupManifest(const std::shared_ptr<ServerInstance> &server,
                           const QString &displayName, const QDateTime &createdAt,
                           const QStringList &categories)
{
    QJsonArray categoryArray;
    for (const QString &category : categories) {
        categoryArray.append(category);
    }
    return {
        { "format", QStringLiteral("JLauncherServerBackup") },
        { "formatVersion", BACKUP_FORMAT_VERSION },
        { "name", displayName },
        { "serverId", server->id() },
        { "serverName", server->name() },
        { "minecraftVersion", server->version() },
        { "loaderType", server->loaderType() },
        { "loaderVersion", server->loaderVersion() },
        { "createdUtc", createdAt.toUTC().toString(Qt::ISODateWithMs) },
        { "includedCategories", categoryArray },
    };
}

bool writeBackupManifest(const QString &backupPath, const QJsonObject &manifest, QString *error)
{
    QSaveFile file(QDir(backupPath).filePath(BACKUP_MANIFEST));
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        if (error) {
            *error = QObject::tr("Could not write the backup manifest.");
        }
        return false;
    }
    return true;
}

ServerBackupInfo inspectBackup(const std::shared_ptr<ServerInstance> &server,
                               const QString &backupPath)
{
    ServerBackupInfo info;
    info.path = QFileInfo(backupPath).absoluteFilePath();
    info.name = QFileInfo(backupPath).fileName();
    info.size = directorySize(backupPath);

    QFile manifestFile(QDir(backupPath).filePath(BACKUP_MANIFEST));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        info.validationError = QObject::tr("The backup manifest is missing.");
        return info;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        info.validationError = QObject::tr("The backup manifest is not valid JSON.");
        return info;
    }
    const QJsonObject manifest = document.object();
    if (manifest.value("format").toString() != QStringLiteral("JLauncherServerBackup")
        || manifest.value("formatVersion").toInt() != BACKUP_FORMAT_VERSION) {
        info.validationError = QObject::tr("The backup format is not supported.");
        return info;
    }
    info.serverId = manifest.value("serverId").toString();
    info.serverName = manifest.value("serverName").toString();
    info.minecraftVersion = manifest.value("minecraftVersion").toString();
    info.loaderType = manifest.value("loaderType").toString();
    info.loaderVersion = manifest.value("loaderVersion").toString();
    info.createdAt = QDateTime::fromString(manifest.value("createdUtc").toString(), Qt::ISODate);
    for (const QJsonValue &category : manifest.value("includedCategories").toArray()) {
        info.includedCategories.append(category.toString());
    }
    const QString displayName = manifest.value("name").toString();
    if (!displayName.isEmpty()) {
        info.name = displayName;
    }
    if (info.serverId != server->id()) {
        info.validationError = QObject::tr("This backup belongs to a different server.");
        return info;
    }
    if (!info.createdAt.isValid()) {
        info.validationError = QObject::tr("The backup creation time is invalid.");
        return info;
    }
    info.valid = true;
    return info;
}

bool createBackupSnapshot(const std::shared_ptr<ServerInstance> &server,
                          const QString &backupRoot, const QString &finalFolderName,
                          const QString &displayName, ServerBackupInfo *createdBackup,
                          QString *error)
{
    if (!QDir().mkpath(backupRoot)) {
        if (error) {
            *error = QObject::tr("Could not create the backup folder.");
        }
        return false;
    }
    const QString stagingName = QStringLiteral(".incomplete-%1")
                                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString stagingPath = QDir(backupRoot).filePath(stagingName);
    if (!copyDirectoryContents(server->serverDirectory(), stagingPath, error,
                               { QStringLiteral("backups") })) {
        QDir(stagingPath).removeRecursively();
        return false;
    }

    const QDateTime createdAt = QDateTime::currentDateTimeUtc();
    const QStringList categories = includedBackupCategories(server->serverDirectory());
    if (!writeBackupManifest(stagingPath,
                             backupManifest(server, displayName, createdAt, categories), error)) {
        QDir(stagingPath).removeRecursively();
        return false;
    }

    QString uniqueFolderName = finalFolderName;
    int suffix = 2;
    while (QFileInfo::exists(QDir(backupRoot).filePath(uniqueFolderName))) {
        uniqueFolderName = QStringLiteral("%1-%2").arg(finalFolderName).arg(suffix++);
    }
    if (!QDir(backupRoot).rename(stagingName, uniqueFolderName)) {
        QDir(stagingPath).removeRecursively();
        if (error) {
            *error = QObject::tr("Could not publish the completed backup.");
        }
        return false;
    }

    if (createdBackup) {
        *createdBackup = inspectBackup(server, QDir(backupRoot).filePath(uniqueFolderName));
    }
    return true;
}

bool clearServerContents(const QString &directoryPath, QString *error)
{
    const QDir directory(directoryPath);
    const QFileInfoList entries =
        directory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        if (entry.fileName().compare("backups", Qt::CaseInsensitive) == 0) {
            continue;
        }
        const bool removed = entry.isDir() && !entry.isSymLink()
            ? QDir(entry.absoluteFilePath()).removeRecursively()
            : QFile::remove(entry.absoluteFilePath());
        if (!removed) {
            if (error) {
                *error = QObject::tr("Could not clear %1.").arg(entry.fileName());
            }
            return false;
        }
    }
    return true;
}
}

ServerManager::ServerManager(const QString &dataDir, QObject *parent)
    : QObject(parent)
    , m_dataDir(dataDir)
    , m_serversFile(QDir(dataDir).filePath("servers.json"))
{
    // Ensure servers directory exists
    QDir dir(dataDir);
    dir.mkpath("servers");
}

ServerManager::~ServerManager()
{
    save();
}

std::shared_ptr<ServerInstance> ServerManager::createServer(const QString &name, const QString &version,
                                                           const QString &loaderType,
                                                           const QString &loaderVersion)
{
    QString id = generateId();
    QString serverDir = QDir(m_dataDir).filePath("servers/" + id);

    auto server = std::make_shared<ServerInstance>(id, name);
    server->setVersion(version);
    server->setLoaderType(loaderType);
    server->setLoaderVersion(loaderVersion);
    server->setServerDirectory(serverDir);

    // Create server directory
    QDir().mkpath(serverDir);

    m_servers[id] = server;
    save();

    emit serverAdded(id);
    return server;
}

bool ServerManager::deleteServer(const QString &id)
{
    if (!m_servers.contains(id)) {
        return false;
    }

    auto server = m_servers[id];

    // Never remove a directory while its Java process is starting, running,
    // stopping, or downloading files into it.
    if (server->status() != ServerStatus::Stopped && server->status() != ServerStatus::Error) {
        return false;
    }

    const QString originalPath = server->serverDirectory();
    QString trashPath;
    if (QFileInfo::exists(originalPath) && !FS::trash(originalPath, &trashPath)) {
        return false;
    }

    m_servers.remove(id);
    if (!save()) {
        m_servers.insert(id, server);
        if (!trashPath.isEmpty()) {
            QFile(trashPath).rename(originalPath);
        }
        return false;
    }

    m_trashHistory.push({ id, originalPath, trashPath, server });

    emit serverRemoved(id);
    return true;
}

bool ServerManager::deleteServerPermanently(const QString &id)
{
    if (!m_servers.contains(id)) {
        return false;
    }

    auto server = m_servers[id];
    if (server->status() != ServerStatus::Stopped && server->status() != ServerStatus::Error) {
        return false;
    }

    const QString originalPath = server->serverDirectory();
    QString stagedPath;
    if (QFileInfo::exists(originalPath)) {
        stagedPath = originalPath + QStringLiteral(".deleting-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QDir().rename(originalPath, stagedPath)) {
            return false;
        }
    }

    m_servers.remove(id);
    if (!save()) {
        m_servers.insert(id, server);
        if (!stagedPath.isEmpty()) {
            QDir().rename(stagedPath, originalPath);
        }
        return false;
    }

    if (!stagedPath.isEmpty() && !FS::deletePath(stagedPath)) {
        QString restoredPath = originalPath;
        if (!QDir().rename(stagedPath, originalPath)) {
            restoredPath = stagedPath;
            server->setServerDirectory(restoredPath);
        }
        m_servers.insert(id, server);
        save();
        return false;
    }

    emit serverRemoved(id);
    return true;
}

bool ServerManager::restoreLastDeletedServer(QString *restoredId)
{
    if (m_trashHistory.isEmpty()) {
        return false;
    }

    TrashHistoryItem item = m_trashHistory.top();
    QString targetPath = item.originalPath;
    int suffix = 1;
    while (QFileInfo::exists(targetPath)) {
        targetPath = item.originalPath + QString("-restored-%1").arg(suffix++);
    }
    if (!item.trashPath.isEmpty() && !QFile(item.trashPath).rename(targetPath)) {
        return false;
    }

    item.server->setServerDirectory(targetPath);
    m_servers.insert(item.id, item.server);
    if (!save()) {
        m_servers.remove(item.id);
        if (!item.trashPath.isEmpty()) {
            QFile(targetPath).rename(item.trashPath);
        }
        item.server->setServerDirectory(item.originalPath);
        return false;
    }

    m_trashHistory.pop();
    if (restoredId) {
        *restoredId = item.id;
    }
    emit serverAdded(item.id);
    return true;
}

bool ServerManager::createServerBackup(const QString &id, const QString &requestedName,
                                       ServerBackupInfo *createdBackup, QString *error)
{
    const auto server = m_servers.value(id);
    if (!server
        || (server->status() != ServerStatus::Stopped && server->status() != ServerStatus::Error)) {
        if (error) {
            *error = tr("Stop the selected server before creating a backup.");
        }
        return false;
    }
    const QString safeName = sanitizedBackupName(requestedName);
    if (safeName.isEmpty()) {
        if (error) {
            *error = tr("Enter a valid backup name.");
        }
        return false;
    }
    const QString backupRoot = QDir(server->serverDirectory()).filePath("backups");
    const QString folderName =
        QStringLiteral("server-%1-%2")
            .arg(safeName, QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz"));
    return createBackupSnapshot(server, backupRoot, folderName, safeName, createdBackup, error);
}

QList<ServerBackupInfo> ServerManager::listServerBackups(const QString &id) const
{
    QList<ServerBackupInfo> backups;
    const auto server = m_servers.value(id);
    if (!server) {
        return backups;
    }
    const QString backupRoot = QDir(server->serverDirectory()).filePath("backups");
    const QFileInfoList directories =
        QDir(backupRoot).entryInfoList({ QStringLiteral("server-*") },
                                       QDir::Dirs | QDir::NoDotAndDotDot,
                                       QDir::Time);
    for (const QFileInfo &directory : directories) {
        backups.append(inspectBackup(server, directory.absoluteFilePath()));
    }
    return backups;
}

bool ServerManager::restoreServerBackup(const QString &id, const QString &backupPath,
                                        QString *error, QString *safetyBackupName)
{
    const auto server = m_servers.value(id);
    if (!server
        || (server->status() != ServerStatus::Stopped && server->status() != ServerStatus::Error)) {
        if (error) {
            *error = tr("Stop the selected server before restoring a backup.");
        }
        return false;
    }
    const QString backupRoot = QDir(server->serverDirectory()).filePath("backups");
    if (!QFileInfo(backupPath).isDir() || !pathIsWithin(backupRoot, backupPath)) {
        if (error) {
            *error = tr("The selected backup is outside this server's backup folder.");
        }
        return false;
    }
    const ServerBackupInfo backup = inspectBackup(server, backupPath);
    if (!backup.valid) {
        if (error) {
            *error = backup.validationError;
        }
        return false;
    }

    QTemporaryDir staging;
    if (!staging.isValid()) {
        if (error) {
            *error = tr("Could not create a temporary restore staging directory.");
        }
        return false;
    }
    const QString stagedReplacement = QDir(staging.path()).filePath("replacement");
    if (!copyDirectoryContents(backupPath, stagedReplacement, error, { BACKUP_MANIFEST })) {
        return false;
    }

    const QString serverDirectory = server->serverDirectory();
    const QString rollbackName =
        QString("server-before-restore-%1").arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"));
    ServerBackupInfo rollbackBackup;
    if (!createBackupSnapshot(server, backupRoot, rollbackName,
                              tr("Before restore"), &rollbackBackup, error)) {
        return false;
    }

    if (!clearServerContents(serverDirectory, error)) {
        return false;
    }
    if (!copyDirectoryContents(stagedReplacement, serverDirectory, error)) {
        QString rollbackError;
        clearServerContents(serverDirectory, &rollbackError);
        if (!copyDirectoryContents(rollbackBackup.path, serverDirectory, &rollbackError,
                                   { BACKUP_MANIFEST }) && error) {
            *error += tr(" Automatic rollback also failed: %1").arg(rollbackError);
        }
        return false;
    }

    const QString previousMinecraftVersion = server->version();
    const QString previousLoaderType = server->loaderType();
    const QString previousLoaderVersion = server->loaderVersion();
    if (!backup.minecraftVersion.isEmpty()) {
        server->setVersion(backup.minecraftVersion);
    }
    if (!backup.loaderType.isEmpty()) {
        server->setLoaderType(backup.loaderType);
        server->setLoaderVersion(backup.loaderVersion);
    }
    if (!save()) {
        server->setVersion(previousMinecraftVersion);
        server->setLoaderType(previousLoaderType);
        server->setLoaderVersion(previousLoaderVersion);
        QString rollbackError;
        clearServerContents(serverDirectory, &rollbackError);
        if (!copyDirectoryContents(rollbackBackup.path, serverDirectory, &rollbackError,
                                   { BACKUP_MANIFEST }) && error) {
            *error = tr("Could not save restored server metadata. Automatic rollback also failed: %1")
                         .arg(rollbackError);
        } else if (error) {
            *error = tr("Could not save restored server metadata. The previous server state was restored.");
        }
        save();
        return false;
    }

    if (safetyBackupName) {
        *safetyBackupName = rollbackName;
    }
    emit serverChanged(id);
    return true;
}

bool ServerManager::deleteServerBackup(const QString &id, const QString &backupPath, QString *error)
{
    const auto server = m_servers.value(id);
    if (!server
        || (server->status() != ServerStatus::Stopped && server->status() != ServerStatus::Error)) {
        if (error) {
            *error = tr("Stop the selected server before deleting a backup.");
        }
        return false;
    }
    const QString backupRoot = QDir(server->serverDirectory()).filePath("backups");
    if (!QFileInfo(backupPath).isDir() || !pathIsWithin(backupRoot, backupPath)) {
        if (error) {
            *error = tr("The selected backup is outside this server's backup folder.");
        }
        return false;
    }
    if (!QDir(backupPath).removeRecursively()) {
        if (error) {
            *error = tr("The selected backup could not be removed.");
        }
        return false;
    }
    return true;
}

bool ServerManager::enforceServerBackupRetention(const QString &id, const QString &namePrefix,
                                                 int maximumBackups, QString *error)
{
    const auto server = m_servers.value(id);
    if (!server
        || (server->status() != ServerStatus::Stopped && server->status() != ServerStatus::Error)) {
        if (error) {
            *error = tr("Stop the selected server before pruning backups.");
        }
        return false;
    }
    if (namePrefix.trimmed().isEmpty() || maximumBackups < 0) {
        if (error) {
            *error = tr("The backup retention rule is invalid.");
        }
        return false;
    }
    if (maximumBackups == 0) {
        return true;
    }

    QList<ServerBackupInfo> matchingBackups;
    for (const ServerBackupInfo& backup : listServerBackups(id)) {
        if (backup.valid && backup.name.startsWith(namePrefix, Qt::CaseInsensitive)) {
            matchingBackups.append(backup);
        }
    }
    std::sort(matchingBackups.begin(), matchingBackups.end(),
              [](const ServerBackupInfo& left, const ServerBackupInfo& right) {
                  return left.createdAt > right.createdAt;
              });
    while (matchingBackups.size() > maximumBackups) {
        const ServerBackupInfo oldest = matchingBackups.takeLast();
        if (!deleteServerBackup(id, oldest.path, error)) {
            return false;
        }
    }
    return true;
}

std::shared_ptr<ServerInstance> ServerManager::getServer(const QString &id) const
{
    return m_servers.value(id);
}

QList<std::shared_ptr<ServerInstance>> ServerManager::getAllServers() const
{
    return m_servers.values();
}

bool ServerManager::save()
{
    QJsonObject json;
    QJsonArray serversArray;

    for (auto &server : m_servers) {
        serversArray.append(server->toJson());
    }

    json["servers"] = serversArray;
    json["version"] = 1;

    QSaveFile file(m_serversFile);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    if (file.write(QJsonDocument(json).toJson()) < 0) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool ServerManager::load()
{
    QFile file(m_serversFile);
    if (!file.exists()) {
        return true;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        return false;
    }

    QJsonObject json = doc.object();
    QJsonArray serversArray = json["servers"].toArray();

    m_servers.clear();

    for (const auto &value : serversArray) {
        QJsonObject serverJson = value.toObject();
        auto server = ServerInstance::fromJson(serverJson, m_dataDir);
        if (server) {
            m_servers[server->id()] = server;
        }
    }

    return true;
}

QString ServerManager::generateId() const
{
    // Use C locale to guarantee ASCII digits in the ID.
    // System locale (e.g. Arabic) may produce non-ASCII numerals that break Java paths.
    QString timestamp = QLocale::c().toString(QDateTime::currentDateTime(), "yyyyMMddhhmmss");
    QString random = QUuid::createUuid().toString().remove('{').remove('}').remove('-').left(8);
    return timestamp + "_" + random;
}
