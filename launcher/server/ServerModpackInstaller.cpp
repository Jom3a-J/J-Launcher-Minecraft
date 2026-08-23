#include "ServerModpackInstaller.h"

#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "server/ServerPackCompatibility.h"

#include "archive/ArchiveReader.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/MetadataHandler.h"
#include "modplatform/ModIndex.h"
#include "modplatform/helpers/HashUtils.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>

namespace {
QString normalizedRelativePath(QString path)
{
    path = QDir::fromNativeSeparators(QDir::cleanPath(path.trimmed()));
    while (path.startsWith("./")) {
        path.remove(0, 2);
    }
    return path;
}

bool isSafeRelativePath(const QString &path)
{
    const QString normalized = normalizedRelativePath(path);
    return !normalized.isEmpty() && normalized != ".." && !normalized.startsWith("../")
        && !QDir::isAbsolutePath(normalized);
}

const QStringList &serverContentRoots()
{
    static const QStringList roots{
        QStringLiteral("mods"),
        QStringLiteral("config"),
        QStringLiteral("configureddefaults"),
        QStringLiteral("datapacks"),
        QStringLiteral("defaultconfigs"),
        QStringLiteral("ftbteambases"),
        QStringLiteral("global_packs"),
        QStringLiteral("kubejs"),
        QStringLiteral("openloader"),
        QStringLiteral("patchouli_books"),
        QStringLiteral("resources"),
        QStringLiteral("scripts"),
        QStringLiteral("structures"),
    };
    return roots;
}

bool hasServerContentRoot(const QString &path)
{
    const QDir directory(path);
    for (const QString &root : serverContentRoots()) {
        if (QFileInfo(directory.filePath(root)).isDir()) {
            return true;
        }
    }
    return false;
}

QString publishedServerPackRoot(const QString &instanceRoot, QString *error)
{
    const QString extractedRoot = QDir(instanceRoot).filePath(
        QStringLiteral("server-pack/server-files"));
    if (!QFileInfo(extractedRoot).isDir()) {
        if (error) {
            *error = QObject::tr("The published server pack was not extracted.");
        }
        return {};
    }
    if (hasServerContentRoot(extractedRoot)) {
        return extractedRoot;
    }

    const QDir base(extractedRoot);
    QList<QPair<int, QString>> candidates;
    QDirIterator iterator(extractedRoot, QDir::Dirs | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (!hasServerContentRoot(path)) {
            continue;
        }
        const QString relative = normalizedRelativePath(base.relativeFilePath(path));
        candidates.append({ relative.count(QLatin1Char('/')), path });
    }

    if (candidates.isEmpty()) {
        if (error) {
            *error = QObject::tr(
                "The published server pack contains no supported server content folders.");
        }
        return {};
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto &left, const auto &right) {
                  return left.first < right.first;
              });
    if (candidates.size() > 1 && candidates.at(0).first == candidates.at(1).first) {
        if (error) {
            *error = QObject::tr(
                "The published server pack has more than one possible content root.");
        }
        return {};
    }
    return candidates.constFirst().second;
}

bool copyFile(const QString &source, const QString &destination, QString *error)
{
    if (!QDir().mkpath(QFileInfo(destination).dir().absolutePath())) {
        if (error) {
            *error = QObject::tr("Could not create the destination folder for %1.")
                         .arg(QFileInfo(destination).fileName());
        }
        return false;
    }
    if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
        if (error) {
            *error = QObject::tr("Could not replace %1.").arg(QFileInfo(destination).fileName());
        }
        return false;
    }
    if (!QFile::copy(source, destination)) {
        if (error) {
            *error = QObject::tr("Could not copy %1.").arg(QFileInfo(source).fileName());
        }
        return false;
    }
    return true;
}

bool copyDirectory(const QString &sourceRoot, const QString &destinationRoot,
                   const QSet<QString> &excludedPaths, QString *error)
{
    const QDir source(sourceRoot);
    if (!source.exists()) {
        return true;
    }
    QDirIterator iterator(sourceRoot,
                          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo entry = iterator.fileInfo();
        const QString relative = normalizedRelativePath(source.relativeFilePath(entry.absoluteFilePath()));
        const QString serverRelative = normalizedRelativePath(
            QFileInfo(sourceRoot).fileName() + QLatin1Char('/') + relative);
        if (entry.isSymLink()) {
            if (error) {
                *error = QObject::tr("The modpack contains an unsafe symbolic link: %1.")
                             .arg(serverRelative);
            }
            return false;
        }
        if (excludedPaths.contains(serverRelative.toLower())
            || serverRelative.compare(QStringLiteral("mods/.index"), Qt::CaseInsensitive) == 0
            || serverRelative.startsWith(QStringLiteral("mods/.index/"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString destination = QDir(destinationRoot).filePath(serverRelative);
        if (entry.isDir()) {
            if (!QDir().mkpath(destination)) {
                if (error) {
                    *error = QObject::tr("Could not create the server folder %1.").arg(serverRelative);
                }
                return false;
            }
        } else if (entry.isFile() && !copyFile(entry.absoluteFilePath(), destination, error)) {
            return false;
        }
    }
    return true;
}

bool copyDirectoryContents(const QString &sourceRoot, const QString &destinationRoot,
                           QString *error)
{
    const QDir source(sourceRoot);
    if (!source.exists()) {
        return true;
    }
    QDirIterator iterator(sourceRoot,
                          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                              | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo entry = iterator.fileInfo();
        const QString relative = normalizedRelativePath(
            source.relativeFilePath(entry.absoluteFilePath()));
        if (!isSafeRelativePath(relative) || entry.isSymLink()) {
            if (error) {
                *error = QObject::tr("The server pack contains an unsafe path: %1.")
                             .arg(relative);
            }
            return false;
        }
        const QString destinationRelative =
            relative.compare(QStringLiteral("default-server.properties"),
                             Qt::CaseInsensitive) == 0
            ? QStringLiteral("server.properties")
            : relative;
        const QString destination = QDir(destinationRoot).filePath(destinationRelative);
        if (entry.isDir()) {
            if (!QDir().mkpath(destination)) {
                if (error) {
                    *error = QObject::tr("Could not create the server folder %1.")
                                 .arg(relative);
                }
                return false;
            }
        } else if (entry.isFile()
                   && !copyFile(entry.absoluteFilePath(), destination, error)) {
            return false;
        }
    }
    return true;
}

bool copyServerRootFiles(const QString &sourceRoot, const QString &destinationRoot,
                         QString *error)
{
    const QDir source(sourceRoot);
    const QDir destination(destinationRoot);

    QString propertiesSource = source.filePath(QStringLiteral("server.properties"));
    if (!QFileInfo(propertiesSource).isFile()) {
        propertiesSource = source.filePath(QStringLiteral("default-server.properties"));
    }
    if (QFileInfo(propertiesSource).isFile()
        && !copyFile(propertiesSource,
                     destination.filePath(QStringLiteral("server.properties")), error)) {
        return false;
    }

    const QString iconSource = source.filePath(QStringLiteral("server-icon.png"));
    return !QFileInfo(iconSource).isFile()
        || copyFile(iconSource,
                    destination.filePath(QStringLiteral("server-icon.png")), error);
}

bool readPathList(const QString &path, QSet<QString> *paths, QString *error)
{
    QFile file(path);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QObject::tr("Could not read the server compatibility manifest.");
        }
        return false;
    }
    while (!file.atEnd()) {
        const QString entry = normalizedRelativePath(
            QString::fromUtf8(file.readLine()));
        if (entry.isEmpty()) {
            continue;
        }
        if (!isSafeRelativePath(entry)) {
            if (error) {
                *error = QObject::tr(
                             "The server compatibility manifest contains an unsafe path: %1")
                             .arg(entry);
            }
            return false;
        }
        paths->insert(entry);
    }
    return true;
}

bool readModrinthRules(const QString &instanceRoot, const QString &gameRoot,
                       QSet<QString> *includedPaths, QSet<QString> *excludedPaths,
                       QStringList *skippedClientFiles, QString *error)
{
    const QString mrpackRoot = QDir(instanceRoot).filePath(QStringLiteral("mrpack"));
    QFile indexFile(QDir(mrpackRoot).filePath(QStringLiteral("modrinth.index.json")));
    if (!indexFile.exists()) {
        return true;
    }
    if (!indexFile.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QObject::tr("Could not read the installed Modrinth pack index.");
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(indexFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QObject::tr("The installed Modrinth pack index is invalid.");
        }
        return false;
    }

    for (const QJsonValue &value : document.object().value(QStringLiteral("files")).toArray()) {
        const QJsonObject file = value.toObject();
        const QString path = normalizedRelativePath(file.value(QStringLiteral("path")).toString());
        if (!isSafeRelativePath(path)) {
            if (error) {
                *error = QObject::tr("The installed Modrinth pack contains an unsafe path: %1")
                             .arg(path);
            }
            return false;
        }
        const QString serverSupport = file.value(QStringLiteral("env")).toObject()
                                           .value(QStringLiteral("server"))
                                           .toString();
        if (serverSupport == QStringLiteral("unsupported")) {
            excludedPaths->insert(path.toLower());
            if (skippedClientFiles) {
                skippedClientFiles->append(path);
            }
            continue;
        }
        includedPaths->insert(path);
        if (serverSupport == QStringLiteral("required")
            && !QFileInfo::exists(QDir(gameRoot).filePath(path))) {
            const QString cachedPath =
                QDir(mrpackRoot).filePath(QStringLiteral("server-files/") + path);
            if (QFileInfo::exists(cachedPath)) {
                continue;
            }
            if (error) {
                *error = QObject::tr(
                             "This pack has a required server-only file that the client download "
                             "does not contain: %1")
                             .arg(path);
            }
            return false;
        }
    }

    if (!readPathList(QDir(mrpackRoot).filePath(QStringLiteral("overrides.txt")),
                      includedPaths, error)) {
        return false;
    }

    QFile clientOverrides(QDir(mrpackRoot).filePath(QStringLiteral("client-overrides.txt")));
    if (clientOverrides.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!clientOverrides.atEnd()) {
            const QString path = normalizedRelativePath(
                QString::fromUtf8(clientOverrides.readLine()));
            if (!isSafeRelativePath(path)) {
                continue;
            }
            excludedPaths->insert(path.toLower());
            if (skippedClientFiles) {
                skippedClientFiles->append(path);
            }
        }
    }
    return true;
}

bool jarDeclaresClientOnly(const QString &path)
{
    MMCZip::ArchiveReader archive(path);
    if (const auto fabricMetadata = archive.goToFile(QStringLiteral("fabric.mod.json"))) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            fabricMetadata->readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            const QString environment = document.object()
                                            .value(QStringLiteral("environment"))
                                            .toString()
                                            .trimmed();
            if (environment.compare(QStringLiteral("client"), Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
    }

    // Older Forge metadata has an explicit clientSideOnly flag. Modern
    // displayTest values describe network-version checks, not physical side,
    // so they must not be treated as proof that a mod is client-only.
    for (const QString &metadataPath : {
             QStringLiteral("META-INF/mods.toml"),
             QStringLiteral("META-INF/neoforge.mods.toml") }) {
        MMCZip::ArchiveReader forgeArchive(path);
        if (const auto forgeMetadata = forgeArchive.goToFile(metadataPath)) {
            const QString contents = QString::fromUtf8(forgeMetadata->readAll());
            static const QRegularExpression clientOnlyExpression(
                QStringLiteral(R"((?im)^\s*clientSideOnly\s*=\s*true\s*(?:#.*)?$)"));
            if (clientOnlyExpression.match(contents).hasMatch()) {
                return true;
            }
        }
    }
    return false;
}

void excludeClientOnlyMod(const QString &filename, QSet<QString> *excludedPaths,
                          QStringList *skippedClientFiles)
{
    if (filename.isEmpty()) return;
    const QString relativePath = normalizedRelativePath(
        QStringLiteral("mods/") + filename);
    excludedPaths->insert(relativePath.toLower());
    if (skippedClientFiles) {
        skippedClientFiles->append(relativePath);
    }
}

void readDeclaredClientOnlyMods(const QString &gameRoot, QSet<QString> *excludedPaths,
                                QStringList *skippedClientFiles)
{
    // New downloads use mods/.index. Prism-compatible legacy instances use
    // jarmods. Both describe files installed in the same minecraft/mods folder.
    const QList<QDir> metadataDirectories{
        QDir(QDir(gameRoot).filePath(QStringLiteral("mods/.index"))),
        QDir(QDir(gameRoot).filePath(QStringLiteral("jarmods"))),
    };
    for (const QDir &indexDirectory : metadataDirectories) {
        for (const QString &entry : indexDirectory.entryList(
                 QStringList() << QStringLiteral("*.pw.toml"), QDir::Files)) {
            const auto metadata = Metadata::get(indexDirectory, entry);
            if (!metadata.isValid()
                || metadata.side != ModPlatform::SideType::ClientSide) {
                continue;
            }
            excludeClientOnlyMod(metadata.filename, excludedPaths,
                                 skippedClientFiles);
        }
    }

    const QDir modsDirectory(QDir(gameRoot).filePath(QStringLiteral("mods")));
    for (const QFileInfo &jar : modsDirectory.entryInfoList(
             QStringList() << QStringLiteral("*.jar"), QDir::Files)) {
        const QString relativePath = normalizedRelativePath(
            QStringLiteral("mods/") + jar.fileName());
        if (!excludedPaths->contains(relativePath.toLower())
            && jarDeclaresClientOnly(jar.absoluteFilePath())) {
            excludeClientOnlyMod(jar.fileName(), excludedPaths,
                                 skippedClientFiles);
        }
    }
}

void readServerPairClientOnlyFiles(const QString &instanceRoot,
                                   QSet<QString> *excludedPaths,
                                   QStringList *skippedClientFiles)
{
    QFile file(QDir(instanceRoot).filePath(
        QStringLiteral("server-pack/client-only.txt")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    while (!file.atEnd()) {
        const QString path =
            normalizedRelativePath(QString::fromUtf8(file.readLine()));
        if (!isSafeRelativePath(path)) {
            continue;
        }
        excludedPaths->insert(path.toLower());
        if (skippedClientFiles) {
            skippedClientFiles->append(path);
        }
    }
}

bool copyIncludedServerFiles(const QString &instanceRoot, const QString &gameRoot,
                             const QSet<QString> &includedPaths,
                             const QSet<QString> &excludedPaths,
                             const QString &destination, QString *error)
{
    QStringList sortedPaths(includedPaths.cbegin(), includedPaths.cend());
    sortedPaths.sort(Qt::CaseInsensitive);
    for (const QString &relativePath : sortedPaths) {
        if (excludedPaths.contains(relativePath.toLower())) {
            continue;
        }
        const QStringList candidates{
            QDir(instanceRoot).filePath(
                QStringLiteral("server-pack/server-files/") + relativePath),
            QDir(instanceRoot).filePath(
                QStringLiteral("mrpack/server-files/") + relativePath),
            QDir(gameRoot).filePath(relativePath),
        };
        QString source;
        for (const QString &candidate : candidates) {
            const QFileInfo info(candidate);
            if (info.isSymLink()) {
                if (error) {
                    *error = QObject::tr("The modpack contains an unsafe symbolic link: %1.")
                                 .arg(relativePath);
                }
                return false;
            }
            if (info.isFile()) {
                source = candidate;
                break;
            }
        }
        if (source.isEmpty()) {
            if (error) {
                *error = QObject::tr(
                             "A provider-declared server file is missing after download: %1")
                             .arg(relativePath);
            }
            return false;
        }
        const QString destinationPath =
            relativePath.compare(QStringLiteral("default-server.properties"),
                                 Qt::CaseInsensitive) == 0
            ? QStringLiteral("server.properties")
            : relativePath;
        if (!copyFile(source, QDir(destination).filePath(destinationPath), error)) {
            return false;
        }
    }
    return true;
}

void importContentTracking(const QString &gameRoot,
                           const std::shared_ptr<ServerInstance> &server)
{
    if (!server) return;
    QSettings settings;
    const QString sourcePrefix = QStringLiteral("ServerContentSources/%1/").arg(server->id());
    const QFileInfoList installedFiles = QDir(server->modsDirectory()).entryInfoList(
        QStringList() << QStringLiteral("*.jar"), QDir::Files);
    for (const QFileInfo &installed : installedFiles) {
        const QString source = ServerModpackInstaller::contentTrackingSource(
            gameRoot, installed.absoluteFilePath());
        if (!source.isEmpty()) {
            settings.setValue(sourcePrefix + installed.fileName(), source);
        }
    }
}
}

ServerModpackProfile ServerModpackInstaller::profileForVersions(
    const QString &minecraftVersion, const QString &fabricVersion,
    const QString &forgeVersion, const QString &neoForgeVersion,
    const QString &quiltVersion)
{
    ServerModpackProfile result;
    result.minecraftVersion = minecraftVersion.trimmed();
    if (result.minecraftVersion.isEmpty()) {
        result.error = QObject::tr("The modpack does not declare a Minecraft version.");
        return result;
    }

    const QList<QPair<QString, QString>> loaders{
        { QStringLiteral("fabric"), fabricVersion.trimmed() },
        { QStringLiteral("forge"), forgeVersion.trimmed() },
        { QStringLiteral("neoforge"), neoForgeVersion.trimmed() },
    };
    for (const auto &loader : loaders) {
        if (loader.second.isEmpty()) {
            continue;
        }
        if (!result.loaderType.isEmpty()) {
            result.error = QObject::tr("The modpack declares more than one server loader.");
            return result;
        }
        result.loaderType = loader.first;
        result.loaderVersion = loader.second;
    }
    if (!quiltVersion.trimmed().isEmpty()) {
        result.error = QObject::tr(
            "Quilt modpack servers are not supported by the current server runtime.");
        return result;
    }
    if (result.loaderType.isEmpty()) {
        result.error = QObject::tr(
            "The selected pack is not a supported Fabric, Forge, or NeoForge modpack.");
    }
    return result;
}

QString ServerModpackInstaller::contentTrackingSource(
    const QString &gameRoot, const QString &installedFilePath)
{
    const QFileInfo installed(installedFilePath);
    if (!installed.isFile()) return {};

    const QList<QDir> metadataDirectories{
        QDir(QDir(gameRoot).filePath(QStringLiteral("mods/.index"))),
        QDir(QDir(gameRoot).filePath(QStringLiteral("jarmods"))),
    };
    for (const QDir &indexDirectory : metadataDirectories) {
        for (const QString &entry : indexDirectory.entryList(
                 QStringList() << QStringLiteral("*.pw.toml"), QDir::Files)) {
            const auto metadata = Metadata::get(indexDirectory, entry);
            if (!metadata.isValid()
                || metadata.filename.compare(installed.fileName(), Qt::CaseInsensitive) != 0) {
                continue;
            }

            bool matches = false;
            const Hashing::Algorithm algorithm =
                Hashing::algorithmFromString(metadata.hash_format.toLower());
            if (!metadata.hash.isEmpty() && algorithm != Hashing::Algorithm::Unknown) {
                const QString actualHash = Hashing::hash(installed.absoluteFilePath(), algorithm);
                matches = !actualHash.isEmpty()
                    && actualHash.compare(metadata.hash, Qt::CaseInsensitive) == 0;
            } else {
                const QFileInfo sourceFile(
                    QDir(gameRoot).filePath(QStringLiteral("mods/") + metadata.filename));
                if (sourceFile.isFile() && sourceFile.size() == installed.size()) {
                    const QString installedHash = Hashing::hash(
                        installed.absoluteFilePath(), Hashing::Algorithm::Sha1);
                    const QString sourceHash = Hashing::hash(
                        sourceFile.absoluteFilePath(), Hashing::Algorithm::Sha1);
                    matches = !installedHash.isEmpty() && installedHash == sourceHash;
                }
            }
            if (!matches) continue;

            const QString provider =
                metadata.provider == ModPlatform::ResourceProvider::MODRINTH
                ? QStringLiteral("modrinth") : QStringLiteral("curseforge");
            return QStringLiteral("%1:%2:%3")
                .arg(provider, metadata.project_id.toString(),
                     metadata.file_id.toString());
        }
    }
    return {};
}

ServerModpackProfile ServerModpackInstaller::inspect(const MinecraftInstance &instance)
{
    const auto *profile = instance.getPackProfile();
    const auto resolvedProfile = profileForVersions(
        profile->getComponentVersion(QStringLiteral("net.minecraft")),
        profile->getComponentVersion(QStringLiteral("net.fabricmc.fabric-loader")),
        profile->getComponentVersion(QStringLiteral("net.minecraftforge")),
        profile->getComponentVersion(QStringLiteral("net.neoforged")),
        profile->getComponentVersion(QStringLiteral("org.quiltmc.quilt-loader")));
    if (resolvedProfile.isValid()) {
        return resolvedProfile;
    }

    // Provider installation can finish before the remote metadata resolver has
    // refreshed every component. The local component document is already
    // transactional and contains the exact versions selected by the provider,
    // so it is the reliable fallback for server pairing.
    const auto savedProfile = profileFromInstanceRoot(instance.instanceRoot());
    return savedProfile.isValid() ? savedProfile : resolvedProfile;
}

ServerModpackProfile ServerModpackInstaller::profileFromInstanceRoot(
    const QString &instanceRoot)
{
    QFile file(QDir(instanceRoot).filePath(QStringLiteral("mmc-pack.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        ServerModpackProfile result;
        result.error = QObject::tr("The installed modpack component file could not be read.");
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        ServerModpackProfile result;
        result.error = QObject::tr("The installed modpack component file is invalid.");
        return result;
    }

    QHash<QString, QString> versions;
    const QJsonArray components =
        document.object().value(QStringLiteral("components")).toArray();
    for (const QJsonValue &value : components) {
        const QJsonObject component = value.toObject();
        const QString uid = component.value(QStringLiteral("uid")).toString();
        QString version = component.value(QStringLiteral("version")).toString();
        if (version.isEmpty()) {
            version = component.value(QStringLiteral("cachedVersion")).toString();
        }
        if (!uid.isEmpty() && !version.isEmpty()) {
            versions.insert(uid, version);
        }
    }

    return profileForVersions(
        versions.value(QStringLiteral("net.minecraft")),
        versions.value(QStringLiteral("net.fabricmc.fabric-loader")),
        versions.value(QStringLiteral("net.minecraftforge")),
        versions.value(QStringLiteral("net.neoforged")),
        versions.value(QStringLiteral("org.quiltmc.quilt-loader")));
}

bool ServerModpackInstaller::prepareContent(const QString &instanceRoot,
                                            const QString &gameRoot,
                                            const QString &destination,
                                            QStringList *skippedClientFiles,
                                            QString *error)
{
    if (!QFileInfo(gameRoot).isDir()) {
        if (error) {
            *error = QObject::tr("The downloaded instance has no game directory.");
        }
        return false;
    }
    const auto compatibility = inspectServerPack(instanceRoot);
    if (compatibility.isIncompatible()) {
        if (error) {
            *error = serverPackCompatibilityDescription(compatibility);
        }
        return false;
    }
    if (!QDir().mkpath(destination)) {
        if (error) {
            *error = QObject::tr("Could not create temporary server content.");
        }
        return false;
    }

    QSet<QString> includedPaths;
    QSet<QString> excludedPaths;
    for (const auto &file : compatibility.files) {
        if (file.side == ServerPackFileSide::ClientOnly) {
            excludedPaths.insert(file.path.toLower());
            if (skippedClientFiles) {
                skippedClientFiles->append(file.path);
            }
        } else if (file.side == ServerPackFileSide::ServerOnly
                   || file.side == ServerPackFileSide::Universal
                   || file.side == ServerPackFileSide::Unknown) {
            includedPaths.insert(file.path);
        }
    }
    if (!readModrinthRules(instanceRoot, gameRoot, &includedPaths, &excludedPaths,
                           skippedClientFiles, error)) {
        return false;
    }
    if (!readPathList(
            QDir(instanceRoot).filePath(QStringLiteral("server-pack/include.txt")),
            &includedPaths, error)) {
        return false;
    }
    if (!readPathList(
            QDir(instanceRoot).filePath(QStringLiteral("flame/overrides.txt")),
            &includedPaths, error)) {
        return false;
    }
    readDeclaredClientOnlyMods(gameRoot, &excludedPaths, skippedClientFiles);
    readServerPairClientOnlyFiles(instanceRoot, &excludedPaths,
                                  skippedClientFiles);

    const bool hasPublishedServerPack = QFileInfo::exists(
        QDir(instanceRoot).filePath(
            QStringLiteral("server-pack/published-server-pack.txt")));
    if (hasPublishedServerPack) {
        const QString serverPackRoot = publishedServerPackRoot(instanceRoot, error);
        if (serverPackRoot.isEmpty()) {
            return false;
        }
        return copyDirectoryContents(serverPackRoot, destination, error);
    }

    if (!includedPaths.isEmpty()) {
        const bool copied = copyIncludedServerFiles(
            instanceRoot, gameRoot, includedPaths, excludedPaths, destination, error);
        if (copied && skippedClientFiles) {
            skippedClientFiles->removeDuplicates();
            skippedClientFiles->sort(Qt::CaseInsensitive);
        }
        return copied;
    }

    for (const QString &root : serverContentRoots()) {
        if (!copyDirectory(QDir(gameRoot).filePath(root), destination,
                           excludedPaths, error)) {
            return false;
        }
        if (!copyDirectory(
                QDir(instanceRoot).filePath(
                    QStringLiteral("mrpack/server-files/") + root),
                destination, excludedPaths, error)) {
            return false;
        }
        if (!copyDirectory(
                QDir(instanceRoot).filePath(
                    QStringLiteral("server-pack/server-files/") + root),
                destination, excludedPaths, error)) {
            return false;
        }
    }
    if (!copyServerRootFiles(gameRoot, destination, error)
        || !copyServerRootFiles(
            QDir(instanceRoot).filePath(QStringLiteral("mrpack/server-files")),
            destination, error)
        || !copyServerRootFiles(
            QDir(instanceRoot).filePath(QStringLiteral("server-pack/server-files")),
            destination, error)) {
        return false;
    }
    if (skippedClientFiles) {
        skippedClientFiles->removeDuplicates();
        skippedClientFiles->sort(Qt::CaseInsensitive);
    }
    return true;
}

ServerModpackInstallResult ServerModpackInstaller::createMatchingServer(
    ServerManager *manager, const MinecraftInstance &instance,
    const QString &serverName)
{
    return createMatchingServer(manager, inspect(instance), instance.instanceRoot(),
                                instance.gameRoot(),
                                serverName.trimmed().isEmpty()
                                    ? instance.name() + QObject::tr(" Server")
                                    : serverName);
}

ServerModpackInstallResult ServerModpackInstaller::createMatchingServer(
    ServerManager *manager, const ServerModpackProfile &profile,
    const QString &instanceRoot, const QString &gameRoot,
    const QString &serverName)
{
    ServerModpackInstallResult result;
    if (!manager) {
        result.error = QObject::tr("The server manager is not available.");
        return result;
    }
    if (!profile.isValid()) {
        result.error = profile.error;
        return result;
    }

    const auto compatibility = evaluateServerPack(
        instanceRoot, profile.minecraftVersion, profile.loaderType,
        profile.loaderVersion);
    if (compatibility.isIncompatible()) {
        result.error = serverPackCompatibilityDescription(compatibility);
        return result;
    }

    const bool hasPublishedServerPack = compatibility.hasDedicatedServerPack;
    if (!hasPublishedServerPack) {
        result.warnings.append(
            compatibility.sideMetadataPresent
                ? QObject::tr("The provider did not publish a dedicated server pack. "
                              "The server was derived from the client pack using the "
                              "available compatibility metadata.")
                : QObject::tr("The provider did not publish a dedicated server pack and "
                              "did not supply complete compatibility metadata. "
                              "The server projection is unverified and may need manual review."));
    }
    if (compatibility.state == ServerPackCompatibilityState::Unknown) {
        result.warnings.append(QObject::tr(
            "Server compatibility is unknown; client-only content could not be proven safe "
            "for a dedicated server."));
    }
    for (const QString &warning : compatibility.warnings) {
        if (!result.warnings.contains(warning)) {
            result.warnings.append(warning);
        }
    }

    QTemporaryDir staging;
    if (!staging.isValid()
        || !prepareContent(instanceRoot, gameRoot,
                           staging.path(), &result.skippedClientFiles, &result.error)) {
        if (result.error.isEmpty()) {
            result.error = QObject::tr("Could not prepare the modpack for the server.");
        }
        return result;
    }

    const auto server = manager->createServer(
        serverName.trimmed(),
        profile.minecraftVersion, profile.loaderType, profile.loaderVersion);
    if (!server) {
        result.error = QObject::tr("Could not create the matching server.");
        return result;
    }

    if (!copyDirectoryContents(staging.path(), server->serverDirectory(),
                               &result.error)) {
        manager->deleteServer(server->id());
        return result;
    }
    importContentTracking(gameRoot, server);
    result.serverId = server->id();
    return result;
}
