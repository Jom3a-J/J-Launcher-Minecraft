// SPDX-License-Identifier: GPL-3.0-only

#include "ServerModpackTracking.h"

#include "ServerModpackInstaller.h"
#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "server/ServerPackCompatibility.h"
#include "server/ServerProperties.h"
#include "HardwareInfo.h"

#include "minecraft/mod/MetadataHandler.h"
#include "modplatform/ModIndex.h"
#include "modplatform/helpers/HashUtils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSettings>

#include <toml++/toml.h>

#include <algorithm>
#include <utility>

namespace ServerModpackTracking {

void importContentTracking(const QString &gameRoot,
                           const std::shared_ptr<ServerInstance> &server,
                           ServerDataStore &records)
{
    if (!server) return;
    const QFileInfoList installedFiles = QDir(server->modsDirectory()).entryInfoList(
        QStringList() << QStringLiteral("*.jar"), QDir::Files);
    const ContentMetadataIndex metadata = ServerModpackInstaller::loadContentMetadata(gameRoot);
    for (const QFileInfo &installed : installedFiles) {
        const QString source = ServerModpackInstaller::contentTrackingSource(
            gameRoot, metadata, installed.absoluteFilePath());
        if (!source.isEmpty()) {
            records.setValue(server->id(), ServerDataGroup::ContentSources, installed.fileName(), source);
        }
    }
}

}  // namespace ServerModpackTracking

QString ServerModpackInstaller::contentTrackingSource(
    const QString &gameRoot, const QString &installedFilePath)
{
    if (!QFileInfo(installedFilePath).isFile()) return {};
    return contentTrackingSource(gameRoot, loadContentMetadata(gameRoot), installedFilePath);
}

ContentMetadataIndex ServerModpackInstaller::loadContentMetadata(const QString &gameRoot)
{
    ContentMetadataIndex index;
    const QList<QDir> metadataDirectories{
        QDir(QDir(gameRoot).filePath(QStringLiteral("mods/.index"))),
        QDir(QDir(gameRoot).filePath(QStringLiteral("jarmods"))),
    };
    for (const QDir &indexDirectory : metadataDirectories) {
        for (const QString &entry : indexDirectory.entryList(
                 QStringList() << QStringLiteral("*.pw.toml"), QDir::Files)) {
            const auto metadata = Metadata::get(indexDirectory, entry);
            if (!metadata.isValid()) {
                continue;
            }
            const QString provider =
                metadata.provider == ModPlatform::ResourceProvider::MODRINTH
                ? QStringLiteral("modrinth") : QStringLiteral("curseforge");
            index[metadata.filename.toCaseFolded()].append({
                metadata.filename, metadata.hash, metadata.hash_format,
                QStringLiteral("%1:%2:%3").arg(provider, metadata.project_id.toString(),
                                               metadata.file_id.toString()) });
        }
    }
    return index;
}

QString ServerModpackInstaller::contentTrackingSource(
    const QString &gameRoot, const ContentMetadataIndex &metadata,
    const QString &installedFilePath)
{
    const QFileInfo installed(installedFilePath);
    if (!installed.isFile()) return {};

    const auto candidates = metadata.constFind(installed.fileName().toCaseFolded());
    if (candidates == metadata.constEnd()) return {};

    for (const ContentMetadataEntry &entry : *candidates) {
        bool matches = false;
        const Hashing::Algorithm algorithm =
            Hashing::algorithmFromString(entry.hashFormat.toLower());
        if (!entry.hash.isEmpty() && algorithm != Hashing::Algorithm::Unknown) {
            const QString actualHash = Hashing::hash(installed.absoluteFilePath(), algorithm);
            matches = !actualHash.isEmpty()
                && actualHash.compare(entry.hash, Qt::CaseInsensitive) == 0;
        } else {
            const QFileInfo sourceFile(
                QDir(gameRoot).filePath(QStringLiteral("mods/") + entry.filename));
            if (sourceFile.isFile() && sourceFile.size() == installed.size()) {
                const QString installedHash = Hashing::hash(
                    installed.absoluteFilePath(), Hashing::Algorithm::Sha1);
                const QString sourceHash = Hashing::hash(
                    sourceFile.absoluteFilePath(), Hashing::Algorithm::Sha1);
                matches = !installedHash.isEmpty() && installedHash == sourceHash;
            }
        }
        if (matches) {
            return entry.source;
        }
    }
    return {};
}

bool ServerModpackInstaller::markKnownClientOnlyFile(const QString &filePath)
{
    const QString hash = Hashing::hash(
        filePath, Hashing::Algorithm::Sha256).toLower();
    if (hash.isEmpty()) return false;
    QSettings settings;
    settings.setValue(
        QStringLiteral("ServerCompatibility/KnownClientOnlyHashes/") + hash,
        QFileInfo(filePath).fileName());
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool ServerModpackInstaller::isKnownClientOnlyFile(const QString &filePath)
{
    const QString hash = Hashing::hash(
        filePath, Hashing::Algorithm::Sha256).toLower();
    return !hash.isEmpty()
        && QSettings().contains(
            QStringLiteral("ServerCompatibility/KnownClientOnlyHashes/") + hash);
}

QStringList ServerModpackInstaller::knownClientOnlyHashes()
{
    const QString prefix = QStringLiteral("ServerCompatibility/KnownClientOnlyHashes/");
    QStringList hashes;
    const QSettings settings;
    for (const QString &key : settings.allKeys()) {
        if (key.startsWith(prefix)) {
            hashes.append(key.mid(prefix.size()).toLower());
        }
    }
    return hashes;
}
