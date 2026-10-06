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

#include "ServerDownloader.h"
#include "ServerDownloaderProvider.h"
#include "ServerDownloaderShared.h"
#include "BuildConfig.h"
#include <QCoreApplication>
#include <QFile>
#include <QNetworkRequest>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QRegularExpression>
#include "net/ChecksumValidator.h"
#include "net/NetRequest.h"
#include "net/PartFile.h"
#include "net/SegmentedDownload.h"

#if defined(LAUNCHER_APPLICATION)
#include "Application.h"
#include "settings/SettingsObject.h"
#endif

namespace ServerDownloaderDetail {

QString platformLoaderScriptName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("run.bat");
#else
    return QStringLiteral("run.sh");
#endif
}

QString platformLoaderArgumentsFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("win_args.txt");
#else
    return QStringLiteral("unix_args.txt");
#endif
}

bool usesLegacyNeoForgeCoordinates(const QString &minecraftVersion,
                                   const QString &loaderVersion)
{
    // NeoForge's first 1.20.1 releases retained the Forge-style artifact
    // name and combined Minecraft/loader coordinate under net.neoforged.
    // Newer releases use net.neoforged:neoforge:<loader-version>.
    return minecraftVersion.trimmed() == QStringLiteral("1.20.1")
        || loaderVersion.trimmed().startsWith(QStringLiteral("47."));
}

bool fileMatchesSha1(const QString &path, const QByteArray &expectedSha1)
{
    if (expectedSha1.isEmpty())
        return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QCryptographicHash hash(QCryptographicHash::Sha1);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty())
            return false;
        hash.addData(chunk);
    }
    return file.error() == QFile::NoError
        && hash.result().toHex().compare(expectedSha1, Qt::CaseInsensitive) == 0;
}

bool isSha1(const QByteArray &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{40}$"));
    return pattern.match(QString::fromLatin1(value)).hasMatch();
}

}  // namespace ServerDownloaderDetail

using namespace ServerDownloaderDetail;

ServerProviderEndpoints ServerProviderEndpoints::production()
{
    return {
        QUrl("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"),
        QUrl("https://fill.papermc.io/v3/"),
        QUrl("https://meta.fabricmc.net/v2/"),
        QUrl("https://api.purpurmc.org/v2/"),
        QUrl("https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json"),
        QUrl("https://maven.minecraftforge.net/"),
        QUrl("https://maven.neoforged.net/api/maven/versions/releases/net/neoforged/neoforge"),
        QUrl("https://maven.neoforged.net/releases/")
    };
}

QString serverLoaderInstallIncompleteMarkerPath(const QString &serverDirectory)
{
    return QDir(serverDirectory).filePath(
        QStringLiteral(".jlauncher-loader-install-incomplete"));
}

ServerDownloader::ServerDownloader(QObject *parent)
    : ServerDownloader(ServerProviderEndpoints::production(), parent)
{
}

ServerDownloader::ServerDownloader(const QUrl &vanillaManifestUrl, QObject *parent)
    : ServerDownloader(ServerProviderEndpoints::production(), parent)
{
    m_endpoints.vanillaManifest = vanillaManifestUrl;
}

ServerDownloader::ServerDownloader(const ServerProviderEndpoints &endpoints, QObject *parent)
    : QObject(parent)
    , m_endpoints(endpoints)
{
    m_network = new QNetworkAccessManager(this);
    m_downloadNetwork = m_network;
#if defined(LAUNCHER_APPLICATION)
    if (auto *application = APPLICATION_DYN; application && application->network()) {
        m_downloadNetwork = application->network();
    }
#endif
    if (m_downloadNetwork == m_network) {
        if (auto *application = QCoreApplication::instance())
            m_downloadNetwork = new QNetworkAccessManager(application);
    }
}

ServerDownloader::~ServerDownloader()
{
    cleanUp();
}

QStringList ServerDownloader::supportedTypes()
{
    return {"Vanilla", "Paper", "Fabric", "Purpur", "Forge", "NeoForge"};
}

ServerDownloader::VersionChannel ServerDownloader::versionChannel(const QString &version)
{
    const QString normalized = version.trimmed().toLower();
    static const QRegularExpression legacyBetaPattern(QStringLiteral("^[ab]\\d"));
    static const QRegularExpression weeklySnapshotPattern(QStringLiteral("^\\d{2}w\\d{2}[a-z]"));
    static const QRegularExpression previewPattern(
        QStringLiteral("(?:^|[-_.])(?:pre|rc)\\d*(?:$|[-_.])"));

    if (normalized.contains(QStringLiteral("beta"))
        || normalized.contains(QStringLiteral("alpha"))
        || legacyBetaPattern.match(normalized).hasMatch()) {
        return VersionChannel::Beta;
    }
    if (normalized.contains(QStringLiteral("snapshot"))
        || weeklySnapshotPattern.match(normalized).hasMatch()
        || previewPattern.match(normalized).hasMatch()) {
        return VersionChannel::Snapshot;
    }
    return VersionChannel::Release;
}

QNetworkRequest ServerDownloader::createRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setRawHeader(
        "User-Agent",
        QString("%1 (%2)").arg(BuildConfig.USER_AGENT, BuildConfig.LAUNCHER_GIT).toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

void ServerDownloader::sendRequest(const QUrl &url)
{
    m_fileDownload = FileDownload::None;
    QNetworkReply *reply = m_network->get(createRequest(url));
    m_currentReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        // Only the latest request's answer counts; an earlier one is just let go.
        if (reply != m_currentReply) {
            reply->deleteLater();
            return;
        }
        handleReply(reply);
    });
}

void ServerDownloader::startDownload(const QString &version, const QString &type, const QString &destinationDir,
                                     const QString &javaPath, const QString &loaderVersion)
{
    // A version or build list still loading is cancelled, so whoever asked for it hears so;
    // any other work still running is stopped before the new install begins.
    if (m_activity == Activity::ListingVersions || m_activity == Activity::ListingBuilds) {
        cancel();
    }
    cleanUp();
    m_finishedEmitted = false;
    m_version = version;
    m_type = type.toLower();
    m_destinationDir = destinationDir;
    m_targetJarPath = QDir(destinationDir).filePath("server.jar");
    m_javaPath = javaPath;
    m_loaderVersion = loaderVersion.trimmed();
    m_resolvedLoaderVersion.clear();
    m_pendingInstallerLoader.clear();
    m_pendingInstallerPath.clear();
    m_prefetchLibraryPaths.clear();
    m_prefetchLibraryHashes.clear();
    m_loaderScriptExistedBeforeInstall = QFileInfo(
        QDir(destinationDir).filePath(platformLoaderScriptName())).isFile();

    QDir().mkpath(m_destinationDir);

    m_provider = createServerDownloaderProvider(m_type, *this);
    if (!m_provider) {
        finishDownload(false, tr("Unsupported server type: %1").arg(type));
        return;
    }
    m_activity = Activity::Installing;
    m_fileDownload = FileDownload::None;
    m_provider->start();
}

void ServerDownloader::finishDownload(bool success, const QString &errorMessage)
{
    if (m_finishedEmitted)
        return;
    m_finishedEmitted = true;
    m_activity = Activity::Idle;
    m_fileDownload = FileDownload::None;
    emit finished(success, errorMessage);
}

void ServerDownloader::fetchAvailableVersions(const QString &type)
{
    cleanUp();
    m_pendingInstallerLoader.clear();
    m_pendingInstallerPath.clear();
    m_prefetchLibraryPaths.clear();
    m_prefetchLibraryHashes.clear();
    m_versionsType = type.trimmed().toLower();
    if (m_versionsType.isEmpty()) {
        m_versionsType = QStringLiteral("vanilla");
    }

    const auto provider = createServerDownloaderProvider(m_versionsType, *this);
    if (!provider) {
        emit versionsFailed(tr("Unsupported server type: %1").arg(type));
        return;
    }

    m_activity = Activity::ListingVersions;
    emit statusMessage(tr("Fetching available %1 versions...").arg(type));
    sendRequest(provider->versionListUrl());
}

void ServerDownloader::fetchAvailableBuilds(const QString &version, const QString &type)
{
    cleanUp();
    m_buildsVersion = version.trimmed();
    m_buildsType = type.trimmed().toLower();
    if (m_buildsVersion.isEmpty()) {
        emit buildsFailed(tr("A Minecraft version is required to discover builds."));
        return;
    }

    const auto provider = createServerDownloaderProvider(m_buildsType, *this);
    if (!provider) {
        emit buildsFailed(tr("Unsupported server type: %1").arg(type));
        return;
    }
    const QUrl url = provider->buildListUrl(m_buildsVersion);
    if (url.isEmpty()) {
        emit buildsReady({});
        return;
    }

    m_activity = Activity::ListingBuilds;
    emit statusMessage(tr("Fetching %1 builds for Minecraft %2...").arg(type, m_buildsVersion));
    sendRequest(url);
}

void ServerDownloader::cancel()
{
    if (m_activity == Activity::Idle) {
        return;
    }
    const QString cancelledServerJarPath =
        (m_fileDownload == FileDownload::ServerJar || m_fileDownload == FileDownload::LargeFile)
        ? m_fileDownloadPath : QString();
    const Activity cancelled = m_activity;
    cleanUp();
    m_pendingInstallerLoader.clear();
    m_pendingInstallerPath.clear();
    m_prefetchLibraryPaths.clear();
    m_prefetchLibraryHashes.clear();
    if (!cancelledServerJarPath.isEmpty())
        QFile::remove(Net::PartFile::partPathFor(cancelledServerJarPath));
    m_activity = Activity::Idle;
    if (cancelled == Activity::ListingVersions) {
        emit versionsFailed(tr("Version request cancelled."));
    } else if (cancelled == Activity::ListingBuilds) {
        emit buildsFailed(tr("Build request cancelled."));
    } else {
        finishDownload(false, tr("Download cancelled."));
    }
}

void ServerDownloader::cleanUp()
{
    stopInstallerProcess();
    if (m_currentReply) {
        m_currentReply->disconnect(this);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
    retireFileDownloadJob(true);
}

void ServerDownloader::retireFileDownloadJob(bool abort)
{
    if (!m_fileDownloadJob)
        return;

    auto job = m_fileDownloadJob;
    m_fileDownloadJob.reset();
    disconnect(job.get(), nullptr, this, nullptr);
    m_retiredJobs.append(job);
    if (abort && job->isRunning())
        job->abort();
}

void ServerDownloader::handleReply(QNetworkReply *reply)
{
    if (!reply) {
        return;
    }

    m_currentReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        QString errorStr = reply->errorString();
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (m_activity == Activity::Installing && m_provider
            && m_provider->onRequestFailed(statusCode, errorStr)) {
            return;
        }
        cleanUp();
        failCurrentRequest(tr("Network error: %1").arg(errorStr));
        return;
    }

    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    switch (m_activity) {
        case Activity::ListingVersions:
            onVersionManifestFetched(responseData);
            break;
        case Activity::ListingBuilds:
            onBuildManifestFetched(responseData);
            break;
        case Activity::Installing:
            if (m_provider)
                m_provider->onReply(responseData);
            break;
        case Activity::Idle:
            break;
    }
}

void ServerDownloader::downloadServerJar(const QUrl &url, const QByteArray &expectedHash,
                                         QCryptographicHash::Algorithm hashAlgorithm)
{
    cleanUp();
    emit statusMessage(tr("Downloading server jar..."));
    startFileDownload(url, m_targetJarPath, expectedHash, hashAlgorithm, FileDownload::ServerJar);
}

void ServerDownloader::startFileDownload(const QUrl &url, const QString &outputPath,
                                         const QByteArray &expectedHash,
                                         QCryptographicHash::Algorithm hashAlgorithm,
                                         FileDownload kind)
{
#if defined(LAUNCHER_APPLICATION)
    if (auto *application = APPLICATION_DYN; application && application->network())
        m_downloadNetwork = application->network();
#endif
    auto job = NetJob::Ptr(new NetJob(tr("Server file download"), m_downloadNetwork));
    job->setAskRetry(false);
    m_fileDownload = kind;
    m_fileDownloadPath = outputPath;

    const bool serverJar = kind == FileDownload::ServerJar || kind == FileDownload::LargeFile;
    if (serverJar && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))) {
        int segments = Net::SegmentedDownload::DefaultSegments;
#if defined(LAUNCHER_APPLICATION)
        if (auto *application = APPLICATION_DYN)
            segments = application->settings()->get("SegmentedDownloadSegments").toInt();
#endif
        auto segmented = Net::SegmentedDownload::makeFile(
            url, outputPath, m_downloadNetwork, job->scheduler(), segments);
        if (!expectedHash.isEmpty())
            segmented->addValidator(
                new Net::ChecksumValidator(hashAlgorithm, QString::fromLatin1(expectedHash).toLower()));
        job->addTask(segmented);
    } else {
        auto download = Net::NetRequest::makeFile(url, outputPath);
        if (!expectedHash.isEmpty())
            download->addValidator(
                new Net::ChecksumValidator(hashAlgorithm, QString::fromLatin1(expectedHash).toLower()));
        job->addNetAction(download);
    }

    // A loader installer fills the first half of the bar; running it fills the rest.
    const int progressMaximum = serverJar ? 100 : 50;
    connect(job.get(), &Task::progress, this, [this, progressMaximum](qint64 current, qint64 total) {
        if (total <= 0)
            return;
        const int percentage = qBound(0, static_cast<int>(current * progressMaximum / total), progressMaximum);
        emit progress(percentage);
    });
    connect(job.get(), &Task::succeeded, this, &ServerDownloader::onFileDownloadSucceeded);
    connect(job.get(), &Task::failed, this, &ServerDownloader::onFileDownloadFailed);
    connect(job.get(), &Task::aborted, this, [this]() {
        onFileDownloadFailed(tr("Download cancelled."));
    });

    m_fileDownloadJob = job;
    job->start();
}

void ServerDownloader::onFileDownloadSucceeded()
{
    retireFileDownloadJob(false);
    if (m_activity != Activity::Installing || !m_provider)
        return;

    if (m_fileDownload != FileDownload::ServerJar) {
        m_provider->onFileDownloaded();
        return;
    }
    QString error;
    if (!m_provider->checkServerJar(&error)) {
        finishDownload(false, error);
        return;
    }
    emit statusMessage(tr("Download complete!"));
    emit progress(100);
    finishDownload(true);
}

void ServerDownloader::onFileDownloadFailed(const QString &reason)
{
    retireFileDownloadJob(false);

    if (reason == tr("Download cancelled.")) {
        finishDownload(false, reason);
    } else if (reason == QStringLiteral("Failed to finalize validators")
               || reason == QCoreApplication::translate("Net::SegmentedDownload", "Failed to finalize validators")) {
        finishDownload(false,
                       tr("Download verification failed: the server file hash did not match the provider's value."));
    } else {
        failCurrentRequest(tr("Network error: %1").arg(reason));
    }
}

// ==================== Version and build lists ====================

void ServerDownloader::onVersionManifestFetched(const QByteArray &data)
{
    QString error;
    const QStringList versions = parseAvailableVersions(m_versionsType, data, &error);
    m_activity = Activity::Idle;
    if (versions.isEmpty()) {
        emit versionsFailed(error.isEmpty() ? tr("No compatible versions were returned by the provider.") : error);
        return;
    }
    emit versionsReady(versions);
}

void ServerDownloader::onBuildManifestFetched(const QByteArray &data)
{
    QString error;
    const QStringList builds = parseAvailableBuilds(m_buildsType, m_buildsVersion, data, &error);
    m_activity = Activity::Idle;
    if (builds.isEmpty()) {
        emit buildsFailed(error.isEmpty() ? tr("No compatible builds were returned by the provider.") : error);
        return;
    }
    emit buildsReady(builds);
}

void ServerDownloader::failCurrentRequest(const QString &message)
{
    const QString context = currentFailureContext();
    const QString contextualMessage = context.isEmpty()
        ? message
        : tr("%1 failed: %2").arg(context, message);
    if (m_activity == Activity::ListingVersions) {
        m_activity = Activity::Idle;
        emit versionsFailed(contextualMessage);
    } else if (m_activity == Activity::ListingBuilds) {
        m_activity = Activity::Idle;
        emit buildsFailed(contextualMessage);
    } else {
        finishDownload(false, contextualMessage);
    }
}

QString ServerDownloader::currentFailureContext() const
{
    QString provider = m_activity == Activity::ListingVersions ? m_versionsType
        : (m_activity == Activity::ListingBuilds ? m_buildsType : m_type);
    if (provider.compare("neoforge", Qt::CaseInsensitive) == 0) {
        provider = QStringLiteral("NeoForge");
    } else if (!provider.isEmpty()) {
        provider[0] = provider[0].toUpper();
    }

    if (m_activity == Activity::ListingVersions) {
        return tr("%1 version discovery").arg(provider);
    }
    if (m_activity == Activity::ListingBuilds) {
        return tr("%1 build discovery for Minecraft %2").arg(provider, m_buildsVersion);
    }

    QString step;
    if (m_fileDownload == FileDownload::ServerJar) {
        step = tr("downloading the server JAR");
    } else if (m_fileDownload == FileDownload::InstallerLibraries) {
        step = tr("downloading the loader installer");
    } else if (m_activity == Activity::Installing && m_provider) {
        step = m_provider->currentStep();
    } else {
        step = tr("server setup");
    }
    return tr("%1 %2").arg(provider, step);
}

// ==================== Provider services ====================

bool ServerDownloaderProvider::onRequestFailed(int, const QString &)
{
    return false;
}

bool ServerDownloaderProvider::checkServerJar(QString *)
{
    return true;
}

QString ServerDownloaderProvider::tr(const char *text)
{
    return ServerDownloader::tr(text);
}

const QString &ServerDownloaderProvider::version() const
{
    return m_downloader.m_version;
}

const QString &ServerDownloaderProvider::requestedLoaderVersion() const
{
    return m_downloader.m_loaderVersion;
}

const QString &ServerDownloaderProvider::destinationDir() const
{
    return m_downloader.m_destinationDir;
}

const QString &ServerDownloaderProvider::targetJarPath() const
{
    return m_downloader.m_targetJarPath;
}

const ServerProviderEndpoints &ServerDownloaderProvider::endpoints() const
{
    return m_downloader.m_endpoints;
}

void ServerDownloaderProvider::setResolvedLoaderVersion(const QString &loaderVersion)
{
    m_downloader.m_resolvedLoaderVersion = loaderVersion;
}

void ServerDownloaderProvider::status(const QString &message)
{
    emit m_downloader.statusMessage(message);
}

void ServerDownloaderProvider::progress(int percentage)
{
    emit m_downloader.progress(percentage);
}

void ServerDownloaderProvider::request(const QUrl &url)
{
    m_downloader.sendRequest(url);
}

void ServerDownloaderProvider::downloadServerJar(const QUrl &url, const QByteArray &expectedHash,
                                                 QCryptographicHash::Algorithm hashAlgorithm)
{
    m_downloader.downloadServerJar(url, expectedHash, hashAlgorithm);
}

void ServerDownloaderProvider::downloadFile(const QUrl &url, const QString &path,
                                            const QByteArray &expectedHash,
                                            QCryptographicHash::Algorithm hashAlgorithm,
                                            bool largeServerJar)
{
    m_downloader.startFileDownload(url, path, expectedHash, hashAlgorithm,
                                   largeServerJar ? ServerDownloader::FileDownload::LargeFile
                                                  : ServerDownloader::FileDownload::SmallFile);
}

void ServerDownloaderProvider::runLoaderInstaller(const QString &loaderName,
                                                  const QString &installerPath)
{
    m_downloader.runLoaderInstaller(loaderName, installerPath);
}

void ServerDownloaderProvider::stopActiveWork()
{
    m_downloader.cleanUp();
}

void ServerDownloaderProvider::fail(const QString &message)
{
    m_downloader.finishDownload(false, message);
}

std::unique_ptr<ServerDownloaderProvider> createServerDownloaderProvider(const QString &type,
                                                                         ServerDownloader &downloader)
{
    const QString normalized = type.trimmed().toLower();
    if (normalized == QStringLiteral("vanilla"))
        return makeVanillaServerProvider(downloader);
    if (normalized == QStringLiteral("paper"))
        return makePaperServerProvider(downloader);
    if (normalized == QStringLiteral("fabric"))
        return makeFabricServerProvider(downloader);
    if (normalized == QStringLiteral("purpur"))
        return makePurpurServerProvider(downloader);
    if (normalized == QStringLiteral("forge"))
        return makeForgeServerProvider(downloader);
    if (normalized == QStringLiteral("neoforge"))
        return makeNeoForgeServerProvider(downloader);
    return nullptr;
}
