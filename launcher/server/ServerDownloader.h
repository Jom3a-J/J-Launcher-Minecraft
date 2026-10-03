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
#include <QCryptographicHash>
#include <QStringList>
#include <QUrl>
#include <QHash>
#include <memory>

#include "net/NetJob.h"

class QProcess;
class ServerDownloaderProvider;

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

QString serverLoaderInstallIncompleteMarkerPath(const QString &serverDirectory);

/*! Lists and downloads server software for every supported server type.
 *
 *  One object runs one request or install at a time. It owns the network requests, file
 *  downloads and the Forge-style installer process, so cancelling stops all of them; each server
 *  type's own steps live in a ServerDownloaderProvider (ServerDownloaderVanilla/Paper/Fabric/
 *  Forge.cpp; Paper also holds Purpur, Forge also holds NeoForge and the installer run).
 *  ServerDownloaderCatalog.cpp reads the version and build lists the providers publish.
 */
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
    QString resolvedLoaderVersion() const { return m_resolvedLoaderVersion; }

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
    friend class ServerDownloaderProvider;

    enum class Activity {
        Idle,
        ListingVersions,
        ListingBuilds,
        Installing
    };
    //! The file download in progress during an install, if any.
    enum class FileDownload {
        None,
        ServerJar,          //!< The finished server jar; the install ends when it arrives.
        LargeFile,          //!< Another server jar the install needs, fetched in ranged pieces.
        SmallFile,          //!< A loader installer.
        InstallerLibraries  //!< Libraries fetched ahead of a Forge-style installer.
    };

    // Requests and file downloads (ServerDownloader.cpp)
    QNetworkRequest createRequest(const QUrl &url);
    void sendRequest(const QUrl &url);
    void handleReply(QNetworkReply *reply);
    void downloadServerJar(const QUrl &url, const QByteArray &expectedHash,
                           QCryptographicHash::Algorithm hashAlgorithm);
    void startFileDownload(const QUrl &url, const QString &outputPath,
                           const QByteArray &expectedHash,
                           QCryptographicHash::Algorithm hashAlgorithm,
                           FileDownload kind);
    void onFileDownloadSucceeded();
    void onFileDownloadFailed(const QString &reason);
    void retireFileDownloadJob(bool abort);
    void finishDownload(bool success, const QString &errorMessage = QString());
    void failCurrentRequest(const QString &message);
    QString currentFailureContext() const;
    void onVersionManifestFetched(const QByteArray &data);
    void onBuildManifestFetched(const QByteArray &data);
    void cleanUp();

    // Forge-style loader installers (ServerDownloaderForge.cpp)
    void runLoaderInstaller(const QString &loaderName, const QString &installerPath);
    bool prefetchModernInstallerLibraries(const QString &installerPath, const QString &loaderName);
    void onInstallerLibrariesPrefetched();
    void startInstallerProcess(const QString &loaderName, const QString &installerPath);
    void stopInstallerProcess();
    bool writeLoaderInstallIncompleteMarker(QString *errorMessage) const;
    bool validateLoaderInstallation(const QString &loaderName,
                                    QString *errorMessage) const;

    Activity m_activity = Activity::Idle;
    FileDownload m_fileDownload = FileDownload::None;
    std::unique_ptr<ServerDownloaderProvider> m_provider; //!< The server type being installed.
    QString m_version;
    QString m_type;
    QString m_destinationDir;
    QString m_targetJarPath;
    QString m_javaPath;
    QString m_loaderVersion;
    QString m_resolvedLoaderVersion;
    bool m_loaderScriptExistedBeforeInstall = false;
    QString m_versionsType;
    QString m_buildsType;
    QString m_buildsVersion;
    QString m_pendingInstallerLoader;
    QString m_pendingInstallerPath;
    QStringList m_prefetchLibraryPaths;
    QHash<QString, QByteArray> m_prefetchLibraryHashes;

    QNetworkAccessManager *m_network = nullptr; //!< Private manager retained for metadata requests.
    QNetworkAccessManager *m_downloadNetwork = nullptr; //!< Shared app manager, or app-owned fallback for downloads.
    QNetworkReply *m_currentReply = nullptr;
    QProcess *m_installerProcess = nullptr;
    QString m_activeInstallerPath;
    NetJob::Ptr m_fileDownloadJob;
    QList<NetJob::Ptr> m_retiredJobs;
    QString m_fileDownloadPath;
    ServerProviderEndpoints m_endpoints;
    bool m_finishedEmitted = false;};
