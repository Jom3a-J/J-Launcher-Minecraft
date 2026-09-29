#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <memory>

class MinecraftInstance;
class ServerInstance;
class ServerManager;

enum class ServerModpackFailureCategory {
    None,
    InvalidProfile,
    CompatibilityMetadata,
    ContentProjection,
    DependencyIncompatibility,
    LauncherInternal,
};

enum class ServerModpackFailureStage {
    None,
    ProfileInspection,
    CompatibilityCheck,
    ContentPreparation,
    DependencyValidation,
    ServerCreation,
    ContentInstallation,
};

QString serverModpackFailureCategoryName(ServerModpackFailureCategory category);
QString serverModpackFailureStageName(ServerModpackFailureStage stage);

struct ServerModpackProfile {
    QString minecraftVersion;
    QString loaderType;
    QString loaderVersion;
    QString error;

    bool isValid() const { return error.isEmpty(); }
};

struct ServerModpackInstallResult {
    QString serverId;
    QString provider;
    QStringList skippedClientFiles;
    QStringList warnings;
    QStringList missingFiles;
    QStringList dependencyRequirements;
    QStringList missingDependencyIds;
    QString error;
    bool hasDedicatedServerPack = false;
    ServerModpackFailureCategory failureCategory = ServerModpackFailureCategory::None;
    ServerModpackFailureStage failureStage = ServerModpackFailureStage::None;

    bool isValid() const { return !serverId.isEmpty() && error.isEmpty(); }
};

struct PreparedServerModpack {
    ServerModpackInstallResult result;
    QString dependencyWarning;
    bool hasPublishedServerPack = false;
    ServerModpackProfile profile;
    QString instanceRoot;
    QString gameRoot;
    QString preparedDirectory;
    std::shared_ptr<QString> stagingDirectoryOwner;

    bool isReady() const { return result.error.isEmpty() && !preparedDirectory.isEmpty(); }
};

enum class ServerDependencyCheckState {
    Compatible,
    DefiniteFailure,
    Inconclusive,
    Unsafe,
};

struct ServerDependencyCheckResult {
    ServerDependencyCheckState state = ServerDependencyCheckState::Compatible;
    QString error;
    QStringList warnings;
    QStringList missingDependencyIds;

    bool isCompatible() const { return state == ServerDependencyCheckState::Compatible; }
};

class ServerModpackInstaller {
public:
    static ServerModpackProfile profileForVersions(const QString &minecraftVersion,
                                                   const QString &fabricVersion,
                                                   const QString &forgeVersion,
                                                   const QString &neoForgeVersion,
                                                   const QString &quiltVersion);
    static ServerModpackProfile inspect(const MinecraftInstance &instance);
    static ServerModpackProfile profileFromInstanceRoot(const QString &instanceRoot);
    static QString gameRootForInstanceRoot(const QString &instanceRoot);

    static bool prepareContent(const QString &instanceRoot, const QString &gameRoot,
                               const QString &destination, QStringList *skippedClientFiles,
                               QString *error, QStringList *warnings = nullptr,
                               const QString &publishedServerRoot = QString(),
                               QStringList *missingRequiredFiles = nullptr,
                               const QStringList &knownClientOnlyHashes = {},
                               bool knownClientOnlyHashesProvided = false);
    static QStringList knownClientOnlyHashes();
    static QStringList publishedServerRootChoices(const QString &instanceRoot);
    static ServerDependencyCheckResult checkServerDependencies(
        const QString &serverRoot, const QString &loaderType,
        const QString &minecraftVersion, const QString &loaderVersion);
    static QString contentTrackingSource(const QString &gameRoot,
                                         const QString &installedFilePath);
    static bool markKnownClientOnlyFile(const QString &filePath);
    static bool isKnownClientOnlyFile(const QString &filePath);

    static ServerModpackInstallResult createMatchingServer(ServerManager *manager,
                                                           const MinecraftInstance &instance,
                                                           const QString &serverName,
                                                           const QString &publishedServerRoot = QString());
    static ServerModpackInstallResult createMatchingServer(
        ServerManager *manager, const ServerModpackProfile &profile,
        const QString &instanceRoot, const QString &gameRoot,
        const QString &serverName, int providerRecommendationMiB = 0,
        quint64 totalRamMiB = 0,
        const QString &publishedServerRoot = QString());
    static PreparedServerModpack prepareMatchingServer(
        const ServerModpackProfile &profile, const QString &instanceRoot,
        const QString &gameRoot, const QString &stagingParent,
        const QString &publishedServerRoot = QString(),
        const QStringList &knownClientOnlyHashes = {});
    static ServerModpackInstallResult installPreparedServer(
        ServerManager *manager, PreparedServerModpack prepared,
        const QString &serverName, int providerRecommendationMiB = 0,
        quint64 totalRamMiB = 0);
};
