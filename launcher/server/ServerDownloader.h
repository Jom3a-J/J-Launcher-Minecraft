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

#pragma once

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QStringList>
#include <QUrl>
#include <memory>

struct ServerProviderEndpoints
{
    QUrl vanillaManifest;
    QUrl paperApiBase;
    QUrl fabricApiBase;
    QUrl purpurApiBase;
    QUrl forgePromotions;
    QUrl forgeMavenBase;
    QUrl neoForgeVersions;
    QUrl neoForgeMavenBase;

    static ServerProviderEndpoints production();
};

class ServerDownloader : public QObject
{
    Q_OBJECT

public:
    enum class VersionChannel {
        Release,
        Snapshot,
        Beta
    };

    explicit ServerDownloader(QObject *parent = nullptr);
    explicit ServerDownloader(const QUrl &vanillaManifestUrl, QObject *parent = nullptr);
    explicit ServerDownloader(const ServerProviderEndpoints &endpoints, QObject *parent = nullptr);
    ~ServerDownloader();

    void startDownload(const QString &version, const QString &type, const QString &destinationDir,
                       const QString &javaPath = QString(),
                       const QString &loaderVersion = QString());

    // Fetch versions actually published by the selected server provider.
    void fetchAvailableVersions(const QString &type = QStringLiteral("vanilla"));
    // Fetch provider build/loader versions for one Minecraft version.
    void fetchAvailableBuilds(const QString &version, const QString &type);
    void cancel();

    // Supported server types
    static QStringList supportedTypes();
    static QStringList parseAvailableVersions(const QString &type, const QByteArray &data,
                                              QString *errorMessage = nullptr);
    static QStringList parseAvailableBuilds(const QString &type, const QString &version,
                                            const QByteArray &data,
                                            QString *errorMessage = nullptr);
    static VersionChannel versionChannel(const QString &version);

signals:
    void progress(int percentage);
    void statusMessage(const QString &message);
    void finished(bool success, const QString &errorMessage = QString());
    void versionsReady(const QStringList &versions);
    void versionsFailed(const QString &errorMessage);
    void buildsReady(const QStringList &builds);
    void buildsFailed(const QString &errorMessage);

private:
    // Network helpers
    QNetworkRequest createRequest(const QUrl &url);
    void downloadFile(const QString &url, const QString &outputPath,
                      const QByteArray &expectedHash = QByteArray(),
                      QCryptographicHash::Algorithm hashAlgorithm = QCryptographicHash::Sha256);
    bool appendDownloadData(const QByteArray &data);
    bool finalizeDownloadedFile(QString *errorMessage);
    void cleanUp();

    // Generic reply handler
    void handleReply(QNetworkReply *reply);

    // Vanilla
    void fetchVanillaManifest();
    void onVanillaManifestFetched(const QByteArray &data);
    void fetchVanillaVersionJson(const QString &url);
    void onVanillaVersionJsonFetched(const QByteArray &data);

    // Paper (v3 API — fill.papermc.io)
    void fetchPaperBuilds();
    void onPaperBuildsFetched(const QByteArray &data);

    // Fabric
    void fetchFabricInstaller();
    void onFabricInstallerFetched(const QByteArray &data);
    void fetchFabricLoader(const QString &installerVer);
    void onFabricLoaderFetched(const QString &installerVer, const QByteArray &data);

    // Purpur
    void fetchPurpurBuilds();
    void onPurpurBuildsFetched(const QByteArray &data);

    // Forge
    void fetchForgeVersions();
    void onForgeVersionsFetched(const QByteArray &data);
    void downloadForgeInstaller(const QString &forgeVersion);
    void onForgeInstallerDownloaded();

    // NeoForge
    void fetchNeoForgeVersions();
    void onNeoForgeVersionsFetched(const QByteArray &data);
    void downloadNeoForgeInstaller(const QString &neoForgeVersion);
    void onNeoForgeInstallerDownloaded();

    // Version manifest fetching
    void onVersionManifestFetched(const QByteArray &data);
    void onBuildManifestFetched(const QByteArray &data);
    bool isVersionListStep() const;
    bool isBuildListStep() const;
    void failCurrentRequest(const QString &message);
    QString currentFailureContext() const;

    enum class Step {
        Idle,
        // Version listing
        FetchingVersionManifest,
        FetchingPaperVersions,
        FetchingFabricGameVersions,
        FetchingPurpurVersions,
        FetchingForgePromotions,
        FetchingNeoForgeGameVersions,
        // Build/loader listing for one Minecraft version
        FetchingPaperBuildList,
        FetchingFabricBuildList,
        FetchingPurpurBuildList,
        FetchingForgeBuildList,
        FetchingNeoForgeBuildList,
        // Vanilla
        FetchingVanillaManifest,
        FetchingVanillaVersionJson,
        // Paper
        FetchingPaperBuilds,
        // Fabric
        FetchingFabricInstallerList,
        FetchingFabricLoaderList,
        // Purpur
        FetchingPurpurBuilds,
        // Forge
        FetchingForgeVersions,
        DownloadingForgeInstaller,
        // NeoForge
        FetchingNeoForgeVersions,
        DownloadingNeoForgeInstaller,
        // Final download
        DownloadingJar
    };

    Step m_step = Step::Idle;
    QString m_version;
    QString m_type;
    QString m_destinationDir;
    QString m_targetJarPath;
    QString m_javaPath;
    QString m_loaderVersion;
    QString m_versionsType;
    QString m_buildsType;
    QString m_buildsVersion;
    QString m_fabricInstallerVer; // cached for Fabric two-step

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_currentReply = nullptr;
    std::unique_ptr<QSaveFile> m_outputFile;
    std::unique_ptr<QCryptographicHash> m_downloadHash;
    QByteArray m_expectedHash;
    bool m_fileWriteFailed = false;
    ServerProviderEndpoints m_endpoints;
};
