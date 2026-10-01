#include "ServerModpackInstaller.h"

#include "ServerModpackContent.h"
#include "ServerModpackTracking.h"
#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "server/ServerMemory.h"
#include "server/ServerPackCompatibility.h"
#include "server/ServerProperties.h"
#include "HardwareInfo.h"

#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/MetadataHandler.h"
#include "logs/Privacy.h"
#include "modplatform/ModIndex.h"
#include "modplatform/helpers/HashUtils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <toml++/toml.h>

#include <algorithm>
#include <utility>

QString serverModpackFailureCategoryName(ServerModpackFailureCategory category)
{
    switch (category) {
        case ServerModpackFailureCategory::None:
            return QObject::tr("None");
        case ServerModpackFailureCategory::InvalidProfile:
            return QObject::tr("Invalid modpack profile");
        case ServerModpackFailureCategory::CompatibilityMetadata:
            return QObject::tr("Provider compatibility metadata");
        case ServerModpackFailureCategory::ContentProjection:
            return QObject::tr("Server content projection");
        case ServerModpackFailureCategory::DependencyIncompatibility:
            return QObject::tr("Mod dependency incompatibility");
        case ServerModpackFailureCategory::LauncherInternal:
            return QObject::tr("Launcher failure");
    }
    return QObject::tr("Unknown");
}

QString serverModpackFailureStageName(ServerModpackFailureStage stage)
{
    switch (stage) {
        case ServerModpackFailureStage::None:
            return QObject::tr("None");
        case ServerModpackFailureStage::ProfileInspection:
            return QObject::tr("Profile inspection");
        case ServerModpackFailureStage::CompatibilityCheck:
            return QObject::tr("Compatibility check");
        case ServerModpackFailureStage::ContentPreparation:
            return QObject::tr("Server content preparation");
        case ServerModpackFailureStage::DependencyValidation:
            return QObject::tr("Dependency validation");
        case ServerModpackFailureStage::ServerCreation:
            return QObject::tr("Server creation");
        case ServerModpackFailureStage::ContentInstallation:
            return QObject::tr("Server content installation");
    }
    return QObject::tr("Unknown");
}

namespace {
struct PreparedDirectoryCleanup {
    QString path;
    ~PreparedDirectoryCleanup()
    {
        if (!path.isEmpty()) {
            QDir(path).removeRecursively();
        }
    }
};

void recordServerModpackFailure(ServerModpackInstallResult *result,
                                ServerModpackFailureCategory category,
                                ServerModpackFailureStage stage,
                                QString error)
{
    result->failureCategory = category;
    result->failureStage = stage;
    result->error = std::move(error);
    qWarning().noquote()
        << "Modpack server creation failed. Category:"
        << serverModpackFailureCategoryName(category)
        << "Stage:" << serverModpackFailureStageName(stage)
        << "Reason:" << Privacy::sanitizeText(result->error);
}
}  // namespace

using namespace ServerModpackContent;
using ServerModpackTracking::importContentTracking;

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

QString ServerModpackInstaller::gameRootForInstanceRoot(const QString &instanceRoot)
{
    // Same rule MinecraftInstance uses, so a pack that was never registered as
    // an instance still resolves to the folder its files actually landed in.
    const QFileInfo mcDir(QDir(instanceRoot).filePath(QStringLiteral("minecraft")));
    const QFileInfo dotMcDir(QDir(instanceRoot).filePath(QStringLiteral(".minecraft")));
    if (dotMcDir.exists() && !mcDir.exists()) {
        return dotMcDir.filePath();
    }
    return mcDir.filePath();
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

QStringList ServerModpackInstaller::publishedServerRootChoices(
    const QString &instanceRoot)
{
    return publishedServerPackRootChoicesInternal(instanceRoot);
}

ServerModpackInstallResult ServerModpackInstaller::createMatchingServer(
    ServerManager *manager, const MinecraftInstance &instance,
    const QString &serverName, const QString &publishedServerRoot)
{
    // Trust an explicit pack recommendation only when the source instance
    // truly overrides memory or carries an exported recommendation. The
    // global default must not count. Never parse startup scripts or command
    // text for memory flags.
    int providerRecommendation = 0;
    if (auto *settings = const_cast<MinecraftInstance &>(instance).settings()) {
        providerRecommendation = ServerMemory::resolveProviderRecommendation(
            settings->get("OverrideMemory").toBool(),
            settings->get("MaxMemAlloc").toInt(),
            settings->get("ExportRecommendedRAM").toInt());
    }
    return createMatchingServer(manager, inspect(instance), instance.instanceRoot(),
                                instance.gameRoot(),
                                serverName.trimmed().isEmpty()
                                    ? instance.name() + QObject::tr(" Server")
                                    : serverName,
                                providerRecommendation, 0, publishedServerRoot);
}

ServerModpackInstallResult ServerModpackInstaller::createMatchingServer(
    ServerManager *manager, const ServerModpackProfile &profile,
    const QString &instanceRoot, const QString &gameRoot,
    const QString &serverName, int providerRecommendationMiB,
    quint64 totalRamMiB, const QString &publishedServerRoot)
{
    if (!manager) {
        ServerModpackInstallResult result;
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::LauncherInternal,
            ServerModpackFailureStage::ServerCreation,
            QObject::tr("The server manager is not available."));
        return result;
    }
    PreparedServerModpack prepared = prepareMatchingServer(
        profile, instanceRoot, gameRoot, manager->serversRoot(), publishedServerRoot,
        knownClientOnlyHashes());
    return installPreparedServer(manager, std::move(prepared), serverName,
                                  providerRecommendationMiB, totalRamMiB);
}

PreparedServerModpack ServerModpackInstaller::prepareMatchingServer(
    const ServerModpackProfile &profile, const QString &instanceRoot,
    const QString &gameRoot, const QString &stagingParent,
    const QString &publishedServerRoot,
    const QStringList &knownClientOnlyHashes)
{
    PreparedServerModpack prepared;
    prepared.profile = profile;
    prepared.instanceRoot = instanceRoot;
    prepared.gameRoot = gameRoot;
    ServerModpackInstallResult &result = prepared.result;
    if (!profile.isValid()) {
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::InvalidProfile,
            ServerModpackFailureStage::ProfileInspection, profile.error);
        return prepared;
    }

    const auto compatibility = evaluateServerPack(
        instanceRoot, profile.minecraftVersion, profile.loaderType,
        profile.loaderVersion);
    result.provider = compatibility.provider;
    result.hasDedicatedServerPack = compatibility.hasDedicatedServerPack;
    if (compatibility.isIncompatible()) {
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::CompatibilityMetadata,
            ServerModpackFailureStage::CompatibilityCheck,
            serverPackCompatibilityDescription(compatibility));
        return prepared;
    }

    const bool hasPublishedServerPack = compatibility.hasDedicatedServerPack;
    prepared.hasPublishedServerPack = hasPublishedServerPack;
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
    // Surface advisory provider diagnostics without string matching: the
    // installed profile already won, these only explain what was ignored.
    // Integrity is aggregated by count below to avoid one warning per file.
    // Published packs are authoritative byte-identical content; their missing
    // hashes are expected and stay silent (the loader validates at startup).
    for (const ServerPackIssue &issue : compatibility.issues) {
        if (issue.severity != ServerPackIssueSeverity::Advisory) {
            continue;
        }
        if (issue.kind == ServerPackIssueKind::UnverifiedIntegrity) {
            continue;
        }
        if (!result.warnings.contains(issue.message)) {
            result.warnings.append(issue.message);
        }
    }
    if (!hasPublishedServerPack && compatibility.unverifiedFileCount > 0) {
        const QString integritySummary = QObject::tr(
            "%1 server file(s) have no supported provider hash and remain unverified.")
                                             .arg(compatibility.unverifiedFileCount);
        if (!result.warnings.contains(integritySummary)) {
            result.warnings.append(integritySummary);
        }
    }
    for (const QString &warning : compatibility.projectionWarnings) {
        if (!result.warnings.contains(warning)) {
            result.warnings.append(warning);
        }
    }
    if (compatibility.hasAnyAdvisory() && compatibility.providerMetadataPresent) {
        const QString installedSummary = QObject::tr(
            "Using installed Minecraft %1 with %2 %3; provider values that disagreed were ignored. "
            "The server was created stopped and may still fail to start if third-party content is broken.")
                                             .arg(profile.minecraftVersion, profile.loaderType,
                                                  profile.loaderVersion);
        if (!result.warnings.contains(installedSummary)) {
            result.warnings.append(installedSummary);
        }
    }

    if (!QDir().mkpath(stagingParent)) {
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::ContentProjection,
            ServerModpackFailureStage::ContentPreparation,
            QObject::tr("Could not prepare the modpack for the server."));
        return prepared;
    }
    const QString stagingPath = QDir(stagingParent).filePath(
        QStringLiteral(".preparing-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!QDir().mkpath(stagingPath)) {
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::ContentProjection,
            ServerModpackFailureStage::ContentPreparation,
            QObject::tr("Could not prepare the modpack for the server."));
        return prepared;
    }
    prepared.stagingDirectoryOwner = std::shared_ptr<QString>(
        new QString(stagingPath), [](QString *path) {
            QDir(*path).removeRecursively();
            delete path;
        });
    prepared.preparedDirectory = stagingPath;
    QString preparationError;
    if (!prepareContent(instanceRoot, gameRoot, stagingPath,
                           &result.skippedClientFiles, &preparationError,
                           &result.warnings, publishedServerRoot,
                           &result.missingFiles, knownClientOnlyHashes, true)) {
        if (preparationError.isEmpty()) {
            preparationError = QObject::tr("Could not prepare the modpack for the server.");
        }
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::ContentProjection,
            ServerModpackFailureStage::ContentPreparation,
            preparationError);
        return prepared;
    }
    if (!hasUsablePreparedContent(stagingPath)) {
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::ContentProjection,
            ServerModpackFailureStage::ContentPreparation,
            QObject::tr("The modpack contains no usable server content to create."));
        return prepared;
    }
    const ServerDependencyCheckResult dependencyCheck = checkServerDependencies(
        stagingPath, profile.loaderType, profile.minecraftVersion,
        profile.loaderVersion);
    result.missingDependencyIds = dependencyCheck.missingDependencyIds;
    for (const QString &warning : dependencyCheck.warnings) {
        if (!result.warnings.contains(warning)) result.warnings.append(warning);
    }
    QString dependencyWarning;
    if (!dependencyCheck.isCompatible()) {
        if (dependencyCheck.state == ServerDependencyCheckState::Unsafe) {
            recordServerModpackFailure(
                &result, ServerModpackFailureCategory::DependencyIncompatibility,
                ServerModpackFailureStage::DependencyValidation,
                dependencyCheck.error);
            return prepared;
        }
        if (!dependencyCheck.error.isEmpty()) {
            result.dependencyRequirements.append(dependencyCheck.error);
        }
        if (hasPublishedServerPack) {
            // Published packs are authoritative; the loader validates at startup.
            dependencyWarning = QObject::tr(
                "The launcher could not confirm all dependencies in the published server pack. "
                "The server was created; the %1 loader will check them at startup.\n\nDetails: %2")
                                    .arg(profile.loaderType, dependencyCheck.error);
        } else if (dependencyCheck.state == ServerDependencyCheckState::Inconclusive) {
            dependencyWarning = QObject::tr(
                "The launcher could not fully verify dependencies in the derived server copy. "
                "The server was created stopped, but it may fail to start; the %1 loader will check them at startup.\n\nDetails: %2")
                                    .arg(profile.loaderType, dependencyCheck.error);
        } else {
            dependencyWarning = QObject::tr(
                "The derived server is missing required content. It was created stopped so you can add the missing dependency.\n\nDetails: %1")
                                    .arg(dependencyCheck.error);
        }
    }

    prepared.dependencyWarning = dependencyWarning;
    return prepared;
}

ServerModpackInstallResult ServerModpackInstaller::installPreparedServer(
    ServerManager *manager, PreparedServerModpack prepared,
    const QString &serverName, int providerRecommendationMiB,
    quint64 totalRamMiB)
{
    const PreparedDirectoryCleanup cleanup{ prepared.preparedDirectory };
    Q_UNUSED(cleanup);
    if (!prepared.result.error.isEmpty()) {
        return std::move(prepared.result);
    }
    if (!manager) {
        ServerModpackInstallResult result = std::move(prepared.result);
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::LauncherInternal,
            ServerModpackFailureStage::ServerCreation,
            QObject::tr("The server manager is not available."));
        return result;
    }
    if (!prepared.isReady()) {
        ServerModpackInstallResult result = std::move(prepared.result);
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::LauncherInternal,
            ServerModpackFailureStage::ServerCreation,
            QObject::tr("The prepared server content is not available."));
        return result;
    }
    ServerModpackInstallResult result = std::move(prepared.result);
    const ServerModpackProfile &profile = prepared.profile;
    const QString &instanceRoot = prepared.instanceRoot;
    const QString &gameRoot = prepared.gameRoot;
    const bool hasPublishedServerPack = prepared.hasPublishedServerPack;
    const QString &dependencyWarning = prepared.dependencyWarning;

    const auto server = manager->createServer(
        serverName.trimmed(),
        profile.minecraftVersion, profile.loaderType, profile.loaderVersion);
    if (!server) {
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::LauncherInternal,
            ServerModpackFailureStage::ServerCreation,
            QObject::tr("Could not create the matching server."));
        return result;
    }

    QString installationError;
    QDir(server->serverDirectory()).removeRecursively();
    const bool moved = QDir().rename(prepared.preparedDirectory,
                                     server->serverDirectory());
    if (!moved && !copyDirectoryContents(prepared.preparedDirectory,
                                         server->serverDirectory(),
                                         &installationError)) {
        manager->deleteServer(server->id());
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::ContentProjection,
            ServerModpackFailureStage::ContentInstallation,
            installationError);
        return result;
    }
    if (!applyServerPropertyOverrides(instanceRoot, server,
                                      &installationError)) {
        manager->deleteServer(server->id());
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::ContentProjection,
            ServerModpackFailureStage::ContentInstallation,
            installationError);
        return result;
    }
    if (!hasPublishedServerPack
        && !writeDerivedServerMarker(server->serverDirectory(),
                                     &installationError)) {
        manager->deleteServer(server->id());
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::LauncherInternal,
            ServerModpackFailureStage::ContentInstallation,
            installationError);
        return result;
    }
    importContentTracking(gameRoot, server, manager->dataStore());
    // Size memory only now that the prepared server content is known, and
    // only for this newly created server. Existing servers are never
    // touched. Persist through the manager so save/reload keeps the values.
    {
        const quint64 hostRamMiB =
            totalRamMiB > 0 ? totalRamMiB : HardwareInfo::totalRamMiB();
        const int deployedJars =
            ServerMemory::countDeployedServerJars(server->serverDirectory(),
                                                  profile.loaderType);
        const ServerMemoryRecommendation memory = ServerMemory::recommend(
            profile.loaderType, deployedJars, hostRamMiB,
            providerRecommendationMiB);
        server->setMinMemory(memory.minMemoryMiB);
        server->setMaxMemory(memory.maxMemoryMiB);
        if (!manager->save()) {
            const QString saveError =
                QObject::tr("Could not save the new server's memory settings.");
            manager->deleteServer(server->id());
            recordServerModpackFailure(
                &result, ServerModpackFailureCategory::LauncherInternal,
                ServerModpackFailureStage::ServerCreation, saveError);
            return result;
        }
    }
    const QString setupIssue = ServerProperties::worldSetupIssue(server->serverDirectory());
    if (!setupIssue.isEmpty()) {
        result.warnings.append(setupIssue);
    }
    if (!dependencyWarning.isEmpty()) {
        result.warnings.append(dependencyWarning);
    }
    result.serverId = server->id();
    return result;
}
