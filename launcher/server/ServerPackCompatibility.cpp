/* SPDX-License-Identifier: GPL-3.0-only */

#include "ServerPackCompatibility.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

#include <algorithm>

namespace {

QString normalizedPath(QString path)
{
    path = QDir::fromNativeSeparators(QDir::cleanPath(path.trimmed()));
    while (path.startsWith(QStringLiteral("./"))) {
        path.remove(0, 2);
    }
    return path;
}

bool isSafeRelativePath(const QString &path)
{
    const QString normalized = normalizedPath(path);
    return !normalized.isEmpty() && normalized != QStringLiteral("..")
        && !normalized.startsWith(QStringLiteral("../"))
        && !QDir::isAbsolutePath(normalized);
}

QString normalizedLoader(QString loader)
{
    loader = loader.trimmed().toLower();
    if (loader == QStringLiteral("net.fabricmc.fabric-loader")
        || loader == QStringLiteral("fabric-loader")) {
        return QStringLiteral("fabric");
    }
    if (loader == QStringLiteral("net.minecraftforge")) {
        return QStringLiteral("forge");
    }
    if (loader == QStringLiteral("net.neoforged")) {
        return QStringLiteral("neoforge");
    }
    if (loader == QStringLiteral("org.quiltmc.quilt-loader")
        || loader == QStringLiteral("quilt-loader")) {
        return QStringLiteral("quilt");
    }
    return loader;
}

QString normalizedLoaderVersion(const QString &loaderType, QString version,
                                const QString &minecraftVersion)
{
    // Technic exposes Forge/NeoForge Maven coordinates here; the other
    // supported adapters already persist the loader version used by MMC.
    version = version.trimmed();
    const QString loader = normalizedLoader(loaderType);
    if (loader != QStringLiteral("forge") && loader != QStringLiteral("neoforge")) {
        return version;
    }

    const QString minecraft = minecraftVersion.trimmed();
    const QString prefix = minecraft + QLatin1Char('-');
    if (minecraft.isEmpty() || !version.startsWith(prefix)) {
        return version;
    }
    version.remove(0, prefix.size());
    const QString suffix = QLatin1Char('-') + minecraft;
    if (version.endsWith(suffix)) {
        version.chop(suffix.size());
    }
    return version;
}

bool isRecognizedLoader(const QString &loader)
{
    static const QStringList knownLoaders{
        QStringLiteral("vanilla"), QStringLiteral("fabric"), QStringLiteral("forge"),
        QStringLiteral("neoforge"), QStringLiteral("quilt"), QStringLiteral("paper"),
        QStringLiteral("spigot"), QStringLiteral("bukkit"), QStringLiteral("purpur"),
        QStringLiteral("folia"),
    };
    return knownLoaders.contains(normalizedLoader(loader));
}

QString normalizedProvider(QString provider)
{
    provider = provider.trimmed().toLower();
    if (provider == QStringLiteral("curseforge") || provider == QStringLiteral("flame")) {
        return QStringLiteral("curseforge");
    }
    if (provider == QStringLiteral("ftbapp") || provider == QStringLiteral("ftb-app")) {
        return QStringLiteral("ftb-app");
    }
    if (provider == QStringLiteral("legacyftb") || provider == QStringLiteral("ftb-legacy")) {
        return QStringLiteral("ftb-legacy");
    }
    return provider;
}

void addReason(ServerPackCompatibilityReport &report, const QString &reason)
{
    report.state = ServerPackCompatibilityState::Incompatible;
    report.reasons.append(reason);
}

void addWarning(ServerPackCompatibilityReport &report, const QString &warning)
{
    report.warnings.append(warning);
}

bool readJsonObject(const QString &path, const QString &description,
                    ServerPackCompatibilityReport &report, QJsonObject *object)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        addReason(report, QObject::tr("Could not read %1.").arg(description));
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        addReason(report, QObject::tr("The %1 is malformed.").arg(description));
        return false;
    }
    *object = document.object();
    return true;
}

int hashLength(const QString &algorithm)
{
    if (algorithm == QStringLiteral("md5")) {
        return 32;
    }
    if (algorithm == QStringLiteral("sha1")) {
        return 40;
    }
    if (algorithm == QStringLiteral("sha256")) {
        return 64;
    }
    if (algorithm == QStringLiteral("sha512")) {
        return 128;
    }
    return 0;
}

bool validHash(const QString &algorithm, const QString &value)
{
    const int length = hashLength(algorithm);
    return length > 0 && value.size() == length
        && QRegularExpression(QStringLiteral("^[0-9a-fA-F]+$")).match(value).hasMatch();
}

void setHash(ServerPackFileDecision &decision, const QJsonObject &hashes,
             const QString &source, ServerPackCompatibilityReport &report)
{
    static const QStringList supportedAlgorithms{
        QStringLiteral("sha512"), QStringLiteral("sha256"), QStringLiteral("sha1"),
        QStringLiteral("md5"),
    };
    for (const QString &algorithm : supportedAlgorithms) {
        const QString value = hashes.value(algorithm).toString().trimmed();
        if (value.isEmpty()) {
            continue;
        }
        if (!validHash(algorithm, value)) {
            addReason(report, QObject::tr("The provider supplied an invalid %1 hash for %2.")
                                  .arg(algorithm, decision.path));
            return;
        }
        decision.integrity = ServerPackIntegrityState::ProviderHashAvailable;
        decision.hashAlgorithm = algorithm;
        decision.hashValue = value;
        decision.hashSource = source;
        return;
    }
    addWarning(report, QObject::tr("No supported hash was supplied for %1; it is unverified.")
                              .arg(decision.path));
}

void addFile(ServerPackCompatibilityReport &report, const QString &path,
             ServerPackFileSide side, const QJsonObject *hashes = nullptr,
             const QString &hashSource = QString())
{
    const QString normalized = normalizedPath(path);
    if (!isSafeRelativePath(normalized)) {
        addReason(report, QObject::tr("The provider supplied an unsafe server file path: %1")
                              .arg(path));
        return;
    }

    auto iterator = std::find_if(report.files.begin(), report.files.end(),
                                 [&normalized](const ServerPackFileDecision &file) {
                                     return file.path.compare(normalized, Qt::CaseInsensitive) == 0;
                                 });
    if (iterator == report.files.end()) {
        ServerPackFileDecision decision;
        decision.path = normalized;
        decision.side = side;
        if (hashes) {
            setHash(decision, *hashes, hashSource, report);
        }
        report.files.append(decision);
    } else {
        if (iterator->side == ServerPackFileSide::Unknown
            && side != ServerPackFileSide::Unknown) {
            iterator->side = side;
        } else if (side != ServerPackFileSide::Unknown
                   && iterator->side != ServerPackFileSide::Unknown
                   && iterator->side != side) {
            // A generic include list and the extracted server-files tree can
            // both describe the same server path. Keep the narrower, server-
            // only result deterministically; client-only conflicts remain
            // incompatible below.
            const bool universalServerOnly =
                (iterator->side == ServerPackFileSide::Universal
                 && side == ServerPackFileSide::ServerOnly)
                || (iterator->side == ServerPackFileSide::ServerOnly
                    && side == ServerPackFileSide::Universal);
            if (universalServerOnly) {
                iterator->side = ServerPackFileSide::ServerOnly;
            } else {
                addReason(report, QObject::tr(
                    "Conflicting server compatibility metadata was supplied for %1.")
                                      .arg(normalized));
            }
        }
        if (hashes && iterator->integrity == ServerPackIntegrityState::Unverified) {
            setHash(*iterator, *hashes, hashSource, report);
        }
    }
    report.sideMetadataPresent = report.sideMetadataPresent
        || side != ServerPackFileSide::Unknown;
}

void addPathList(ServerPackCompatibilityReport &report, const QString &path,
                 ServerPackFileSide side, const QString &description)
{
    QFile file(path);
    if (!file.exists()) {
        return;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        addReason(report, QObject::tr("Could not read the %1.").arg(description));
        return;
    }
    while (!file.atEnd()) {
        const QString entry = normalizedPath(QString::fromUtf8(file.readLine()));
        if (entry.isEmpty()) {
            continue;
        }
        addFile(report, entry, side);
    }
}

void addDirectoryFiles(ServerPackCompatibilityReport &report, const QString &root,
                       ServerPackFileSide side)
{
    if (!QFileInfo(root).isDir()) {
        return;
    }
    QDirIterator iterator(root, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden
                              | QDir::System, QDirIterator::Subdirectories);
    const QDir base(root);
    while (iterator.hasNext()) {
        const QString filePath = iterator.next();
        const QFileInfo info(filePath);
        addFile(report, base.relativeFilePath(info.absoluteFilePath()), side);
    }
}

void readComponentMetadata(const QString &instanceRoot,
                           ServerPackCompatibilityReport &report)
{
    const QString path = QDir(instanceRoot).filePath(QStringLiteral("mmc-pack.json"));
    if (!QFileInfo::exists(path)) {
        return;
    }

    QJsonObject document;
    if (!readJsonObject(path, QObject::tr("the installed component document"), report,
                        &document)) {
        return;
    }
    const QJsonValue componentsValue = document.value(QStringLiteral("components"));
    if (!componentsValue.isArray()) {
        addReason(report, QObject::tr("The installed component document has no valid component list."));
        return;
    }
    for (const QJsonValue &value : componentsValue.toArray()) {
        if (!value.isObject()) {
            addReason(report, QObject::tr("The installed component document contains an invalid component."));
            continue;
        }
        const QJsonObject component = value.toObject();
        const QString uid = component.value(QStringLiteral("uid")).toString();
        QString version = component.value(QStringLiteral("version")).toString();
        if (version.isEmpty()) {
            version = component.value(QStringLiteral("cachedVersion")).toString();
        }
        if (uid == QStringLiteral("net.minecraft")) {
            report.minecraftVersion = version;
        } else if (uid == QStringLiteral("net.fabricmc.fabric-loader")) {
            report.loaderType = QStringLiteral("fabric");
            report.loaderVersion = version;
        } else if (uid == QStringLiteral("net.minecraftforge")) {
            report.loaderType = QStringLiteral("forge");
            report.loaderVersion = version;
        } else if (uid == QStringLiteral("net.neoforged")) {
            report.loaderType = QStringLiteral("neoforge");
            report.loaderVersion = version;
        } else if (uid == QStringLiteral("org.quiltmc.quilt-loader")) {
            report.loaderType = QStringLiteral("quilt");
            report.loaderVersion = version;
        }
    }
}

void readModrinthMetadata(const QString &instanceRoot,
                          ServerPackCompatibilityReport &report)
{
    const QString path = QDir(instanceRoot).filePath(
        QStringLiteral("mrpack/modrinth.index.json"));
    if (!QFileInfo::exists(path)) {
        return;
    }
    report.provider = QStringLiteral("modrinth");
    report.providerMetadataPresent = true;

    QJsonObject document;
    if (!readJsonObject(path, QObject::tr("the Modrinth pack index"), report, &document)) {
        return;
    }
    if (document.value(QStringLiteral("game")).toString() != QStringLiteral("minecraft")) {
        addReason(report, QObject::tr("The Modrinth pack declares an unsupported game."));
    }

    const QJsonValue dependenciesValue = document.value(QStringLiteral("dependencies"));
    if (!dependenciesValue.isObject()) {
        addWarning(report, QObject::tr(
            "The Modrinth pack has no complete dependency metadata; compatibility is unverified."));
    } else {
        const QJsonObject dependencies = dependenciesValue.toObject();
        const QString minecraft = dependencies.value(QStringLiteral("minecraft")).toString();
        if (!minecraft.isEmpty()) {
            report.minecraftVersion = minecraft;
            report.minecraftVersionMetadataPresent = true;
        } else {
            addWarning(report, QObject::tr(
                "The Modrinth pack does not declare a Minecraft version; compatibility is unverified."));
        }
        const QStringList loaderKeys{
            QStringLiteral("fabric-loader"), QStringLiteral("forge"),
            QStringLiteral("neoforge"), QStringLiteral("quilt-loader"),
        };
        QString declaredLoader;
        for (const QString &key : loaderKeys) {
            const QString version = dependencies.value(key).toString();
            if (version.isEmpty()) {
                continue;
            }
            if (!declaredLoader.isEmpty()) {
                addReason(report, QObject::tr("The Modrinth pack declares more than one loader."));
                break;
            }
            declaredLoader = normalizedLoader(key);
            report.loaderType = declaredLoader;
            report.loaderVersion = version;
            report.loaderMetadataPresent = true;
        }
        for (auto iterator = dependencies.constBegin(); iterator != dependencies.constEnd(); ++iterator) {
            if (iterator.key().contains(QStringLiteral("loader"), Qt::CaseInsensitive)
                && !loaderKeys.contains(iterator.key())) {
                addReason(report, QObject::tr("The Modrinth pack declares an unknown loader: %1")
                                      .arg(iterator.key()));
            }
        }
    }

    const QJsonValue filesValue = document.value(QStringLiteral("files"));
    if (!filesValue.isArray()) {
        addReason(report, QObject::tr("The Modrinth pack has no valid file list."));
        return;
    }
    for (const QJsonValue &value : filesValue.toArray()) {
        if (!value.isObject()) {
            addReason(report, QObject::tr("The Modrinth pack contains an invalid file entry."));
            continue;
        }
        const QJsonObject file = value.toObject();
        const QString pathValue = file.value(QStringLiteral("path")).toString();
        const QJsonValue environmentValue = file.value(QStringLiteral("env"));
        ServerPackFileSide side = ServerPackFileSide::Unknown;
        if (!environmentValue.isUndefined()) {
            if (!environmentValue.isObject()) {
                addReason(report, QObject::tr(
                    "The Modrinth environment metadata for %1 is not an object.")
                                  .arg(pathValue));
            } else {
                const QJsonObject environment = environmentValue.toObject();
                if (!environment.isEmpty()) {
                    bool valid = true;
                    auto readEnvironmentValue = [&](const QString &key) {
                        const QJsonValue value = environment.value(key);
                        if (value.isUndefined()) {
                            return QStringLiteral("required");
                        }
                        if (!value.isString()) {
                            valid = false;
                            addReason(report, QObject::tr(
                                "The Modrinth environment value for %1 is not a string.")
                                                  .arg(pathValue));
                            return QString();
                        }
                        const QString result = value.toString();
                        if (result != QStringLiteral("required")
                            && result != QStringLiteral("optional")
                            && result != QStringLiteral("unsupported")) {
                            valid = false;
                            addReason(report, QObject::tr(
                                "The Modrinth environment value for %1 is invalid.")
                                                  .arg(pathValue));
                        }
                        return result;
                    };
                    const QString client = readEnvironmentValue(QStringLiteral("client"));
                    const QString server = readEnvironmentValue(QStringLiteral("server"));
                    if (valid && client == QStringLiteral("unsupported")
                        && server == QStringLiteral("unsupported")) {
                        addReason(report, QObject::tr(
                            "The Modrinth environment metadata for %1 marks the file unsupported on both sides.")
                                              .arg(pathValue));
                    } else if (valid && server == QStringLiteral("unsupported")) {
                        side = ServerPackFileSide::ClientOnly;
                    } else if (valid && client == QStringLiteral("unsupported")) {
                        side = ServerPackFileSide::ServerOnly;
                    } else if (valid) {
                        side = ServerPackFileSide::Universal;
                    }
                }
            }
        }
        const QJsonObject hashes = file.value(QStringLiteral("hashes")).toObject();
        addFile(report, pathValue, side, hashes.isEmpty() ? nullptr : &hashes,
                QStringLiteral("mrpack/modrinth.index.json"));
    }
}

void readCurseForgeMetadata(const QString &instanceRoot,
                            ServerPackCompatibilityReport &report)
{
    const QString path = QDir(instanceRoot).filePath(
        QStringLiteral("flame/manifest.json"));
    if (!QFileInfo::exists(path)) {
        return;
    }
    report.provider = QStringLiteral("curseforge");
    report.providerMetadataPresent = true;

    QJsonObject document;
    if (!readJsonObject(path, QObject::tr("the CurseForge pack manifest"), report,
                        &document)) {
        return;
    }
    if (document.value(QStringLiteral("manifestType")).toString()
        != QStringLiteral("minecraftModpack")) {
        addReason(report, QObject::tr("The CurseForge manifest is not a Minecraft modpack."));
    }
    const QJsonObject minecraft = document.value(QStringLiteral("minecraft")).toObject();
    report.minecraftVersion = minecraft.value(QStringLiteral("version")).toString();
    if (report.minecraftVersion.isEmpty()) {
        addReason(report, QObject::tr("The CurseForge manifest does not declare a Minecraft version."));
    } else {
        report.minecraftVersionMetadataPresent = true;
    }
    const QJsonValue loadersValue = minecraft.value(QStringLiteral("modLoaders"));
    if (!loadersValue.isArray()) {
        addReason(report, QObject::tr("The CurseForge manifest has no valid loader list."));
        return;
    }
    QString declaredLoader;
    for (const QJsonValue &value : loadersValue.toArray()) {
        if (!value.isObject()) {
            addReason(report, QObject::tr("The CurseForge manifest contains an invalid loader entry."));
            continue;
        }
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        QString loader;
        QString version;
        for (const QString &candidate : { QStringLiteral("neoforge-"),
                                          QStringLiteral("forge-"),
                                          QStringLiteral("fabric-"),
                                          QStringLiteral("quilt-") }) {
            if (id.startsWith(candidate)) {
                loader = candidate.left(candidate.size() - 1);
                version = id.mid(candidate.size());
                if (loader == QStringLiteral("neoforge")
                    && version.startsWith(report.minecraftVersion + QLatin1Char('-'))) {
                    version.remove(0, report.minecraftVersion.size() + 1);
                }
                break;
            }
        }
        if (loader.isEmpty()) {
            addReason(report, QObject::tr("The CurseForge manifest declares an unknown loader: %1")
                                  .arg(id));
            continue;
        }
        if (version.isEmpty()) {
            addReason(report, QObject::tr(
                "The CurseForge manifest does not declare a version for the %1 loader.")
                              .arg(loader));
            continue;
        }
        if (!declaredLoader.isEmpty()) {
            addReason(report, QObject::tr("The CurseForge manifest declares more than one loader."));
            continue;
        }
        declaredLoader = loader;
        report.loaderType = loader;
        report.loaderVersion = version;
        report.loaderMetadataPresent = true;
    }
}

void readFtbAppMetadata(const QString &instanceRoot,
                        ServerPackCompatibilityReport &report)
{
    const QString path = QDir(instanceRoot).filePath(
        QStringLiteral("minecraft/instance.json"));
    if (!QFileInfo::exists(path)) {
        return;
    }
    report.provider = QStringLiteral("ftb-app");
    report.providerMetadataPresent = true;

    QJsonObject document;
    if (!readJsonObject(path, QObject::tr("the FTB App instance metadata"), report,
                        &document)) {
        return;
    }
    report.minecraftVersion = document.value(QStringLiteral("mcVersion")).toString();
    if (report.minecraftVersion.isEmpty()) {
        addReason(report, QObject::tr("The FTB App metadata does not declare a Minecraft version."));
    } else {
        report.minecraftVersionMetadataPresent = true;
    }
    const QString modLoader = document.value(QStringLiteral("modLoader")).toString();
    if (!modLoader.isEmpty()) {
        const int separator = modLoader.indexOf(QLatin1Char('-'));
        const QString loader = separator < 0 ? modLoader : modLoader.left(separator);
        report.loaderType = normalizedLoader(loader);
        report.loaderVersion = separator < 0 ? QString() : modLoader.mid(separator + 1);
        report.loaderMetadataPresent = !report.loaderVersion.isEmpty();
        if (!isRecognizedLoader(report.loaderType)) {
            addReason(report, QObject::tr("The FTB App metadata declares an unknown loader: %1")
                                  .arg(loader));
        }
    }
}

void readTechnicMetadata(const QString &instanceRoot,
                         ServerPackCompatibilityReport &report)
{
    const QString path = QDir(instanceRoot).filePath(
        QStringLiteral("minecraft/bin/version.json"));
    if (!QFileInfo::exists(path)) {
        return;
    }
    report.provider = QStringLiteral("technic");
    report.providerMetadataPresent = true;

    QJsonObject document;
    if (!readJsonObject(path, QObject::tr("the Technic version metadata"), report,
                        &document)) {
        return;
    }
    report.minecraftVersion = document.value(QStringLiteral("inheritsFrom")).toString();
    if (report.minecraftVersion.isEmpty()) {
        addWarning(report, QObject::tr(
            "Technic did not provide a Minecraft version; compatibility is unverified."));
    } else {
        report.minecraftVersionMetadataPresent = true;
    }
    const QJsonValue librariesValue = document.value(QStringLiteral("libraries"));
    if (!librariesValue.isArray()) {
        addWarning(report, QObject::tr("Technic did not provide loader metadata; the loader is unverified."));
        return;
    }
    for (const QJsonValue &value : librariesValue.toArray()) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString();
        if (name.startsWith(QStringLiteral("net.minecraftforge:forge:"))) {
            report.loaderType = QStringLiteral("forge");
            report.loaderVersion = name.mid(QStringLiteral("net.minecraftforge:forge:").size());
        } else if (name.startsWith(QStringLiteral("net.fabricmc:fabric-loader:"))) {
            report.loaderType = QStringLiteral("fabric");
            report.loaderVersion = name.mid(QStringLiteral("net.fabricmc:fabric-loader:").size());
        } else if (name.startsWith(QStringLiteral("org.quiltmc:quilt-loader:"))) {
            report.loaderType = QStringLiteral("quilt");
            report.loaderVersion = name.mid(QStringLiteral("org.quiltmc:quilt-loader:").size());
        } else if (name.startsWith(QStringLiteral("net.neoforged:fancymodloader:"))) {
            report.loaderType = QStringLiteral("neoforge");
            report.loaderVersion = name.mid(QStringLiteral("net.neoforged:fancymodloader:").size());
        } else if (name.startsWith(QStringLiteral("net.neoforged:neoforge:"))) {
            report.loaderType = QStringLiteral("neoforge");
            report.loaderVersion = name.mid(QStringLiteral("net.neoforged:neoforge:").size());
        }
        if (!report.loaderType.isEmpty() && !report.loaderVersion.isEmpty()) {
            report.loaderMetadataPresent = true;
            break;
        }
    }
}

void readProviderMarker(const QString &instanceRoot,
                        ServerPackCompatibilityReport &report)
{
    const QStringList markerPaths{
        QDir(instanceRoot).filePath(QStringLiteral("server-pack/provider.txt")),
        QDir(instanceRoot).filePath(QStringLiteral("mrpack/provider.txt")),
    };
    for (const QString &path : markerPaths) {
        QFile marker(path);
        if (!marker.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        const QString provider = normalizedProvider(QString::fromUtf8(marker.readLine()));
        if (!provider.isEmpty()) {
            report.provider = provider;
        }
        return;
    }
}

void readServerPackMetadata(const QString &instanceRoot,
                            ServerPackCompatibilityReport &report)
{
    // FTB/ATLauncher/legacy FTB imports currently retain the normalized side
    // manifests, not their provider manifests. Their installer tasks validate
    // published hashes before staging, so this later server projection cannot
    // reconstruct the original algorithm/value and reports those files as
    // unverified unless a retained provider document supplies it.
    const QDir root(instanceRoot);
    const QString marker = root.filePath(QStringLiteral("server-pack/published-server-pack.txt"));
    if (QFileInfo(marker).isFile()) {
        report.hasDedicatedServerPack = true;
        if (report.provider.isEmpty()) {
            QFile file(marker);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                report.provider = normalizedProvider(QString::fromUtf8(file.readLine()));
            }
        }
    }

    addPathList(report, root.filePath(QStringLiteral("server-pack/client-only.txt")),
                ServerPackFileSide::ClientOnly,
                QObject::tr("the client-only compatibility list"));
    addPathList(report, root.filePath(QStringLiteral("server-pack/include.txt")),
                ServerPackFileSide::Universal,
                QObject::tr("the server compatibility list"));
    addPathList(report, root.filePath(QStringLiteral("server-pack/server-only.txt")),
                ServerPackFileSide::ServerOnly,
                QObject::tr("the server-only compatibility list"));
    addPathList(report, root.filePath(QStringLiteral("server-pack/unknown.txt")),
                ServerPackFileSide::Unknown,
                QObject::tr("the unknown-side compatibility list"));
    addDirectoryFiles(report, root.filePath(QStringLiteral("server-pack/server-files")),
                      ServerPackFileSide::ServerOnly);
    addDirectoryFiles(report, root.filePath(QStringLiteral("mrpack/server-files")),
                      ServerPackFileSide::ServerOnly);
}

void finalizeReport(ServerPackCompatibilityReport &report)
{
    report.provider = normalizedProvider(report.provider);
    if (report.provider.isEmpty()) {
        report.provider = QStringLiteral("local/custom");
    }
    if (!report.providerMetadataPresent && !report.sideMetadataPresent
        && !report.hasDedicatedServerPack) {
        addWarning(report, QObject::tr(
            "No provider compatibility metadata was found; server content is included only as an unverified projection."));
    }
    if (!report.sideMetadataPresent && !report.hasDedicatedServerPack) {
        addWarning(report, QObject::tr(
            "No client/server side metadata was retained; server content is included conservatively and remains unverified."));
    }
    QStringList unknownSidePaths;
    int unverifiedFiles = 0;
    for (const auto &file : report.files) {
        if (file.side == ServerPackFileSide::Unknown) {
            unknownSidePaths.append(file.path);
        }
        if (file.integrity == ServerPackIntegrityState::Unverified) {
            ++unverifiedFiles;
        }
    }
    constexpr qsizetype unknownSideSampleLimit = 5;
    for (qsizetype i = 0; i < std::min(unknownSideSampleLimit, unknownSidePaths.size()); ++i) {
        addWarning(report, QObject::tr(
            "The server side of %1 is unknown; it was included conservatively and may need manual review.")
                              .arg(unknownSidePaths.at(i)));
    }
    if (unknownSidePaths.size() > unknownSideSampleLimit) {
        addWarning(report, QObject::tr(
            "%1 additional server files have unknown side metadata; review the projection before launching.")
                              .arg(unknownSidePaths.size() - unknownSideSampleLimit));
    }
    if (unverifiedFiles > 0) {
        addWarning(report, QObject::tr(
            "%1 server file(s) have no supported provider hash and remain unverified.")
                              .arg(unverifiedFiles));
    }
    const bool hasDeclaredCompatibility = report.minecraftVersionMetadataPresent
        && report.loaderMetadataPresent
        && (report.hasDedicatedServerPack
            || (report.sideMetadataPresent && unknownSidePaths.isEmpty()));
    if (report.providerMetadataPresent && hasDeclaredCompatibility
        && !report.isIncompatible()) {
        report.state = ServerPackCompatibilityState::KnownCompatible;
    }
    if (!report.isIncompatible() && report.state != ServerPackCompatibilityState::KnownCompatible) {
        report.state = ServerPackCompatibilityState::Unknown;
    }
}

void compareExpected(ServerPackCompatibilityReport &report,
                     const QString &minecraftVersion, const QString &loaderType,
                     const QString &loaderVersion)
{
    if (!report.providerMetadataPresent || report.isIncompatible()) {
        return;
    }
    if (!report.minecraftVersion.isEmpty() && !minecraftVersion.trimmed().isEmpty()
        && report.minecraftVersion != minecraftVersion.trimmed()) {
        addReason(report, QObject::tr(
            "The provider declares Minecraft %1, but the installed pack uses Minecraft %2.")
                          .arg(report.minecraftVersion, minecraftVersion.trimmed()));
    }
    const QString expectedLoader = normalizedLoader(loaderType);
    if (!report.loaderType.isEmpty() && !expectedLoader.isEmpty()
        && normalizedLoader(report.loaderType) != expectedLoader) {
        addReason(report, QObject::tr(
            "The provider declares the %1 loader, but the installed pack uses %2.")
                          .arg(report.loaderType, expectedLoader));
    }
    const QString providerLoaderVersion = normalizedLoaderVersion(
        report.loaderType, report.loaderVersion, report.minecraftVersion);
    const QString expectedLoaderVersion = normalizedLoaderVersion(
        expectedLoader, loaderVersion, minecraftVersion);
    if (!providerLoaderVersion.isEmpty() && !expectedLoaderVersion.isEmpty()
        && providerLoaderVersion != expectedLoaderVersion) {
        addReason(report, QObject::tr(
            "The provider declares loader version %1, but the installed pack uses %2.")
                          .arg(report.loaderVersion, loaderVersion.trimmed()));
    }
    if (!report.loaderType.isEmpty() && !isRecognizedLoader(report.loaderType)) {
        addReason(report, QObject::tr("The provider declares an unknown loader: %1")
                              .arg(report.loaderType));
    }
}

}  // namespace

ServerPackCompatibilityReport inspectServerPack(const QString &instanceRoot)
{
    ServerPackCompatibilityReport report;
    if (instanceRoot.trimmed().isEmpty() || !QFileInfo(instanceRoot).isDir()) {
        addReason(report, QObject::tr("The modpack instance directory is missing."));
        return report;
    }

    readComponentMetadata(instanceRoot, report);
    readProviderMarker(instanceRoot, report);

    const bool hasModrinth = QFileInfo(
        QDir(instanceRoot).filePath(QStringLiteral("mrpack/modrinth.index.json"))).isFile();
    const bool hasCurseForge = QFileInfo(
        QDir(instanceRoot).filePath(QStringLiteral("flame/manifest.json"))).isFile();
    const bool hasFtbApp = QFileInfo(
        QDir(instanceRoot).filePath(QStringLiteral("minecraft/instance.json"))).isFile();
    const bool hasTechnic = QFileInfo(
        QDir(instanceRoot).filePath(QStringLiteral("minecraft/bin/version.json"))).isFile()
        || QFileInfo(QDir(instanceRoot).filePath(
            QStringLiteral("minecraft/bin/modpack.jar"))).isFile();
    const int providerDocuments = static_cast<int>(hasModrinth) + static_cast<int>(hasCurseForge)
        + static_cast<int>(hasFtbApp) + static_cast<int>(hasTechnic);
    if (providerDocuments > 1) {
        addReason(report, QObject::tr("The installed pack contains conflicting provider metadata."));
    }
    if (hasModrinth) {
        readModrinthMetadata(instanceRoot, report);
    } else if (hasCurseForge) {
        readCurseForgeMetadata(instanceRoot, report);
    } else if (hasFtbApp) {
        readFtbAppMetadata(instanceRoot, report);
    } else if (hasTechnic) {
        readTechnicMetadata(instanceRoot, report);
    }

    readServerPackMetadata(instanceRoot, report);
    finalizeReport(report);
    return report;
}

ServerPackCompatibilityReport evaluateServerPack(const QString &instanceRoot,
                                                  const QString &minecraftVersion,
                                                  const QString &loaderType,
                                                  const QString &loaderVersion)
{
    auto report = inspectServerPack(instanceRoot);
    compareExpected(report, minecraftVersion, loaderType, loaderVersion);
    if (!report.isIncompatible() && report.providerMetadataPresent
        && report.minecraftVersionMetadataPresent && report.loaderMetadataPresent
        && (report.hasDedicatedServerPack
            || (report.sideMetadataPresent
                && std::none_of(report.files.cbegin(), report.files.cend(),
                                [](const ServerPackFileDecision &file) {
                                    return file.side == ServerPackFileSide::Unknown;
                                })))) {
        report.state = ServerPackCompatibilityState::KnownCompatible;
    }
    return report;
}

QString serverPackCompatibilityDescription(const ServerPackCompatibilityReport &report)
{
    if (report.reasons.isEmpty()) {
        return {};
    }
    return report.reasons.join(QStringLiteral("\n"));
}
