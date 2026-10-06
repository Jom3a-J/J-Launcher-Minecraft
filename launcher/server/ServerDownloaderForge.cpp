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

// Forge and NeoForge: installer download, library prefetch, running the installer and
// checking what it installed.

#include "ServerDownloader.h"
#include "ServerDownloaderProvider.h"
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
#include "net/Request.h"
#include "archive/ArchiveReader.h"
#include <algorithm>
#include <utility>

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
/// Forge and NeoForge both fetch an installer with a published SHA-1 and run it in the server folder.
class LoaderInstallerProvider : public ServerDownloaderProvider
{
public:
    LoaderInstallerProvider(ServerDownloader &downloader, QString loaderName, QString installerFileName)
        : ServerDownloaderProvider(downloader)
        , m_loaderName(std::move(loaderName))
        , m_installerFileName(std::move(installerFileName))
    {
    }

    bool onRequestFailed(int httpStatus, const QString &error) override
    {
        if (m_installerStep != InstallerStep::FetchingChecksum)
            return false;
        if (httpStatus == 404) {
            status(tr("No checksum is published for this installer; continuing without verification."));
            m_installerStep = InstallerStep::DownloadingInstaller;
            downloadFile(m_installerUrl, installerPath(), {}, QCryptographicHash::Sha1, false);
            return true;
        }
        stopActiveWork();
        fail(tr("Failed to fetch the installer checksum: %1").arg(error));
        return true;
    }

    void onFileDownloaded() override
    {
        if (m_installerStep != InstallerStep::DownloadingInstaller)
            return;
        progress(50);
        runLoaderInstaller(m_loaderName, installerPath());
    }

protected:
    /// Fetches the installer's published SHA-1, then the installer itself.
    void fetchInstaller(const QUrl &installerUrl)
    {
        stopActiveWork();
        m_installerUrl = installerUrl;
        QUrl checksumUrl = installerUrl;
        checksumUrl.setPath(checksumUrl.path() + QStringLiteral(".sha1"));
        m_installerStep = InstallerStep::FetchingChecksum;
        status(tr("Fetching %1 installer checksum...").arg(m_loaderName));
        request(checksumUrl);
    }

    /// Handles the reply while the installer is being fetched; false before that.
    bool onInstallerReply(const QByteArray &data)
    {
        if (m_installerStep != InstallerStep::FetchingChecksum)
            return false;
        const QStringList tokens = QString::fromLatin1(data).split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        const QByteArray token = tokens.isEmpty() ? QByteArray() : tokens.first().toLatin1();
        if (!isSha1(token)) {
            fail(tr("The %1 installer checksum is malformed.").arg(m_loaderName));
            return true;
        }
        m_installerStep = InstallerStep::DownloadingInstaller;
        status(tr("Downloading %1 installer...").arg(m_loaderName));
        downloadFile(m_installerUrl, installerPath(), token, QCryptographicHash::Sha1, false);
        return true;
    }

    bool fetchingInstaller() const { return m_installerStep != InstallerStep::None; }

    QString installerStep() const
    {
        return m_installerStep == InstallerStep::DownloadingInstaller
            ? tr("downloading the loader installer") : tr("server setup");
    }

    const QString &loaderName() const { return m_loaderName; }

private:
    QString installerPath() const { return QDir(destinationDir()).filePath(m_installerFileName); }

    enum class InstallerStep { None, FetchingChecksum, DownloadingInstaller };
    InstallerStep m_installerStep = InstallerStep::None;
    QString m_loaderName;
    QString m_installerFileName;
    QUrl m_installerUrl;
};

// ==================== Forge ====================

class ForgeServerProvider final : public LoaderInstallerProvider
{
public:
    explicit ForgeServerProvider(ServerDownloader &downloader)
        : LoaderInstallerProvider(downloader, QStringLiteral("Forge"), QStringLiteral("forge-installer.jar"))
    {
    }

    QUrl versionListUrl() const override { return mavenMetadataUrl(); }
    QUrl buildListUrl(const QString &) const override { return mavenMetadataUrl(); }

    void start() override
    {
        if (requestedLoaderVersion().isEmpty()) {
            m_step = Step::FetchingVersions;
            status(tr("Fetching Forge versions..."));
            // Maven metadata covers every published Forge build. The promotions feed
            // only contains selected recommended/latest Minecraft versions and can
            // reject otherwise valid server combinations.
            request(mavenMetadataUrl());
        } else {
            downloadInstaller(requestedLoaderVersion());
        }
    }

    void onReply(const QByteArray &data) override
    {
        if (onInstallerReply(data))
            return;
        switch (m_step) {
            case Step::FetchingVersions:
                onVersionsFetched(data);
                return;
            case Step::ResolvingInstallerMetadata:
                prepareInstaller(m_forgeVersion, resolveMavenVersion(data, m_forgeVersion));
                return;
            case Step::FetchingVanillaManifest:
                onVanillaManifestFetched(data);
                return;
            case Step::FetchingVanillaDetails:
                onVanillaDetailsFetched(data);
                return;
            case Step::DownloadingVanillaJar:
                return;
        }
    }

    bool onRequestFailed(int httpStatus, const QString &error) override
    {
        if (!fetchingInstaller() && m_step == Step::ResolvingInstallerMetadata) {
            prepareInstaller(m_forgeVersion, QString());
            return true;
        }
        return LoaderInstallerProvider::onRequestFailed(httpStatus, error);
    }

    void onFileDownloaded() override
    {
        if (!fetchingInstaller() && m_step == Step::DownloadingVanillaJar) {
            fetchInstallerFor(m_forgeVersion, m_mavenVersion);
            return;
        }
        LoaderInstallerProvider::onFileDownloaded();
    }

    QString currentStep() const override
    {
        if (fetchingInstaller())
            return installerStep();
        switch (m_step) {
            case Step::FetchingVersions:
            case Step::ResolvingInstallerMetadata:
                return tr("resolving the loader installer");
            case Step::FetchingVanillaManifest:
                return tr("fetching the version manifest");
            case Step::FetchingVanillaDetails:
                return tr("fetching version details");
            case Step::DownloadingVanillaJar:
                return tr("downloading the vanilla server JAR for Forge");
        }
        return tr("server setup");
    }

private:
    QUrl mavenMetadataUrl() const
    {
        return endpoints().forgeMavenBase.resolved(
            QUrl(QStringLiteral("net/minecraftforge/forge/maven-metadata.xml")));
    }

    void onVersionsFetched(const QByteArray &data)
    {
        if (data.trimmed().startsWith('<')) {
            QString error;
            const QStringList builds = ServerDownloader::parseAvailableBuilds(
                QStringLiteral("forge"), version(), data, &error);
            if (builds.isEmpty()) {
                fail(error.isEmpty()
                    ? tr("No Forge version found for Minecraft %1.").arg(version())
                    : error);
                return;
            }
            prepareInstaller(builds.first(), resolveMavenVersion(data, builds.first()));
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isNull()) {
            fail(tr("Failed to parse Forge promotions."));
            return;
        }

        QJsonObject promos = doc.object()["promos"].toObject();

        // Try recommended first, then latest
        QString forgeVersion;
        QString recKey = version() + "-recommended";
        QString latKey = version() + "-latest";

        if (promos.contains(recKey)) {
            forgeVersion = promos[recKey].toString();
        } else if (promos.contains(latKey)) {
            forgeVersion = promos[latKey].toString();
        }

        if (forgeVersion.isEmpty()) {
            fail(tr("No Forge version found for Minecraft %1.").arg(version()));
            return;
        }

        downloadInstaller(forgeVersion);
    }

    void downloadInstaller(const QString &forgeVersion)
    {
        setResolvedLoaderVersion(forgeVersion);
        m_forgeVersion = forgeVersion;
        m_step = Step::ResolvingInstallerMetadata;
        status(tr("Resolving the Forge installer version..."));
        request(mavenMetadataUrl());
    }

    QString resolveMavenVersion(const QByteArray &data, const QString &forgeVersion) const
    {
        QXmlStreamReader xml(data);
        const QString exactVersion = version() + QLatin1Char('-') + forgeVersion;
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

    void prepareInstaller(const QString &forgeVersion, const QString &mavenVersion)
    {
        setResolvedLoaderVersion(forgeVersion);
        m_forgeVersion = forgeVersion;
        m_mavenVersion = mavenVersion;

        // Installers for Minecraft before 1.13 expect the vanilla server jar beside them,
        // and the address they would fetch it from no longer works.
        if (isBeforeMinecraft113(version())) {
            m_step = Step::FetchingVanillaManifest;
            status(tr("Fetching Mojang version manifest..."));
            request(endpoints().vanillaManifest);
            return;
        }

        fetchInstallerFor(forgeVersion, mavenVersion);
    }

    void onVanillaManifestFetched(const QByteArray &data)
    {
        QString detailsUrl;
        QString error;
        if (!findVanillaVersionDetails(data, version(), &detailsUrl, &error)) {
            fail(error);
            return;
        }
        m_step = Step::FetchingVanillaDetails;
        status(tr("Fetching version details..."));
        request(QUrl(detailsUrl));
    }

    void onVanillaDetailsFetched(const QByteArray &data)
    {
        QString jarUrl;
        QByteArray sha1;
        QString error;
        if (!findVanillaServerJar(data, &jarUrl, &sha1, &error)) {
            fail(error);
            return;
        }
        const QString jarPath = QDir(destinationDir()).filePath(
            QStringLiteral("minecraft_server.%1.jar").arg(version()));
        if (fileMatchesSha1(jarPath, sha1)) {
            fetchInstallerFor(m_forgeVersion, m_mavenVersion);
            return;
        }
        m_step = Step::DownloadingVanillaJar;
        stopActiveWork();
        status(tr("Downloading the vanilla server jar for the Forge installer..."));
        downloadFile(QUrl(jarUrl), jarPath, sha1, QCryptographicHash::Sha1, true);
    }

    void fetchInstallerFor(const QString &forgeVersion, const QString &mavenVersion)
    {
        // Download the exact loader selected by the modpack when one is supplied.
        const QString resolvedMavenVersion = mavenVersion.isEmpty()
            ? version() + QLatin1Char('-') + forgeVersion : mavenVersion;
        fetchInstaller(endpoints().forgeMavenBase.resolved(
            QUrl(QStringLiteral("net/minecraftforge/forge/%1/forge-%1-installer.jar")
                     .arg(resolvedMavenVersion))));
    }

    enum class Step {
        FetchingVersions,
        ResolvingInstallerMetadata,
        FetchingVanillaManifest,
        FetchingVanillaDetails,
        DownloadingVanillaJar
    };
    Step m_step = Step::FetchingVersions;
    QString m_forgeVersion;
    QString m_mavenVersion;
};

// ==================== NeoForge ====================

class NeoForgeServerProvider final : public LoaderInstallerProvider
{
public:
    explicit NeoForgeServerProvider(ServerDownloader &downloader)
        : LoaderInstallerProvider(downloader, QStringLiteral("NeoForge"), QStringLiteral("neoforge-installer.jar"))
    {
    }

    QUrl versionListUrl() const override { return endpoints().neoForgeVersions; }

    QUrl buildListUrl(const QString &minecraftVersion) const override
    {
        // NeoForge 1.20.1 builds are listed under the Forge-style coordinates.
        return usesLegacyNeoForgeCoordinates(minecraftVersion, QString())
            ? endpoints().neoForgeMavenBase.resolved(
                  QUrl(QStringLiteral("net/neoforged/forge/maven-metadata.xml")))
            : endpoints().neoForgeVersions;
    }

    void start() override
    {
        if (requestedLoaderVersion().isEmpty()) {
            status(tr("Fetching NeoForge versions..."));
            request(buildListUrl(version()));
        } else {
            downloadInstaller(requestedLoaderVersion());
        }
    }

    void onReply(const QByteArray &data) override
    {
        if (onInstallerReply(data))
            return;
        onVersionsFetched(data);
    }

    QString currentStep() const override
    {
        return fetchingInstaller() ? installerStep() : tr("resolving the loader installer");
    }

private:
    void onVersionsFetched(const QByteArray &data)
    {
        if (!data.trimmed().startsWith('<') && QJsonDocument::fromJson(data).isNull()) {
            fail(tr("Failed to parse NeoForge version list."));
            return;
        }

        QString error;
        const QStringList builds = ServerDownloader::parseAvailableBuilds(
            QStringLiteral("neoforge"), version(), data, &error);
        if (builds.isEmpty()) {
            fail(error.isEmpty()
                ? tr("No NeoForge version found for Minecraft %1.").arg(version())
                : error);
            return;
        }

        downloadInstaller(builds.first());
    }

    void downloadInstaller(const QString &neoforgeVersion)
    {
        setResolvedLoaderVersion(neoforgeVersion);
        const bool legacyCoordinates = usesLegacyNeoForgeCoordinates(version(), neoforgeVersion);
        const QString installerRelativePath = legacyCoordinates
            ? QStringLiteral(
                  "net/neoforged/forge/%1-%2/forge-%1-%2-installer.jar")
                  .arg(version(), neoforgeVersion)
            : QStringLiteral(
                  "net/neoforged/neoforge/%1/neoforge-%1-installer.jar")
                  .arg(neoforgeVersion);
        fetchInstaller(endpoints().neoForgeMavenBase.resolved(QUrl(installerRelativePath)));
    }
};

}  // namespace

std::unique_ptr<ServerDownloaderProvider> makeForgeServerProvider(ServerDownloader &downloader)
{
    return std::make_unique<ForgeServerProvider>(downloader);
}

std::unique_ptr<ServerDownloaderProvider> makeNeoForgeServerProvider(ServerDownloader &downloader)
{
    return std::make_unique<NeoForgeServerProvider>(downloader);
}

// ==================== Running a Forge-style installer ====================

void ServerDownloader::runLoaderInstaller(const QString &loaderName, const QString &installerPath)
{
    if (prefetchModernInstallerLibraries(installerPath, loaderName))
        return;
    startInstallerProcess(loaderName, installerPath);
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
    const QJsonDocument profileDocument = QJsonDocument::fromJson((*profileFile)->readAll().value_or(QByteArray()));
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
        const QJsonDocument versionDocument = QJsonDocument::fromJson((*versionFile)->readAll().value_or(QByteArray()));
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
        finishDownload(false, markerError);
        return true;
    }

    m_pendingInstallerLoader = loaderName;
    m_pendingInstallerPath = installerPath;
    m_prefetchLibraryPaths = downloadPaths;
    m_prefetchLibraryHashes.clear();
    m_fileDownload = FileDownload::InstallerLibraries;
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
        auto download = Net::Request::makeFile(library.url, outputPath);
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
                finishDownload(false, tr("%1 installer failed with exit code %2.").arg(loaderName).arg(exitCode));
            return;
        }

        QString validationError;
        if (!validateLoaderInstallation(loaderName, &validationError)) {
                finishDownload(false, validationError);
            return;
        }

        const QString markerPath = serverLoaderInstallIncompleteMarkerPath(m_destinationDir);
        if (!QFile::remove(markerPath) && QFileInfo::exists(markerPath)) {
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
        finishDownload(false, markerError);
        return;
    }
    installer->start(m_javaPath.isEmpty() ? "java" : m_javaPath,
                     QStringList() << "-jar" << installerPath << "--installServer");
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
