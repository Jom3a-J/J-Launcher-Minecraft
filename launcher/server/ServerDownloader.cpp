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
#include "ServerDownloaderShared.h"
#include "BuildConfig.h"
#include <QCoreApplication>
#include <QFile>
#include <QNetworkRequest>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QProcess>
#include <QDirIterator>
#include <QRegularExpression>
#include "net/ChecksumValidator.h"
#include "net/Download.h"
#include "net/PartFile.h"
#include "net/SegmentedDownload.h"
#include "archive/ArchiveReader.h"
#include <algorithm>

#if defined(LAUNCHER_APPLICATION)
#include "Application.h"
#include "settings/SettingsObject.h"
#endif

namespace {
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
}  // namespace

namespace ServerDownloaderDetail {

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

void ServerDownloader::startDownload(const QString &version, const QString &type, const QString &destinationDir,
                                     const QString &javaPath, const QString &loaderVersion)
{
    m_finishedEmitted = false;
    m_version = version;
    m_type = type.toLower();
    m_destinationDir = destinationDir;
    m_targetJarPath = QDir(destinationDir).filePath("server.jar");
    m_javaPath = javaPath;
    m_loaderVersion = loaderVersion.trimmed();
    m_resolvedLoaderVersion.clear();
    m_pendingForgeVersion.clear();
    m_pendingForgeMavenVersion.clear();
    m_fabricJarPendingValidation = false;
    m_pendingInstallerPath.clear();
    m_pendingInstallerUrl.clear();
    m_pendingPurpurBuild.clear();
    m_legacyForgeServerJarPath.clear();
    m_fetchingLegacyForgeServerJar = false;
    m_downloadingLegacyForgeServerJar = false;
    m_pendingInstallerLoader.clear();
    m_pendingInstallerPath.clear();
    m_prefetchLibraryPaths.clear();
    m_prefetchLibraryHashes.clear();
    m_loaderScriptExistedBeforeInstall = QFileInfo(
        QDir(destinationDir).filePath(platformLoaderScriptName())).isFile();

    QDir().mkpath(m_destinationDir);

    if (m_type == "vanilla") {
        fetchVanillaManifest();
    } else if (m_type == "paper") {
        fetchPaperBuilds();
    } else if (m_type == "fabric") {
        fetchFabricInstaller();
    } else if (m_type == "purpur") {
        fetchPurpurBuilds();
    } else if (m_type == "forge") {
        if (m_loaderVersion.isEmpty()) {
            fetchForgeVersions();
        } else {
            downloadForgeInstaller(m_loaderVersion);
        }
    } else if (m_type == "neoforge") {
        if (m_loaderVersion.isEmpty()) {
            fetchNeoForgeVersions();
        } else {
            downloadNeoForgeInstaller(m_loaderVersion);
        }
    } else {
        finishDownload(false, tr("Unsupported server type: %1").arg(type));
    }
}

void ServerDownloader::finishDownload(bool success, const QString &errorMessage)
{
    if (m_finishedEmitted)
        return;
    m_finishedEmitted = true;
    m_step = Step::Idle;
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

    QUrl url;
    if (m_versionsType == "vanilla") {
        m_step = Step::FetchingVersionManifest;
        url = m_endpoints.vanillaManifest;
    } else if (m_versionsType == "paper") {
        m_step = Step::FetchingPaperVersions;
        url = m_endpoints.paperApiBase.resolved(QUrl("projects/paper"));
    } else if (m_versionsType == "fabric") {
        m_step = Step::FetchingFabricGameVersions;
        url = m_endpoints.fabricApiBase.resolved(QUrl("versions/game"));
    } else if (m_versionsType == "purpur") {
        m_step = Step::FetchingPurpurVersions;
        url = m_endpoints.purpurApiBase.resolved(QUrl("purpur"));
    } else if (m_versionsType == "forge") {
        m_step = Step::FetchingForgePromotions;
        url = m_endpoints.forgeMavenBase.resolved(
            QUrl(QStringLiteral("net/minecraftforge/forge/maven-metadata.xml")));
    } else if (m_versionsType == "neoforge") {
        m_step = Step::FetchingNeoForgeGameVersions;
        url = m_endpoints.neoForgeVersions;
    } else {
        emit versionsFailed(tr("Unsupported server type: %1").arg(type));
        return;
    }

    emit statusMessage(tr("Fetching available %1 versions...").arg(type));
    QNetworkRequest request = createRequest(url);
    m_currentReply = m_network->get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
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

    QUrl url;
    if (m_buildsType == "paper") {
        m_step = Step::FetchingPaperBuildList;
        url = m_endpoints.paperApiBase.resolved(
            QUrl(QString("projects/paper/versions/%1/builds").arg(m_buildsVersion)));
    } else if (m_buildsType == "fabric") {
        m_step = Step::FetchingFabricBuildList;
        url = m_endpoints.fabricApiBase.resolved(
            QUrl(QString("versions/loader/%1").arg(m_buildsVersion)));
    } else if (m_buildsType == "purpur") {
        m_step = Step::FetchingPurpurBuildList;
        url = m_endpoints.purpurApiBase.resolved(
            QUrl(QString("purpur/%1").arg(m_buildsVersion)));
    } else if (m_buildsType == "forge") {
        m_step = Step::FetchingForgeBuildList;
        url = m_endpoints.forgeMavenBase.resolved(
            QUrl(QStringLiteral("net/minecraftforge/forge/maven-metadata.xml")));
    } else if (m_buildsType == "neoforge") {
        m_step = Step::FetchingNeoForgeBuildList;
        url = usesLegacyNeoForgeCoordinates(m_buildsVersion, QString())
            ? m_endpoints.neoForgeMavenBase.resolved(
                  QUrl(QStringLiteral("net/neoforged/forge/maven-metadata.xml")))
            : m_endpoints.neoForgeVersions;
    } else if (m_buildsType == "vanilla") {
        emit buildsReady({});
        return;
    } else {
        emit buildsFailed(tr("Unsupported server type: %1").arg(type));
        return;
    }

    emit statusMessage(tr("Fetching %1 builds for Minecraft %2...").arg(type, m_buildsVersion));
    m_currentReply = m_network->get(createRequest(url));
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::cancel()
{
    if (m_step == Step::Idle) {
        return;
    }
    const QString cancelledServerJarPath =
        (m_step == Step::DownloadingJar || m_step == Step::DownloadingLegacyForgeServerJar)
        ? m_fileDownloadPath : QString();
    const bool versionRequest = isVersionListStep();
    const bool buildRequest = isBuildListStep();
    cleanUp();
    m_pendingInstallerLoader.clear();
    m_pendingInstallerPath.clear();
    m_prefetchLibraryPaths.clear();
    m_prefetchLibraryHashes.clear();
    if (!cancelledServerJarPath.isEmpty())
        QFile::remove(Net::PartFile::partPathFor(cancelledServerJarPath));
    m_step = Step::Idle;
    if (versionRequest) {
        emit versionsFailed(tr("Version request cancelled."));
    } else if (buildRequest) {
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

void ServerDownloader::stopInstallerProcess()
{
    if (!m_installerProcess)
        return;

    QProcess *installer = m_installerProcess;
    m_installerProcess = nullptr;
    disconnect(installer, nullptr, this, nullptr);
    if (installer->state() != QProcess::NotRunning) {
        installer->kill();
        installer->waitForFinished(-1);
    }
    installer->deleteLater();

    if (!m_activeInstallerPath.isEmpty()) {
        QFile::remove(m_activeInstallerPath);
        QFile::remove(m_activeInstallerPath + QStringLiteral(".log"));
        m_activeInstallerPath.clear();
    }
}

bool ServerDownloader::writeLoaderInstallIncompleteMarker(QString *errorMessage) const
{
    QFile marker(serverLoaderInstallIncompleteMarkerPath(m_destinationDir));
    if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *errorMessage = tr("Could not mark the loader installation as incomplete: %1")
                            .arg(marker.errorString());
        return false;
    }
    if (marker.write(QByteArrayLiteral("J Launcher loader installation in progress\n")) < 0
        || !marker.flush()) {
        *errorMessage = tr("Could not write the incomplete loader installation marker: %1")
                            .arg(marker.errorString());
        return false;
    }
    return true;
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
        if (m_step == Step::ResolvingForgeInstallerMetadata) {
            prepareForgeInstaller(m_pendingForgeVersion, QString());
            return;
        }
        if (m_step == Step::FetchingForgeInstallerChecksum
            || m_step == Step::FetchingNeoForgeInstallerChecksum) {
            if (statusCode == 404) {
                emit statusMessage(tr("No checksum is published for this installer; continuing without verification."));
                const bool forge = m_step == Step::FetchingForgeInstallerChecksum;
                m_step = forge ? Step::DownloadingForgeInstaller : Step::DownloadingNeoForgeInstaller;
                startFileDownload(m_pendingInstallerUrl, m_pendingInstallerPath, {},
                                  QCryptographicHash::Sha1, false);
                return;
            }
            cleanUp();
            finishDownload(false, tr("Failed to fetch the installer checksum: %1").arg(errorStr));
            return;
        }
        if (m_step == Step::FetchingPurpurBuildInfo) {
            cleanUp();
            finishDownload(false, tr("Failed to fetch the Purpur checksum for build %1: %2")
                                     .arg(m_pendingPurpurBuild, errorStr));
            return;
        }
        cleanUp();
        failCurrentRequest(tr("Network error: %1").arg(errorStr));
        return;
    }

    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    switch (m_step) {
        case Step::FetchingVersionManifest:
        case Step::FetchingPaperVersions:
        case Step::FetchingFabricGameVersions:
        case Step::FetchingPurpurVersions:
        case Step::FetchingForgePromotions:
        case Step::FetchingNeoForgeGameVersions:
            onVersionManifestFetched(responseData);
            break;
        case Step::FetchingPaperBuildList:
        case Step::FetchingFabricBuildList:
        case Step::FetchingPurpurBuildList:
        case Step::FetchingForgeBuildList:
        case Step::FetchingNeoForgeBuildList:
            onBuildManifestFetched(responseData);
            break;
        case Step::FetchingVanillaManifest:
            onVanillaManifestFetched(responseData);
            break;
        case Step::FetchingVanillaVersionJson:
            onVanillaVersionJsonFetched(responseData);
            break;
        case Step::FetchingPaperBuilds:
            onPaperBuildsFetched(responseData);
            break;
        case Step::FetchingFabricInstallerList:
            onFabricInstallerFetched(responseData);
            break;
        case Step::FetchingFabricLoaderList:
            onFabricLoaderFetched(m_fabricInstallerVer, responseData);
            break;
        case Step::FetchingPurpurBuilds:
            onPurpurBuildsFetched(responseData);
            break;
        case Step::FetchingPurpurBuildInfo:
            onPurpurBuildInfoFetched(responseData);
            break;
        case Step::FetchingForgeVersions:
            onForgeVersionsFetched(responseData);
            break;
        case Step::ResolvingForgeInstallerMetadata:
            onForgeInstallerMetadataFetched(responseData);
            break;
        case Step::FetchingForgeInstallerChecksum:
            onForgeInstallerChecksumFetched(responseData);
            break;
        case Step::FetchingNeoForgeVersions:
            onNeoForgeVersionsFetched(responseData);
            break;
        case Step::FetchingNeoForgeInstallerChecksum:
            onNeoForgeInstallerChecksumFetched(responseData);
            break;
        default:
            break;
    }
}

void ServerDownloader::downloadFile(const QString &url, const QString &outputPath,
                                    const QByteArray &expectedHash,
                                    QCryptographicHash::Algorithm hashAlgorithm)
{
    cleanUp();
    m_step = m_downloadingLegacyForgeServerJar
        ? Step::DownloadingLegacyForgeServerJar : Step::DownloadingJar;
    emit statusMessage(m_downloadingLegacyForgeServerJar
                           ? tr("Downloading the vanilla server jar for the Forge installer...")
                           : tr("Downloading server jar..."));
    startFileDownload(QUrl(url), outputPath, expectedHash, hashAlgorithm, true);
}

void ServerDownloader::startFileDownload(const QUrl &url, const QString &outputPath,
                                         const QByteArray &expectedHash,
                                         QCryptographicHash::Algorithm hashAlgorithm,
                                         bool mayBeLarge)
{
#if defined(LAUNCHER_APPLICATION)
    if (auto *application = APPLICATION_DYN; application && application->network())
        m_downloadNetwork = application->network();
#endif
    auto job = NetJob::Ptr(new NetJob(tr("Server file download"), m_downloadNetwork));
    job->setAskRetry(false);
    m_fileDownloadPath = outputPath;

    if (mayBeLarge && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))) {
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
        auto download = Net::Download::makeFile(url, outputPath);
        if (!expectedHash.isEmpty())
            download->addValidator(
                new Net::ChecksumValidator(hashAlgorithm, QString::fromLatin1(expectedHash).toLower()));
        job->addNetAction(download);
    }

    const int progressMaximum =
        (m_step == Step::DownloadingJar || m_step == Step::DownloadingLegacyForgeServerJar) ? 100 : 50;
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

    switch (m_step) {
        case Step::DownloadingLegacyForgeServerJar:
            m_downloadingLegacyForgeServerJar = false;
            beginForgeInstallerDownload(m_pendingForgeVersion, m_pendingForgeMavenVersion);
            return;
        case Step::DownloadingForgeInstaller:
            emit progress(50);
            onForgeInstallerDownloaded();
            return;
        case Step::DownloadingNeoForgeInstaller:
            emit progress(50);
            onNeoForgeInstallerDownloaded();
            return;
        case Step::DownloadingJar:
            if (m_fabricJarPendingValidation) {
                MMCZip::ArchiveReader archive(m_targetJarPath);
                if (!archive.goToFile(QStringLiteral("META-INF/MANIFEST.MF"))) {
                    QFile::remove(m_targetJarPath);
                    m_fabricJarPendingValidation = false;
                    finishDownload(false, tr("The downloaded Fabric server launcher is not a valid jar file."));
                    return;
                }
                m_fabricJarPendingValidation = false;
            }
            emit statusMessage(tr("Download complete!"));
            emit progress(100);
            finishDownload(true);
            return;
        default:
            return;
    }
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

bool ServerDownloader::validateLoaderInstallation(const QString &loaderName,
                                                   QString *errorMessage) const
{
    const QDir serverDir(m_destinationDir);
    const QString normalizedLoader = loaderName.toLower();
    QStringList rootJarPatterns{ normalizedLoader + QStringLiteral("-*.jar") };
    if (normalizedLoader == QStringLiteral("forge")) {
        rootJarPatterns << QStringLiteral("minecraftforge-*.jar");
    } else if (normalizedLoader == QStringLiteral("neoforge")
               && usesLegacyNeoForgeCoordinates(m_version,
                                                 m_resolvedLoaderVersion)) {
        rootJarPatterns << QStringLiteral("forge-*.jar");
    }
    const QStringList rootJars = serverDir.entryList(
        rootJarPatterns, QDir::Files, QDir::Name);
    const bool hasRootLauncher = std::any_of(
        rootJars.cbegin(), rootJars.cend(), [&serverDir](const QString &fileName) {
            return !fileName.contains(QStringLiteral("installer"), Qt::CaseInsensitive)
                && QFileInfo(serverDir.filePath(fileName)).size() > 0;
        });

    const QString scriptName = platformLoaderScriptName();
    const QString argumentsFileName = platformLoaderArgumentsFileName();
    const QFileInfo script(serverDir.filePath(scriptName));
    if (!script.isFile() && !hasRootLauncher) {
        *errorMessage = tr("%1 installer completed without creating a runnable server.")
                            .arg(loaderName);
        return false;
    }

    // Forge 1.17+ does not normally create server.jar in the server root. It
    // installs the Mojang server and loader artifacts below libraries/ and
    // generates a run script that references a platform argument file.
    if (script.isFile()) {
        QString expectedArgumentsPath;
        if (normalizedLoader == QStringLiteral("forge")
            && !m_resolvedLoaderVersion.isEmpty()) {
            expectedArgumentsPath = serverDir.filePath(
                QStringLiteral("libraries/net/minecraftforge/forge/%1-%2/%3")
                    .arg(m_version, m_resolvedLoaderVersion, argumentsFileName));
        } else if (normalizedLoader == QStringLiteral("neoforge")
                   && !m_resolvedLoaderVersion.isEmpty()) {
            expectedArgumentsPath = usesLegacyNeoForgeCoordinates(
                                        m_version, m_resolvedLoaderVersion)
                ? serverDir.filePath(
                      QStringLiteral("libraries/net/neoforged/forge/%1-%2/%3")
                          .arg(m_version, m_resolvedLoaderVersion,
                               argumentsFileName))
                : serverDir.filePath(
                      QStringLiteral("libraries/net/neoforged/neoforge/%1/%2")
                          .arg(m_resolvedLoaderVersion, argumentsFileName));
        }
        const bool hasExpectedArgumentsFile =
            QFileInfo(expectedArgumentsPath).size() > 0;
        bool hasArgumentsFile = hasExpectedArgumentsFile;
        // A fresh installer may use an older or provider-specific layout that
        // is still unambiguous. During an update, however, accepting any
        // argument file below libraries/ could mistake the previous loader's
        // files for the requested target build.
        if (!hasArgumentsFile && !m_loaderScriptExistedBeforeInstall) {
            QDirIterator arguments(serverDir.filePath(QStringLiteral("libraries")),
                                   { argumentsFileName }, QDir::Files,
                                   QDirIterator::Subdirectories);
            while (arguments.hasNext()) {
                if (QFileInfo(arguments.next()).size() > 0) {
                    hasArgumentsFile = true;
                    break;
                }
            }
        }
        if (!hasArgumentsFile) {
            *errorMessage = tr("%1 installer created %2 but did not create its loader argument file.")
                                .arg(loaderName, scriptName);
            return false;
        }
        if (hasExpectedArgumentsFile && !expectedArgumentsPath.isEmpty()) {
            QFile scriptFile(script.absoluteFilePath());
            if (!scriptFile.open(QIODevice::ReadOnly)) {
                *errorMessage = tr("%1 installer created %2 but J Launcher could not verify it.")
                                    .arg(loaderName, scriptName);
                return false;
            }
            const QString scriptContents = QDir::fromNativeSeparators(
                QString::fromLocal8Bit(scriptFile.readAll()));
            const QString expectedReference = QDir::fromNativeSeparators(
                serverDir.relativeFilePath(expectedArgumentsPath));
            if (!scriptContents.contains(expectedReference, Qt::CaseInsensitive)) {
                *errorMessage = tr("%1 installer did not connect %2 to the requested loader build.")
                                    .arg(loaderName, scriptName);
                return false;
            }
        }

        bool hasMinecraftServer = false;
        QDirIterator minecraftServers(
            serverDir.filePath(QStringLiteral("libraries/net/minecraft/server/%1")
                                   .arg(m_version)),
            { QStringLiteral("server-*.jar") }, QDir::Files,
            QDirIterator::Subdirectories);
        while (minecraftServers.hasNext()) {
            if (QFileInfo(minecraftServers.next()).size() > 0) {
                hasMinecraftServer = true;
                break;
            }
        }
        if (!hasMinecraftServer) {
            *errorMessage = tr("%1 installer did not download the Minecraft server files. Check the installer network output and try again.")
                                .arg(loaderName);
            return false;
        }
    }
    return true;
}

// ==================== Version Manifest ====================

void ServerDownloader::onVersionManifestFetched(const QByteArray &data)
{
    QString error;
    const QStringList versions = parseAvailableVersions(m_versionsType, data, &error);
    m_step = Step::Idle;
    if (versions.isEmpty()) {
        emit versionsFailed(error.isEmpty() ? tr("No compatible versions were returned by the provider.") : error);
        return;
    }
    emit versionsReady(versions);
}

bool ServerDownloader::isVersionListStep() const
{
    return m_step == Step::FetchingVersionManifest
        || m_step == Step::FetchingPaperVersions
        || m_step == Step::FetchingFabricGameVersions
        || m_step == Step::FetchingPurpurVersions
        || m_step == Step::FetchingForgePromotions
        || m_step == Step::FetchingNeoForgeGameVersions;
}

bool ServerDownloader::isBuildListStep() const
{
    return m_step == Step::FetchingPaperBuildList
        || m_step == Step::FetchingFabricBuildList
        || m_step == Step::FetchingPurpurBuildList
        || m_step == Step::FetchingForgeBuildList
        || m_step == Step::FetchingNeoForgeBuildList;
}

void ServerDownloader::onBuildManifestFetched(const QByteArray &data)
{
    QString error;
    const QStringList builds = parseAvailableBuilds(m_buildsType, m_buildsVersion, data, &error);
    m_step = Step::Idle;
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
    if (isVersionListStep()) {
        m_step = Step::Idle;
        emit versionsFailed(contextualMessage);
    } else if (isBuildListStep()) {
        m_step = Step::Idle;
        emit buildsFailed(contextualMessage);
    } else {
        finishDownload(false, contextualMessage);
    }
}

QString ServerDownloader::currentFailureContext() const
{
    QString provider = isVersionListStep() ? m_versionsType
        : (isBuildListStep() ? m_buildsType : m_type);
    if (provider.compare("neoforge", Qt::CaseInsensitive) == 0) {
        provider = QStringLiteral("NeoForge");
    } else if (!provider.isEmpty()) {
        provider[0] = provider[0].toUpper();
    }

    if (isVersionListStep()) {
        return tr("%1 version discovery").arg(provider);
    }
    if (isBuildListStep()) {
        return tr("%1 build discovery for Minecraft %2").arg(provider, m_buildsVersion);
    }

    QString step;
    switch (m_step) {
        case Step::FetchingVanillaManifest:
            step = tr("fetching the version manifest");
            break;
        case Step::FetchingVanillaVersionJson:
            step = tr("fetching version details");
            break;
        case Step::FetchingPaperBuilds:
        case Step::FetchingPurpurBuilds:
            step = tr("fetching provider builds");
            break;
        case Step::FetchingFabricInstallerList:
            step = tr("fetching Fabric installer versions");
            break;
        case Step::FetchingFabricLoaderList:
            step = tr("fetching Fabric loader versions");
            break;
        case Step::FetchingForgeVersions:
        case Step::FetchingNeoForgeVersions:
        case Step::ResolvingForgeInstallerMetadata:
            step = tr("resolving the loader installer");
            break;
        case Step::DownloadingForgeInstaller:
        case Step::DownloadingNeoForgeInstaller:
        case Step::DownloadingInstallerLibraries:
            step = tr("downloading the loader installer");
            break;
        case Step::DownloadingJar:
            step = tr("downloading the server JAR");
            break;
        case Step::DownloadingLegacyForgeServerJar:
            step = tr("downloading the vanilla server JAR for Forge");
            break;
        default:
            step = tr("server setup");
            break;
    }
    return tr("%1 %2").arg(provider, step);
}
