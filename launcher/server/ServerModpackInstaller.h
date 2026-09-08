#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

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
    QString error;
    bool hasDedicatedServerPack = false;
    ServerModpackFailureCategory failureCategory = ServerModpackFailureCategory::None;
    ServerModpackFailureStage failureStage = ServerModpackFailureStage::None;

    bool isValid() const { return !serverId.isEmpty() && error.isEmpty(); }
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

    static bool prepareContent(const QString &instanceRoot, const QString &gameRoot,
                               const QString &destination, QStringList *skippedClientFiles,
                               QString *error, QStringList *warnings = nullptr);
    static QString contentTrackingSource(const QString &gameRoot,
                                         const QString &installedFilePath);

    static ServerModpackInstallResult createMatchingServer(ServerManager *manager,
                                                           const MinecraftInstance &instance,
                                                           const QString &serverName);
    static ServerModpackInstallResult createMatchingServer(
        ServerManager *manager, const ServerModpackProfile &profile,
        const QString &instanceRoot, const QString &gameRoot,
        const QString &serverName, int providerRecommendationMiB = 0,
        quint64 totalRamMiB = 0);
};
