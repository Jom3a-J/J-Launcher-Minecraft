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

// Blocker policy: only missing instances and unsafe paths block. Everything
// else derived from optional provider metadata is advisory: the installed
// profile (AuthoritativeInstalledProfile) wins and projection falls back to
// conservative local content. Downloaded-content hash mismatches verified at
// download time stay fatal in the download layer, not here.
void addBlocker(ServerPackCompatibilityReport &report, ServerPackIssueKind kind,
                const QString &message,
                ServerPackTrust trust = ServerPackTrust::AdvisoryProviderMetadata)
{
    report.state = ServerPackCompatibilityState::Incompatible;
    report.reasons.append(message);
    report.issues.append({ kind, ServerPackIssueSeverity::Blocker, trust, message });
}

void addAdvisory(ServerPackCompatibilityReport &report, ServerPackIssueKind kind,
                 const QString &message,
                 ServerPackTrust trust = ServerPackTrust::AdvisoryProviderMetadata)
{
    report.warnings.append(message);
    report.issues.append({ kind, ServerPackIssueSeverity::Advisory, trust, message });
}

void addProjectionAdvisory(ServerPackCompatibilityReport &report,
                           ServerPackIssueKind kind, const QString &message)
{
    if (!report.projectionWarnings.contains(message)) {
        report.projectionWarnings.append(message);
    }
    for (const auto &existing : report.issues) {
        if (existing.kind == kind && existing.message == message
            && existing.severity == ServerPackIssueSeverity::Advisory) {
            return;
        }
    }
    report.issues.append({ kind, ServerPackIssueSeverity::Advisory,
                           ServerPackTrust::AdvisoryProviderMetadata, message });
}

int serverSideRank(ServerPackFileSide side)
{
    switch (side) {
        case ServerPackFileSide::ServerOnly:
            return 3;
        case ServerPackFileSide::Universal:
            return 2;
        case ServerPackFileSide::ClientOnly:
            return 1;
        case ServerPackFileSide::Unknown:
            return 0;
    }
    return 0;
}

bool readJsonObject(const QString &path, const QString &description,
                    ServerPackCompatibilityReport &report, QJsonObject *object)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // Optional provider/installed documents: fall back to the installed
        // profile and local content. The installer blocks later only if the
        // actual server profile has no usable Minecraft/loader.
        addAdvisory(report, ServerPackIssueKind::UnreadableMetadata,
                    QObject::tr("Could not read %1; it was ignored and the installed pack profile will be used.")
                        .arg(description));
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        addAdvisory(report, ServerPackIssueKind::MalformedMetadata,
                    QObject::tr("The %1 is malformed; it was ignored and the installed pack profile will be used.")
                        .arg(description));
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
            // Content is already local; treat as unverified, never claim
            // integrity. Download-time hash mismatches remain fatal in the
            // download layer.
            addAdvisory(report, ServerPackIssueKind::UnverifiedIntegrity,
                        QObject::tr("The provider supplied an invalid %1 hash for %2; it is treated as unverified.")
                            .arg(algorithm, decision.path));
            return;
        }
        decision.integrity = ServerPackIntegrityState::ProviderHashAvailable;
        decision.hashAlgorithm = algorithm;
        decision.hashValue = value;
        decision.hashSource = source;
        return;
    }
    addAdvisory(report, ServerPackIssueKind::UnverifiedIntegrity,
                QObject::tr("No supported hash was supplied for %1; it is unverified.")
                    .arg(decision.path));
}

void addFile(ServerPackCompatibilityReport &report, const QString &path,
             ServerPackFileSide side, const QJsonObject *hashes = nullptr,
             const QString &hashSource = QString())
{
    const QString normalized = normalizedPath(path);
    if (normalized.isEmpty()) {
        addAdvisory(report, ServerPackIssueKind::InvalidFileEntry,
                    QObject::tr("The provider supplied an empty server file entry; it was skipped."));
        return;
    }
    if (!isSafeRelativePath(normalized)) {
        addBlocker(report, ServerPackIssueKind::UnsafePath,
                   QObject::tr("The provider supplied an unsafe server file path: %1")
                       .arg(path));
        return;
    }

    if (side == ServerPackFileSide::ClientOnly) {
        report.hasClientOnlyFileMetadata = true;
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
            // Side labels are advisory; retain the most server-capable label
            // regardless of metadata order. Unsafe paths still block above.
            const bool clientOnlyConflict = side == ServerPackFileSide::ClientOnly
                || iterator->side == ServerPackFileSide::ClientOnly;
            if (serverSideRank(side) > serverSideRank(iterator->side)) {
                iterator->side = side;
            }
            if (clientOnlyConflict) {
                addProjectionAdvisory(report,
                                      ServerPackIssueKind::CompatibilityUncertainty,
                                      QObject::tr(
                                          "%1 is marked both game-only and for servers. This does not block server creation.")
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
        addAdvisory(report, ServerPackIssueKind::UnreadableMetadata,
                    QObject::tr("Could not read the %1; it was ignored.")
                        .arg(description));
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
        addAdvisory(report, ServerPackIssueKind::MalformedMetadata,
                    QObject::tr("The installed component document has no valid component list; the installed pack profile will be used where available."),
                    ServerPackTrust::AuthoritativeInstalledProfile);
        return;
    }
    for (const QJsonValue &value : componentsValue.toArray()) {
        if (!value.isObject()) {
            addAdvisory(report, ServerPackIssueKind::MalformedMetadata,
                        QObject::tr("The installed component document contains an invalid component; it was skipped."),
                        ServerPackTrust::AuthoritativeInstalledProfile);
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
        addAdvisory(report, ServerPackIssueKind::UnsupportedGame,
                    QObject::tr("The Modrinth pack declares an unsupported game; it was ignored and the installed pack profile will be used."));
    }

    const QJsonValue dependenciesValue = document.value(QStringLiteral("dependencies"));
    if (!dependenciesValue.isObject()) {
        addAdvisory(report, ServerPackIssueKind::CompatibilityUncertainty,
                    QObject::tr(
                        "The Modrinth pack has no complete dependency metadata; compatibility is unverified."));
    } else {
        const QJsonObject dependencies = dependenciesValue.toObject();
        const QString minecraft = dependencies.value(QStringLiteral("minecraft")).toString();
        if (!minecraft.isEmpty()) {
            report.minecraftVersion = minecraft;
            report.minecraftVersionMetadataPresent = true;
        } else {
            addAdvisory(report, ServerPackIssueKind::CompatibilityUncertainty,
                        QObject::tr(
                            "The Modrinth pack does not declare a Minecraft version; compatibility is unverified."));
        }
        const QStringList loaderKeys{
            QStringLiteral("fabric-loader"), QStringLiteral("forge"),
            QStringLiteral("neoforge"), QStringLiteral("quilt-loader"),
        };
        QString declaredLoader;
        QStringList extraLoaders;
        for (const QString &key : loaderKeys) {
            const QString version = dependencies.value(key).toString();
            if (version.isEmpty()) {
                continue;
            }
            if (!declaredLoader.isEmpty()) {
                extraLoaders.append(key);
                continue;
            }
            declaredLoader = normalizedLoader(key);
            report.loaderType = declaredLoader;
            report.loaderVersion = version;
            report.loaderMetadataPresent = true;
        }
        if (!extraLoaders.isEmpty()) {
            addAdvisory(report, ServerPackIssueKind::MultipleLoaders,
                        QObject::tr("The Modrinth pack declares more than one loader (%1); only %2 was kept and the installed pack profile wins.")
                            .arg((QStringList{ declaredLoader } + extraLoaders).join(QStringLiteral(", ")),
                                 declaredLoader));
        }
        for (auto iterator = dependencies.constBegin(); iterator != dependencies.constEnd(); ++iterator) {
            if (iterator.key().contains(QStringLiteral("loader"), Qt::CaseInsensitive)
                && !loaderKeys.contains(iterator.key())) {
                addAdvisory(report, ServerPackIssueKind::UnknownLoader,
                            QObject::tr("The Modrinth pack declares an unknown loader: %1; it was ignored and the installed pack profile wins.")
                                .arg(iterator.key()));
            }
        }
    }

    const QJsonValue filesValue = document.value(QStringLiteral("files"));
    if (!filesValue.isArray()) {
        addAdvisory(report, ServerPackIssueKind::MissingFileList,
                    QObject::tr("The Modrinth pack has no valid file list; server content will be projected conservatively from local files."));
        return;
    }
    for (const QJsonValue &value : filesValue.toArray()) {
        if (!value.isObject()) {
            addAdvisory(report, ServerPackIssueKind::InvalidFileEntry,
                        QObject::tr("The Modrinth pack contains an invalid file entry; it was skipped."));
            continue;
        }
        const QJsonObject file = value.toObject();
        const QString pathValue = file.value(QStringLiteral("path")).toString();
        if (normalizedPath(pathValue).isEmpty()) {
            addAdvisory(report, ServerPackIssueKind::InvalidFileEntry,
                        QObject::tr("The Modrinth pack contains a file entry without a path; it was skipped."));
            continue;
        }
        const QJsonValue environmentValue = file.value(QStringLiteral("env"));
        ServerPackFileSide side = ServerPackFileSide::Unknown;
        if (!environmentValue.isUndefined()) {
            if (!environmentValue.isObject()) {
                addAdvisory(report, ServerPackIssueKind::InvalidFileEntry,
                            QObject::tr(
                                "The Modrinth environment metadata for %1 is not an object; its side is treated as unknown.")
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
                            addAdvisory(report, ServerPackIssueKind::InvalidFileEntry,
                                        QObject::tr(
                                            "The Modrinth environment value for %1 is not a string; its side is treated as unknown.")
                                            .arg(pathValue));
                            return QString();
                        }
                        const QString result = value.toString();
                        if (result != QStringLiteral("required")
                            && result != QStringLiteral("optional")
                            && result != QStringLiteral("unsupported")) {
                            valid = false;
                            addAdvisory(report, ServerPackIssueKind::InvalidFileEntry,
                                        QObject::tr(
                                            "The Modrinth environment value for %1 is invalid; its side is treated as unknown.")
                                            .arg(pathValue));
                        }
                        return result;
                    };
                    const QString client = readEnvironmentValue(QStringLiteral("client"));
                    const QString server = readEnvironmentValue(QStringLiteral("server"));
                    if (valid && client == QStringLiteral("unsupported")
                        && server == QStringLiteral("unsupported")) {
                        addAdvisory(report, ServerPackIssueKind::CompatibilityUncertainty,
                                    QObject::tr(
                                        "The Modrinth environment metadata for %1 marks the file unsupported on both sides; it is included conservatively as unknown.")
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
        addAdvisory(report, ServerPackIssueKind::UnsupportedGame,
                    QObject::tr("The CurseForge manifest is not a Minecraft modpack; it was ignored and the installed pack profile will be used."));
    }
    const QJsonObject minecraft = document.value(QStringLiteral("minecraft")).toObject();
    report.minecraftVersion = minecraft.value(QStringLiteral("version")).toString();
    if (report.minecraftVersion.isEmpty()) {
        addAdvisory(report, ServerPackIssueKind::MalformedMetadata,
                    QObject::tr("The CurseForge manifest does not declare a Minecraft version; the installed pack profile will be used."));
    } else {
        report.minecraftVersionMetadataPresent = true;
    }
    const QJsonValue loadersValue = minecraft.value(QStringLiteral("modLoaders"));
    if (!loadersValue.isArray()) {
        addAdvisory(report, ServerPackIssueKind::MalformedMetadata,
                    QObject::tr("The CurseForge manifest has no valid loader list; the installed pack profile will be used."));
        return;
    }
    QString declaredLoader;
    QStringList extraLoaders;
    for (const QJsonValue &value : loadersValue.toArray()) {
        if (!value.isObject()) {
            addAdvisory(report, ServerPackIssueKind::InvalidFileEntry,
                        QObject::tr("The CurseForge manifest contains an invalid loader entry; it was skipped."));
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
            addAdvisory(report, ServerPackIssueKind::UnknownLoader,
                        QObject::tr("The CurseForge manifest declares an unknown loader: %1; it was ignored.")
                            .arg(id));
            continue;
        }
        if (version.isEmpty()) {
            addAdvisory(report, ServerPackIssueKind::InvalidFileEntry,
                        QObject::tr(
                            "The CurseForge manifest does not declare a version for the %1 loader; it was skipped.")
                            .arg(loader));
            continue;
        }
        if (!declaredLoader.isEmpty()) {
            extraLoaders.append(id);
            continue;
        }
        declaredLoader = loader;
        report.loaderType = loader;
        report.loaderVersion = version;
        report.loaderMetadataPresent = true;
    }
    if (!extraLoaders.isEmpty()) {
        addAdvisory(report, ServerPackIssueKind::MultipleLoaders,
                    QObject::tr("The CurseForge manifest declares more than one loader; only %1 was kept and the installed pack profile wins.")
                        .arg(declaredLoader));
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
        addAdvisory(report, ServerPackIssueKind::MalformedMetadata,
                    QObject::tr("The FTB App metadata does not declare a Minecraft version; the installed pack profile will be used."));
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
            addAdvisory(report, ServerPackIssueKind::UnknownLoader,
                        QObject::tr("The FTB App metadata declares an unknown loader: %1; it was ignored and the installed pack profile wins.")
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
        addAdvisory(report, ServerPackIssueKind::CompatibilityUncertainty,
                    QObject::tr(
                        "Technic did not provide a Minecraft version; compatibility is unverified."));
    } else {
        report.minecraftVersionMetadataPresent = true;
    }
    const QJsonValue librariesValue = document.value(QStringLiteral("libraries"));
    if (!librariesValue.isArray()) {
        addAdvisory(report, ServerPackIssueKind::CompatibilityUncertainty,
                    QObject::tr("Technic did not provide loader metadata; the loader is unverified."));
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
        addAdvisory(report, ServerPackIssueKind::CompatibilityUncertainty,
                    QObject::tr(
                        "No provider compatibility metadata was found; server content is included only as an unverified projection."));
    }
    if (!report.sideMetadataPresent && !report.hasDedicatedServerPack) {
        addAdvisory(report, ServerPackIssueKind::CompatibilityUncertainty,
                    QObject::tr(
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
    if (report.hasDedicatedServerPack && report.hasClientOnlyFileMetadata) {
        addProjectionAdvisory(report, ServerPackIssueKind::CompatibilityUncertainty,
                              QObject::tr(
                                  "Some files are marked game-only. The supplied server pack is used as-is."));
    }
    constexpr qsizetype unknownSideSampleLimit = 5;
    for (qsizetype i = 0; i < std::min(unknownSideSampleLimit, unknownSidePaths.size()); ++i) {
        addAdvisory(report, ServerPackIssueKind::CompatibilityUncertainty,
                    QObject::tr(
                        "The server side of %1 is unknown; it was included conservatively and may need manual review.")
                        .arg(unknownSidePaths.at(i)));
    }
    if (unknownSidePaths.size() > unknownSideSampleLimit) {
        addAdvisory(report, ServerPackIssueKind::CompatibilityUncertainty,
                    QObject::tr(
                        "%1 additional server files have unknown side metadata; review the projection before launching.")
                        .arg(unknownSidePaths.size() - unknownSideSampleLimit));
    }
    report.unverifiedFileCount = unverifiedFiles;
    if (unverifiedFiles > 0) {
        addAdvisory(report, ServerPackIssueKind::UnverifiedIntegrity,
                    QObject::tr(
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
    // The installed profile always wins. Provider declarations that disagree
    // are ignored with an advisory, never a blocker.
    if (!report.minecraftVersion.isEmpty() && !minecraftVersion.trimmed().isEmpty()
        && report.minecraftVersion != minecraftVersion.trimmed()) {
        addAdvisory(report, ServerPackIssueKind::VersionMismatch,
                    QObject::tr(
                        "The provider declares Minecraft %1, but the installed pack uses Minecraft %2; using the installed version.")
                        .arg(report.minecraftVersion, minecraftVersion.trimmed()),
                    ServerPackTrust::AuthoritativeInstalledProfile);
    }
    const QString expectedLoader = normalizedLoader(loaderType);
    if (!report.loaderType.isEmpty() && !expectedLoader.isEmpty()
        && normalizedLoader(report.loaderType) != expectedLoader) {
        addAdvisory(report, ServerPackIssueKind::VersionMismatch,
                    QObject::tr(
                        "The provider declares the %1 loader, but the installed pack uses %2; using the installed loader.")
                        .arg(report.loaderType, expectedLoader),
                    ServerPackTrust::AuthoritativeInstalledProfile);
    }
    const QString providerLoaderVersion = normalizedLoaderVersion(
        report.loaderType, report.loaderVersion, report.minecraftVersion);
    const QString expectedLoaderVersion = normalizedLoaderVersion(
        expectedLoader, loaderVersion, minecraftVersion);
    if (!providerLoaderVersion.isEmpty() && !expectedLoaderVersion.isEmpty()
        && providerLoaderVersion != expectedLoaderVersion) {
        addAdvisory(report, ServerPackIssueKind::VersionMismatch,
                    QObject::tr(
                        "The provider declares loader version %1, but the installed pack uses %2; using the installed version.")
                        .arg(report.loaderVersion, loaderVersion.trimmed()),
                    ServerPackTrust::AuthoritativeInstalledProfile);
    }
    if (!report.loaderType.isEmpty() && !isRecognizedLoader(report.loaderType)) {
        addAdvisory(report, ServerPackIssueKind::UnknownLoader,
                    QObject::tr("The provider declares an unknown loader: %1; it was ignored and the installed pack profile wins.")
                        .arg(report.loaderType));
    }
}

}  // namespace

ServerPackCompatibilityReport inspectServerPack(const QString &instanceRoot)
{
    ServerPackCompatibilityReport report;
    if (instanceRoot.trimmed().isEmpty() || !QFileInfo(instanceRoot).isDir()) {
        addBlocker(report, ServerPackIssueKind::MissingInstance,
                   QObject::tr("The modpack instance directory is missing."),
                   ServerPackTrust::AuthoritativeInstalledProfile);
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
        // Deterministic priority: Modrinth > CurseForge > FTB App > Technic.
        // Only the first is parsed; the rest are ignored with a warning.
        QStringList present;
        if (hasModrinth) {
            present.append(QStringLiteral("Modrinth"));
        }
        if (hasCurseForge) {
            present.append(QStringLiteral("CurseForge"));
        }
        if (hasFtbApp) {
            present.append(QStringLiteral("FTB App"));
        }
        if (hasTechnic) {
            present.append(QStringLiteral("Technic"));
        }
        const QString winner = present.constFirst();
        addAdvisory(report, ServerPackIssueKind::ConflictingProviders,
                    QObject::tr("The installed pack contains conflicting provider metadata (%1); only %2 was used and the rest were ignored.")
                        .arg(present.join(QStringLiteral(", ")), winner));
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

QString serverPackIssueKindName(ServerPackIssueKind kind)
{
    switch (kind) {
        case ServerPackIssueKind::MissingInstance:
            return QStringLiteral("MissingInstance");
        case ServerPackIssueKind::UnsafePath:
            return QStringLiteral("UnsafePath");
        case ServerPackIssueKind::UnreadableMetadata:
            return QStringLiteral("UnreadableMetadata");
        case ServerPackIssueKind::MalformedMetadata:
            return QStringLiteral("MalformedMetadata");
        case ServerPackIssueKind::ConflictingProviders:
            return QStringLiteral("ConflictingProviders");
        case ServerPackIssueKind::UnsupportedGame:
            return QStringLiteral("UnsupportedGame");
        case ServerPackIssueKind::MissingFileList:
            return QStringLiteral("MissingFileList");
        case ServerPackIssueKind::InvalidFileEntry:
            return QStringLiteral("InvalidFileEntry");
        case ServerPackIssueKind::UnknownLoader:
            return QStringLiteral("UnknownLoader");
        case ServerPackIssueKind::MultipleLoaders:
            return QStringLiteral("MultipleLoaders");
        case ServerPackIssueKind::VersionMismatch:
            return QStringLiteral("VersionMismatch");
        case ServerPackIssueKind::UnverifiedIntegrity:
            return QStringLiteral("UnverifiedIntegrity");
        case ServerPackIssueKind::CompatibilityUncertainty:
            return QStringLiteral("CompatibilityUncertainty");
    }
    return QStringLiteral("Unknown");
}
