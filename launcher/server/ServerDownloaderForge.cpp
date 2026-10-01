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

// Forge and NeoForge: installer download, library prefetch, and running the installer.

#include "ServerDownloader.h"
#include "ServerDownloaderShared.h"
#include <QFile>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QVersionNumber>
#include <QXmlStreamReader>
#include "net/ChecksumValidator.h"
#include "net/Download.h"
#include "archive/ArchiveReader.h"
#include <algorithm>

#if defined(LAUNCHER_APPLICATION)
#include "Application.h"
#endif

using namespace ServerDownloaderDetail;

namespace {
bool isBeforeMinecraft113(const QString &version)
{
    const QVersionNumber parsed = QVersionNumber::fromString(version);
    const QVersionNumber cutoff = QVersionNumber::fromString(QStringLiteral("1.13"));
    return !parsed.isNull() && QVersionNumber::compare(parsed, cutoff) < 0;
}

struct InstallerLibrary
{
    QString path;
    QUrl url;
    QByteArray sha1;
};

void addInstallerLibraries(const QJsonArray &entries, QList<InstallerLibrary> &libraries,
                           QSet<QString> &paths)
{
    for (const QJsonValue &entryValue : entries) {
        const QJsonObject downloads = entryValue.toObject().value(QStringLiteral("downloads")).toObject();
        const QJsonObject artifact = downloads.value(QStringLiteral("artifact")).toObject();
        const QString path = artifact.value(QStringLiteral("path")).toString();
        const QUrl url(artifact.value(QStringLiteral("url")).toString());
        const QByteArray sha1 = artifact.value(QStringLiteral("sha1")).toString().toLatin1();
        const QString cleanPath = QDir::cleanPath(path);
        if (path.isEmpty() || cleanPath != path || cleanPath == QStringLiteral("..")
            || cleanPath.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(path)
            || path.contains(QLatin1Char(':')) || path.contains(QLatin1Char('\\'))
            || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
            || url.host().isEmpty() || !isSha1(sha1) || paths.contains(cleanPath)) {
            continue;
        }
        paths.insert(cleanPath);
        libraries.append({cleanPath, url, sha1});
    }
}

void addVanillaServerArtifact(const QJsonObject &artifact, const QString &version,
                             QList<InstallerLibrary> &libraries, QSet<QString> &paths)
{
    if (version.isEmpty())
        return;
    const QUrl url(artifact.value(QStringLiteral("url")).toString());
    const QByteArray sha1 = artifact.value(QStringLiteral("sha1")).toString().toLatin1();
    const QString path = QStringLiteral("net/minecraft/server/%1/server-%1.jar").arg(version);
    const QString cleanPath = QDir::cleanPath(path);
    if ((url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
        || url.host().isEmpty() || !isSha1(sha1) || cleanPath != path
        || QDir::isAbsolutePath(path) || paths.contains(path)) {
        return;
    }
    paths.insert(path);
    libraries.append({path, url, sha1});
}
}  // namespace

// ==================== Forge ====================

void ServerDownloader::fetchForgeVersions()
{
    m_step = Step::FetchingForgeVersions;
    emit statusMessage(tr("Fetching Forge versions..."));

    // Maven metadata covers every published Forge build. The promotions feed
    // only contains selected recommended/latest Minecraft versions and can
    // reject otherwise valid server combinations.
    const QUrl metadataUrl = m_endpoints.forgeMavenBase.resolved(
        QUrl(QStringLiteral("net/minecraftforge/forge/maven-metadata.xml")));
    QNetworkRequest request = createRequest(metadataUrl);
    m_currentReply = m_network->get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onForgeVersionsFetched(const QByteArray &data)
{
    if (data.trimmed().startsWith('<')) {
        QString error;
        const QStringList builds = parseAvailableBuilds(
            QStringLiteral("forge"), m_version, data, &error);
        if (builds.isEmpty()) {
            m_step = Step::Idle;
            finishDownload(false, error.isEmpty()
                ? tr("No Forge version found for Minecraft %1.").arg(m_version)
                : error);
            return;
        }
        prepareForgeInstaller(builds.first(), resolveForgeMavenVersion(data, builds.first()));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        m_step = Step::Idle;
        finishDownload(false, tr("Failed to parse Forge promotions."));
        return;
    }

    QJsonObject promos = doc.object()["promos"].toObject();

    // Try recommended first, then latest
    QString forgeVersion;
    QString recKey = m_version + "-recommended";
    QString latKey = m_version + "-latest";

    if (promos.contains(recKey)) {
        forgeVersion = promos[recKey].toString();
    } else if (promos.contains(latKey)) {
        forgeVersion = promos[latKey].toString();
    }

    if (forgeVersion.isEmpty()) {
        m_step = Step::Idle;
        finishDownload(false, tr("No Forge version found for Minecraft %1.").arg(m_version));
        return;
    }

    downloadForgeInstaller(forgeVersion);
}

void ServerDownloader::downloadForgeInstaller(const QString &forgeVersion)
{
    m_resolvedLoaderVersion = forgeVersion;
    m_pendingForgeVersion = forgeVersion;
    m_step = Step::ResolvingForgeInstallerMetadata;
    emit statusMessage(tr("Resolving the Forge installer version..."));

    const QUrl metadataUrl = m_endpoints.forgeMavenBase.resolved(
        QUrl(QStringLiteral("net/minecraftforge/forge/maven-metadata.xml")));
    m_currentReply = m_network->get(createRequest(metadataUrl));
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onForgeInstallerMetadataFetched(const QByteArray &data)
{
    const QString requestedVersion = m_pendingForgeVersion;
    prepareForgeInstaller(requestedVersion, resolveForgeMavenVersion(data, requestedVersion));
}

QString ServerDownloader::resolveForgeMavenVersion(const QByteArray &data,
                                                    const QString &forgeVersion) const
{
    QXmlStreamReader xml(data);
    const QString exactVersion = m_version + QLatin1Char('-') + forgeVersion;
    const QString suffixedPrefix = exactVersion + QLatin1Char('-');
    QString suffixedVersion;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QStringLiteral("version"))
            continue;
        const QString candidate = xml.readElementText().trimmed();
        if (candidate == exactVersion)
            return candidate;
        if (suffixedVersion.isEmpty() && candidate.startsWith(suffixedPrefix))
            suffixedVersion = candidate;
    }
    return xml.hasError() ? QString() : suffixedVersion;
}

void ServerDownloader::prepareForgeInstaller(const QString &forgeVersion,
                                              const QString &mavenVersion)
{
    m_resolvedLoaderVersion = forgeVersion;
    m_pendingForgeVersion = forgeVersion;
    m_pendingForgeMavenVersion = mavenVersion;

    if (isBeforeMinecraft113(m_version)) {
        m_fetchingLegacyForgeServerJar = true;
        fetchVanillaManifest();
        return;
    }

    beginForgeInstallerDownload(forgeVersion, mavenVersion);
}

void ServerDownloader::beginForgeInstallerDownload(const QString &forgeVersion,
                                                    const QString &mavenVersion)
{
    // Download the exact loader selected by the modpack when one is supplied.
    const QString resolvedMavenVersion = mavenVersion.isEmpty()
        ? m_version + QLatin1Char('-') + forgeVersion : mavenVersion;
    const QUrl installerUrl = m_endpoints.forgeMavenBase.resolved(
        QUrl(QStringLiteral("net/minecraftforge/forge/%1/forge-%1-installer.jar")
                 .arg(resolvedMavenVersion)));

    QString installerPath = QDir(m_destinationDir).filePath("forge-installer.jar");

    cleanUp();
    m_pendingInstallerUrl = installerUrl;
    m_pendingInstallerPath = installerPath;
    fetchForgeInstallerChecksum(installerUrl, installerPath);
}

void ServerDownloader::fetchForgeInstallerChecksum(const QUrl &installerUrl,
                                                    const QString &installerPath)
{
    QUrl checksumUrl = installerUrl;
    checksumUrl.setPath(checksumUrl.path() + QStringLiteral(".sha1"));
    m_pendingInstallerUrl = installerUrl;
    m_pendingInstallerPath = installerPath;
    m_step = Step::FetchingForgeInstallerChecksum;
    emit statusMessage(tr("Fetching Forge installer checksum..."));
    m_currentReply = m_network->get(createRequest(checksumUrl));
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onForgeInstallerChecksumFetched(const QByteArray &data)
{
    const QStringList tokens = QString::fromLatin1(data).split(
        QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const QByteArray token = tokens.isEmpty() ? QByteArray() : tokens.first().toLatin1();
    if (!isSha1(token)) {
        finishDownload(false, tr("The Forge installer checksum is malformed."));
        return;
    }
    m_step = Step::DownloadingForgeInstaller;
    emit statusMessage(tr("Downloading Forge installer..."));
    startFileDownload(m_pendingInstallerUrl, m_pendingInstallerPath, token,
                      QCryptographicHash::Sha1, false);
}

void ServerDownloader::onForgeInstallerDownloaded()
{
    const QString installerPath = QDir(m_destinationDir).filePath("forge-installer.jar");
    if (prefetchModernInstallerLibraries(installerPath, QStringLiteral("Forge")))
        return;
    startInstallerProcess(QStringLiteral("Forge"), installerPath);
}

bool ServerDownloader::prefetchModernInstallerLibraries(const QString &installerPath,
                                                         const QString &loaderName)
{
    QFile installer(installerPath);
    if (!installer.open(QIODevice::ReadOnly))
        return false;
    const QByteArray signature = installer.read(4);
    if (signature.size() < 2 || signature.left(2) != QByteArrayLiteral("PK"))
        return false;
    installer.close();

    MMCZip::ArchiveReader archive(installerPath);
    auto profileFile = archive.goToFile(QStringLiteral("install_profile.json"));
    if (!profileFile)
        return false;
    const QJsonDocument profileDocument = QJsonDocument::fromJson(profileFile->readAll());
    if (!profileDocument.isObject())
        return false;
    const QJsonObject profile = profileDocument.object();
    if (!profile.value(QStringLiteral("spec")).isDouble())
        return false;

    QList<InstallerLibrary> libraries;
    QSet<QString> paths;
    addInstallerLibraries(profile.value(QStringLiteral("libraries")).toArray(), libraries, paths);

    auto versionFile = archive.goToFile(QStringLiteral("version.json"));
    QString versionId = profile.value(QStringLiteral("minecraft")).toString();
    if (versionFile) {
        const QJsonDocument versionDocument = QJsonDocument::fromJson(versionFile->readAll());
        if (versionDocument.isObject()) {
            const QJsonObject version = versionDocument.object();
            if (!version.value(QStringLiteral("id")).toString().isEmpty())
                versionId = version.value(QStringLiteral("id")).toString();
            if (version.value(QStringLiteral("spec")).isDouble()) {
                addInstallerLibraries(version.value(QStringLiteral("libraries")).toArray(), libraries, paths);
                const QJsonObject serverDownload = version.value(QStringLiteral("downloads"))
                                                       .toObject().value(QStringLiteral("server")).toObject();
                addVanillaServerArtifact(serverDownload, versionId, libraries, paths);
            }
        }
    }

    const QJsonObject data = profile.value(QStringLiteral("data")).toObject();
    const QJsonObject minecraftJar = data.value(QStringLiteral("MINECRAFT_JAR")).toObject();
    if (!versionId.isEmpty()) {
        QJsonObject serverArtifact = minecraftJar.value(QStringLiteral("server")).toObject();
        if (serverArtifact.isEmpty())
            serverArtifact = minecraftJar;
        addVanillaServerArtifact(serverArtifact, versionId, libraries, paths);
    }
    bool hasVanillaServerJar = false;
    for (const InstallerLibrary &library : libraries) {
        if (library.path.startsWith(QStringLiteral("net/minecraft/server/"))
            && library.path.endsWith(QStringLiteral(".jar"))) {
            hasVanillaServerJar = true;
        }
    }
    if (!hasVanillaServerJar && minecraftJar.isEmpty()) {
        qInfo().noquote() << loaderName
                          << "installer does not expose a vanilla server jar URL and SHA-1; leaving it to the installer";
    } else if (!hasVanillaServerJar) {
        qInfo().noquote() << loaderName
                          << "installer vanilla server jar metadata is not a supported URL/SHA-1 record; leaving it to the installer";
    }

    QStringList downloadPaths;
    for (const InstallerLibrary &library : libraries) {
        const QString outputPath = QDir(m_destinationDir).filePath(
            QStringLiteral("libraries/") + library.path);
        if (fileMatchesSha1(outputPath, library.sha1))
            continue;
        downloadPaths.append(outputPath);
    }
    if (downloadPaths.isEmpty())
        return false;

    QString markerError;
    if (!writeLoaderInstallIncompleteMarker(&markerError)) {
        m_step = Step::Idle;
        finishDownload(false, markerError);
        return true;
    }

    m_pendingInstallerLoader = loaderName;
    m_pendingInstallerPath = installerPath;
    m_prefetchLibraryPaths = downloadPaths;
    m_prefetchLibraryHashes.clear();
    m_step = Step::DownloadingInstallerLibraries;
    emit statusMessage(tr("Downloading %1 %2 libraries...").arg(downloadPaths.size()).arg(loaderName));

#if defined(LAUNCHER_APPLICATION)
    if (auto *application = APPLICATION_DYN; application && application->network())
        m_downloadNetwork = application->network();
#endif
    auto job = NetJob::Ptr(new NetJob(tr("%1 installer libraries").arg(loaderName), m_downloadNetwork));
    job->setAskRetry(false);
    for (const InstallerLibrary &library : libraries) {
        const QString outputPath = QDir(m_destinationDir).filePath(
            QStringLiteral("libraries/") + library.path);
        if (fileMatchesSha1(outputPath, library.sha1))
            continue;
        m_prefetchLibraryHashes.insert(outputPath, library.sha1);
        auto download = Net::Download::makeFile(library.url, outputPath);
        download->addValidator(new Net::ChecksumValidator(
            QCryptographicHash::Sha1, QString::fromLatin1(library.sha1).toLower()));
        job->addNetAction(download);
    }
    connect(job.get(), &Task::progress, this, [this](qint64 current, qint64 total) {
        if (total <= 0)
            return;
        const int percentage = 50 + qBound(0, static_cast<int>(current * 4 / total), 4);
        emit progress(percentage);
    });
    connect(job.get(), &Task::succeeded, this, &ServerDownloader::onInstallerLibrariesPrefetched);
    connect(job.get(), &Task::failed, this, [this](const QString &) {
        int failedFiles = 0;
        for (const QString &path : m_prefetchLibraryPaths) {
            if (!fileMatchesSha1(path, m_prefetchLibraryHashes.value(path))) {
                QFile::remove(path);
                ++failedFiles;
            }
        }
        qWarning().noquote() << QStringLiteral("%1 library prefetch failed for %2 file(s); continuing with installer")
                                    .arg(m_pendingInstallerLoader)
                                    .arg(failedFiles);
        onInstallerLibrariesPrefetched();
    });
    m_fileDownloadJob = job;
    job->start();
    return true;
}

void ServerDownloader::onInstallerLibrariesPrefetched()
{
    retireFileDownloadJob(false);
    const QString loaderName = m_pendingInstallerLoader;
    const QString installerPath = m_pendingInstallerPath;
    m_pendingInstallerLoader.clear();
    m_pendingInstallerPath.clear();
    m_prefetchLibraryPaths.clear();
    m_prefetchLibraryHashes.clear();
    if (loaderName.isEmpty() || installerPath.isEmpty())
        return;
    startInstallerProcess(loaderName, installerPath);
}

void ServerDownloader::startInstallerProcess(const QString &loaderName,
                                              const QString &installerPath)
{
    emit statusMessage(tr("Running %1 installer (this may take a few minutes)...").arg(loaderName));
    emit progress(55);

    // Run the installer in --installServer mode
    QProcess *installer = new QProcess(this);
    m_installerProcess = installer;
    m_activeInstallerPath = installerPath;
    installer->setWorkingDirectory(m_destinationDir);

    auto completed = std::make_shared<bool>(false);
    connect(installer, &QProcess::errorOccurred, this,
            [this, installer, installerPath, loaderName, completed](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || *completed) {
            return;
        }
        *completed = true;
        if (m_installerProcess == installer)
            m_installerProcess = nullptr;
        m_activeInstallerPath.clear();
        const QString processError = installer->errorString();
        QFile::remove(installerPath);
        installer->deleteLater();
        m_step = Step::Idle;
        finishDownload(false, tr("%1 installer could not start: %2").arg(loaderName, processError));
    });
    connect(installer, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, installer, installerPath, loaderName, completed](int exitCode, QProcess::ExitStatus exitStatus) {
        if (*completed) {
            return;
        }
        *completed = true;
        if (m_installerProcess == installer)
            m_installerProcess = nullptr;
        m_activeInstallerPath.clear();
        installer->deleteLater();

        // Clean up installer
        QFile::remove(installerPath);

        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            m_step = Step::Idle;
            finishDownload(false, tr("%1 installer failed with exit code %2.").arg(loaderName).arg(exitCode));
            return;
        }

        QString validationError;
        if (!validateLoaderInstallation(loaderName, &validationError)) {
            m_step = Step::Idle;
            finishDownload(false, validationError);
            return;
        }

        const QString markerPath = serverLoaderInstallIncompleteMarkerPath(m_destinationDir);
        if (!QFile::remove(markerPath) && QFileInfo::exists(markerPath)) {
            m_step = Step::Idle;
            finishDownload(false, tr("Could not clear the incomplete loader installation marker."));
            return;
        }

        emit statusMessage(tr("%1 installation complete!").arg(loaderName));
        emit progress(100);
        finishDownload(true);
    });

    // Find Java
    QString markerError;
    if (!writeLoaderInstallIncompleteMarker(&markerError)) {
        m_installerProcess = nullptr;
        m_activeInstallerPath.clear();
        installer->deleteLater();
        m_step = Step::Idle;
        finishDownload(false, markerError);
        return;
    }
    installer->start(m_javaPath.isEmpty() ? "java" : m_javaPath,
                     QStringList() << "-jar" << installerPath << "--installServer");
}

// ==================== NeoForge ====================

void ServerDownloader::fetchNeoForgeVersions()
{
    m_step = Step::FetchingNeoForgeVersions;
    emit statusMessage(tr("Fetching NeoForge versions..."));

    const QUrl versionsUrl = usesLegacyNeoForgeCoordinates(m_version, QString())
        ? m_endpoints.neoForgeMavenBase.resolved(
              QUrl(QStringLiteral("net/neoforged/forge/maven-metadata.xml")))
        : m_endpoints.neoForgeVersions;
    QNetworkRequest request = createRequest(versionsUrl);
    m_currentReply = m_network->get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onNeoForgeVersionsFetched(const QByteArray &data)
{
    if (data.trimmed().startsWith('<')) {
        QString error;
        const QStringList builds = parseAvailableBuilds(
            QStringLiteral("neoforge"), m_version, data, &error);
        if (builds.isEmpty()) {
            m_step = Step::Idle;
            finishDownload(false, error.isEmpty()
                ? tr("No NeoForge version found for Minecraft %1.").arg(m_version)
                : error);
            return;
        }
        downloadNeoForgeInstaller(builds.first());
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        m_step = Step::Idle;
        finishDownload(false, tr("Failed to parse NeoForge version list."));
        return;
    }

    QString error;
    const QStringList builds = parseAvailableBuilds(
        QStringLiteral("neoforge"), m_version, data, &error);
    if (builds.isEmpty()) {
        m_step = Step::Idle;
        finishDownload(false, error.isEmpty()
            ? tr("No NeoForge version found for Minecraft %1.").arg(m_version)
            : error);
        return;
    }

    downloadNeoForgeInstaller(builds.first());
}

void ServerDownloader::downloadNeoForgeInstaller(const QString &neoforgeVersion)
{
    m_resolvedLoaderVersion = neoforgeVersion;
    const bool legacyCoordinates = usesLegacyNeoForgeCoordinates(
        m_version, neoforgeVersion);
    const QString installerRelativePath = legacyCoordinates
        ? QStringLiteral(
              "net/neoforged/forge/%1-%2/forge-%1-%2-installer.jar")
              .arg(m_version, neoforgeVersion)
        : QStringLiteral(
              "net/neoforged/neoforge/%1/neoforge-%1-installer.jar")
              .arg(neoforgeVersion);
    const QUrl installerUrl = m_endpoints.neoForgeMavenBase.resolved(
        QUrl(installerRelativePath));

    QString installerPath = QDir(m_destinationDir).filePath("neoforge-installer.jar");

    cleanUp();
    m_pendingInstallerUrl = installerUrl;
    m_pendingInstallerPath = installerPath;
    fetchNeoForgeInstallerChecksum(installerUrl, installerPath);
}

void ServerDownloader::fetchNeoForgeInstallerChecksum(const QUrl &installerUrl,
                                                       const QString &installerPath)
{
    QUrl checksumUrl = installerUrl;
    checksumUrl.setPath(checksumUrl.path() + QStringLiteral(".sha1"));
    m_pendingInstallerUrl = installerUrl;
    m_pendingInstallerPath = installerPath;
    m_step = Step::FetchingNeoForgeInstallerChecksum;
    emit statusMessage(tr("Fetching NeoForge installer checksum..."));
    m_currentReply = m_network->get(createRequest(checksumUrl));
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onNeoForgeInstallerChecksumFetched(const QByteArray &data)
{
    const QStringList tokens = QString::fromLatin1(data).split(
        QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const QByteArray token = tokens.isEmpty() ? QByteArray() : tokens.first().toLatin1();
    if (!isSha1(token)) {
        finishDownload(false, tr("The NeoForge installer checksum is malformed."));
        return;
    }
    m_step = Step::DownloadingNeoForgeInstaller;
    emit statusMessage(tr("Downloading NeoForge installer..."));
    startFileDownload(m_pendingInstallerUrl, m_pendingInstallerPath, token,
                      QCryptographicHash::Sha1, false);
}

void ServerDownloader::onNeoForgeInstallerDownloaded()
{
    const QString installerPath = QDir(m_destinationDir).filePath("neoforge-installer.jar");
    if (prefetchModernInstallerLibraries(installerPath, QStringLiteral("NeoForge")))
        return;
    startInstallerProcess(QStringLiteral("NeoForge"), installerPath);
}
