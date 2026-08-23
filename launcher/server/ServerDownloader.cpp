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
#include "BuildConfig.h"
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QUrl>
#include <QProcess>
#include <QDirIterator>
#include <QRegularExpression>
#include <QSet>
#include <QVersionNumber>
#include <QXmlStreamReader>
#include <algorithm>

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

bool usesLegacyNeoForgeCoordinates(const QString &minecraftVersion,
                                   const QString &loaderVersion)
{
    // NeoForge's first 1.20.1 releases retained the Forge-style artifact
    // name and combined Minecraft/loader coordinate under net.neoforged.
    // Newer releases use net.neoforged:neoforge:<loader-version>.
    return minecraftVersion.trimmed() == QStringLiteral("1.20.1")
        || loaderVersion.trimmed().startsWith(QStringLiteral("47."));
}

void sortBuildVersions(QStringList &builds)
{
    builds.removeAll(QString());
    QSet<QString> unique(builds.begin(), builds.end());
    builds = QStringList(unique.begin(), unique.end());
    std::sort(builds.begin(), builds.end(), [](const QString &left, const QString &right) {
        bool leftNumber = false;
        bool rightNumber = false;
        const qlonglong leftValue = left.toLongLong(&leftNumber);
        const qlonglong rightValue = right.toLongLong(&rightNumber);
        if (leftNumber && rightNumber) return leftValue > rightValue;
        const int comparison = QVersionNumber::compare(QVersionNumber::fromString(left),
                                                        QVersionNumber::fromString(right));
        return comparison == 0 ? left > right : comparison > 0;
    });
}
}

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
    m_version = version;
    m_type = type.toLower();
    m_destinationDir = destinationDir;
    m_targetJarPath = QDir(destinationDir).filePath("server.jar");
    m_javaPath = javaPath;
    m_loaderVersion = loaderVersion.trimmed();
    m_resolvedLoaderVersion.clear();
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
        emit finished(false, tr("Unsupported server type: %1").arg(type));
    }
}

void ServerDownloader::fetchAvailableVersions(const QString &type)
{
    cleanUp();
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
    const bool versionRequest = isVersionListStep();
    const bool buildRequest = isBuildListStep();
    cleanUp();
    m_step = Step::Idle;
    if (versionRequest) {
        emit versionsFailed(tr("Version request cancelled."));
    } else if (buildRequest) {
        emit buildsFailed(tr("Build request cancelled."));
    } else {
        emit finished(false, tr("Download cancelled."));
    }
}

void ServerDownloader::cleanUp()
{
    if (m_currentReply) {
        m_currentReply->disconnect(this);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
    if (m_outputFile && m_outputFile->isOpen()) {
        m_outputFile->cancelWriting();
    }
    m_outputFile.reset();
    m_downloadHash.reset();
    m_expectedHash.clear();
    m_fileWriteFailed = false;
}

void ServerDownloader::handleReply(QNetworkReply *reply)
{
    if (!reply) {
        return;
    }

    m_currentReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        QString errorStr = reply->errorString();
        reply->deleteLater();
        cleanUp();
        failCurrentRequest(tr("Network error: %1").arg(errorStr));
        return;
    }

    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    // readyRead normally drains the reply as it arrives, but a small response
    // can arrive only with finished. Preserve those final bytes so server JARs
    // and installers are never written as empty or truncated files.
    if ((m_step == Step::DownloadingJar || m_step == Step::DownloadingForgeInstaller ||
         m_step == Step::DownloadingNeoForgeInstaller) &&
        m_outputFile && m_outputFile->isOpen() && !responseData.isEmpty()) {
        if (!appendDownloadData(responseData)) {
            emit finished(false, tr("Failed to write the downloaded server file."));
            m_step = Step::Idle;
            return;
        }
    }

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
        case Step::FetchingForgeVersions:
            onForgeVersionsFetched(responseData);
            break;
        case Step::DownloadingForgeInstaller: {
            QString errorMessage;
            if (!finalizeDownloadedFile(&errorMessage)) {
                emit finished(false, errorMessage);
                m_step = Step::Idle;
                break;
            }
            onForgeInstallerDownloaded();
            break;
        }
        case Step::FetchingNeoForgeVersions:
            onNeoForgeVersionsFetched(responseData);
            break;
        case Step::DownloadingNeoForgeInstaller: {
            QString errorMessage;
            if (!finalizeDownloadedFile(&errorMessage)) {
                emit finished(false, errorMessage);
                m_step = Step::Idle;
                break;
            }
            onNeoForgeInstallerDownloaded();
            break;
        }
        case Step::DownloadingJar:
        {
            QString errorMessage;
            if (!finalizeDownloadedFile(&errorMessage)) {
                emit finished(false, errorMessage);
                m_step = Step::Idle;
                break;
            }
        }
            // Data already written by readyRead lambda — just close
            emit statusMessage(tr("Download complete!"));
            emit progress(100);
            emit finished(true);
            m_step = Step::Idle;
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

    m_outputFile.reset(new QSaveFile(outputPath));
    if (!m_outputFile->open(QIODevice::WriteOnly)) {
        emit finished(false, tr("Failed to open destination file for writing: %1").arg(outputPath));
        m_outputFile.reset();
        return;
    }

    m_step = Step::DownloadingJar;
    m_expectedHash = expectedHash;
    m_fileWriteFailed = false;
    if (!m_expectedHash.isEmpty()) {
        m_downloadHash.reset(new QCryptographicHash(hashAlgorithm));
    }
    emit statusMessage(tr("Downloading server jar..."));

    QNetworkRequest request = createRequest(QUrl(url));
    m_currentReply = m_network->get(request);

    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
    connect(m_currentReply, &QNetworkReply::downloadProgress, this, [this](qint64 bytesReceived, qint64 bytesTotal) {
        if (m_step == Step::DownloadingJar && bytesTotal > 0) {
            int pct = static_cast<int>((bytesReceived * 100) / bytesTotal);
            emit progress(pct);
        }
    });
    connect(m_currentReply, &QNetworkReply::readyRead, this, [this]() {
        if (m_currentReply && m_outputFile && m_outputFile->isOpen()) {
            appendDownloadData(m_currentReply->readAll());
        }
    });
}

bool ServerDownloader::appendDownloadData(const QByteArray &data)
{
    if (data.isEmpty()) {
        return true;
    }
    if (!m_outputFile || m_outputFile->write(data) != data.size()) {
        m_fileWriteFailed = true;
        return false;
    }
    if (m_downloadHash) {
        m_downloadHash->addData(data);
    }
    return true;
}

bool ServerDownloader::finalizeDownloadedFile(QString *errorMessage)
{
    if (!m_outputFile) {
        *errorMessage = tr("The downloaded server file was not available for verification.");
        return false;
    }
    if (m_fileWriteFailed) {
        m_outputFile->cancelWriting();
        *errorMessage = tr("Failed to write the downloaded server file.");
        return false;
    }
    if (m_downloadHash && m_downloadHash->result().toHex().compare(m_expectedHash, Qt::CaseInsensitive) != 0) {
        m_outputFile->cancelWriting();
        *errorMessage = tr("Download verification failed: the server file hash did not match the provider's value.");
        return false;
    }
    if (!m_outputFile->commit()) {
        *errorMessage = tr("Failed to finalize the downloaded server file.");
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
        m_step = Step::Idle;
        emit finished(false, contextualMessage);
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
            step = tr("resolving the loader installer");
            break;
        case Step::DownloadingForgeInstaller:
        case Step::DownloadingNeoForgeInstaller:
            step = tr("downloading the loader installer");
            break;
        case Step::DownloadingJar:
            step = tr("downloading the server JAR");
            break;
        default:
            step = tr("server setup");
            break;
    }
    return tr("%1 %2").arg(provider, step);
}

QStringList ServerDownloader::parseAvailableVersions(const QString &type, const QByteArray &data,
                                                     QString *errorMessage)
{
    const QString provider = type.trimmed().toLower();
    if (provider == QStringLiteral("forge") && data.trimmed().startsWith('<')) {
        QStringList versions;
        QXmlStreamReader xml(data);
        const QRegularExpression coordinatePattern(
            QStringLiteral("^((?:1\\.)?\\d+(?:\\.\\d+){0,2})-(.+)$"));
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QStringLiteral("version")) {
                const QRegularExpressionMatch match = coordinatePattern.match(
                    xml.readElementText().trimmed());
                if (match.hasMatch()) {
                    versions.append(match.captured(1));
                }
            }
        }
        if (xml.hasError()) {
            if (errorMessage) {
                *errorMessage = tr("The Forge version response was not valid Maven metadata.");
            }
            return {};
        }
        versions.removeDuplicates();
        std::sort(versions.begin(), versions.end(), [](const QString &left,
                                                        const QString &right) {
            const int comparison = QVersionNumber::compare(
                QVersionNumber::fromString(left), QVersionNumber::fromString(right));
            return comparison == 0 ? left > right : comparison > 0;
        });
        if (versions.isEmpty() && errorMessage) {
            *errorMessage = tr("The Forge service returned no supported Minecraft versions.");
        }
        return versions;
    }

    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (document.isNull()) {
        if (errorMessage) {
            *errorMessage = tr("The %1 version response was not valid JSON.").arg(type);
        }
        return {};
    }

    QStringList versions;
    if (provider == "vanilla") {
        for (const QJsonValue &value : document.object()["versions"].toArray()) {
            const QJsonObject version = value.toObject();
            versions.append(version["id"].toString());
        }
    } else if (provider == "paper") {
        const QJsonObject groupedVersions = document.object()["versions"].toObject();
        for (const QJsonValue &group : groupedVersions) {
            for (const QJsonValue &version : group.toArray()) {
                versions.append(version.toString());
            }
        }
    } else if (provider == "fabric") {
        for (const QJsonValue &value : document.array()) {
            const QJsonObject version = value.toObject();
            versions.append(version["version"].toString());
        }
    } else if (provider == "purpur") {
        for (const QJsonValue &version : document.object()["versions"].toArray()) {
            versions.append(version.toString());
        }
    } else if (provider == "forge") {
        const QJsonObject promotions = document.object()["promos"].toObject();
        for (auto iterator = promotions.constBegin(); iterator != promotions.constEnd(); ++iterator) {
            QString key = iterator.key();
            if (key.endsWith("-recommended")) {
                key.chop(QString("-recommended").size());
                versions.append(key);
            } else if (key.endsWith("-latest")) {
                key.chop(QString("-latest").size());
                versions.append(key);
            }
        }
    } else if (provider == "neoforge") {
        for (const QJsonValue &value : document.object()["versions"].toArray()) {
            const QStringList parts = value.toString().split(QRegularExpression("[.-]"), Qt::SkipEmptyParts);
            if (parts.size() < 2) {
                continue;
            }
            bool majorValid = false;
            const int major = parts.at(0).toInt(&majorValid);
            if (!majorValid) {
                continue;
            }
            versions.append(major >= 26
                                ? QString("%1.%2").arg(parts.at(0), parts.at(1))
                                : QString("1.%1.%2").arg(parts.at(0), parts.at(1)));
        }
        // NeoForge 1.20.1 was published under the legacy
        // net.neoforged:forge coordinate and is therefore absent from the
        // modern net.neoforged:neoforge version endpoint. Its builds are
        // resolved from the legacy Maven metadata when selected.
        versions.append(QStringLiteral("1.20.1"));
    } else {
        if (errorMessage) {
            *errorMessage = tr("Unsupported server type: %1").arg(type);
        }
        return {};
    }

    versions.removeAll(QString());
    QSet<QString> uniqueVersions(versions.begin(), versions.end());
    versions = QStringList(uniqueVersions.begin(), uniqueVersions.end());
    std::sort(versions.begin(), versions.end(), [](const QString &left, const QString &right) {
        const int comparison = QVersionNumber::compare(QVersionNumber::fromString(left),
                                                        QVersionNumber::fromString(right));
        return comparison == 0 ? left > right : comparison > 0;
    });
    if (versions.isEmpty() && errorMessage) {
        *errorMessage = tr("The %1 service returned no supported Minecraft versions.").arg(type);
    }
    return versions;
}

QStringList ServerDownloader::parseAvailableBuilds(const QString &type, const QString &version,
                                                    const QByteArray &data, QString *errorMessage)
{
    const QString provider = type.trimmed().toLower();
    if ((provider == "forge" || provider == "neoforge")
        && data.trimmed().startsWith('<')) {
        QStringList builds;
        QXmlStreamReader xml(data);
        const QString prefix = version + '-';
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QStringLiteral("version")) {
                const QString publishedVersion = xml.readElementText().trimmed();
                if (publishedVersion.startsWith(prefix)) {
                    builds.append(publishedVersion.mid(prefix.size()));
                }
            }
        }
        if (xml.hasError()) {
            if (errorMessage) {
                *errorMessage = tr("The %1 build response was not valid Maven metadata.")
                                    .arg(type);
            }
            return {};
        }
        sortBuildVersions(builds);
        if (builds.isEmpty() && errorMessage) {
            *errorMessage = tr("The %1 service returned no builds for Minecraft %2.")
                                .arg(type, version);
        }
        return builds;
    }

    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (document.isNull()) {
        if (errorMessage) *errorMessage = tr("The %1 build response was not valid JSON.").arg(type);
        return {};
    }

    QStringList builds;
    if (provider == "paper") {
        const QJsonArray entries = document.isArray()
            ? document.array() : document.object()["builds"].toArray();
        for (const QJsonValue &value : entries) {
            const QJsonObject build = value.toObject();
            const int number = build["id"].toInt(build["build"].toInt());
            if (number > 0) builds.append(QString::number(number));
        }
    } else if (provider == "fabric") {
        for (const QJsonValue &value : document.array()) {
            builds.append(value.toObject()["loader"].toObject()["version"].toString());
        }
    } else if (provider == "purpur") {
        const QJsonArray entries = document.object()["builds"].toObject()["all"].toArray();
        for (const QJsonValue &value : entries) {
            builds.append(value.isString() ? value.toString() : QString::number(value.toInt()));
        }
    } else if (provider == "forge") {
        const QJsonObject promotions = document.object()["promos"].toObject();
        builds.append(promotions[version + "-recommended"].toString());
        builds.append(promotions[version + "-latest"].toString());
    } else if (provider == "neoforge") {
        QString minecraftPrefix = version;
        if (minecraftPrefix.startsWith("1.")) minecraftPrefix = minecraftPrefix.mid(2);
        for (const QJsonValue &value : document.object()["versions"].toArray()) {
            const QString candidate = value.toString();
            if (candidate.startsWith(minecraftPrefix + '.')
                || candidate.startsWith(minecraftPrefix + '-')) {
                builds.append(candidate);
            }
        }
    } else {
        if (errorMessage) *errorMessage = tr("Unsupported server type: %1").arg(type);
        return {};
    }

    sortBuildVersions(builds);
    if (builds.isEmpty() && errorMessage) {
        *errorMessage = tr("The %1 service returned no builds for Minecraft %2.").arg(type, version);
    }
    return builds;
}

// ==================== Vanilla ====================

void ServerDownloader::fetchVanillaManifest()
{
    m_step = Step::FetchingVanillaManifest;
    emit statusMessage(tr("Fetching Mojang version manifest..."));

    QNetworkRequest request = createRequest(m_endpoints.vanillaManifest);
    m_currentReply = m_network->get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onVanillaManifestFetched(const QByteArray &data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        emit finished(false, tr("Failed to parse version manifest."));
        return;
    }

    QJsonArray versions = doc.object()["versions"].toArray();
    QString versionUrl;

    for (const auto &val : versions) {
        QJsonObject verObj = val.toObject();
        if (verObj["id"].toString() == m_version) {
            versionUrl = verObj["url"].toString();
            break;
        }
    }

    if (versionUrl.isEmpty()) {
        emit finished(false, tr("Version '%1' not found in manifest.").arg(m_version));
        return;
    }

    fetchVanillaVersionJson(versionUrl);
}

void ServerDownloader::fetchVanillaVersionJson(const QString &url)
{
    m_step = Step::FetchingVanillaVersionJson;
    emit statusMessage(tr("Fetching version details..."));

    QNetworkRequest request = createRequest(QUrl(url));
    m_currentReply = m_network->get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onVanillaVersionJsonFetched(const QByteArray &data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        emit finished(false, tr("Failed to parse version details."));
        return;
    }

    QJsonObject downloads = doc.object()["downloads"].toObject();
    QJsonObject server = downloads["server"].toObject();
    QString jarUrl = server["url"].toString();
    QByteArray sha1 = server["sha1"].toString().toLatin1();

    if (jarUrl.isEmpty() || sha1.isEmpty()) {
        emit finished(false, tr("No server download URL found for this version."));
        return;
    }

    downloadFile(jarUrl, m_targetJarPath, sha1, QCryptographicHash::Sha1);
}

// ==================== Paper (v3 API) ====================

void ServerDownloader::fetchPaperBuilds()
{
    m_step = Step::FetchingPaperBuilds;
    emit statusMessage(tr("Fetching PaperMC builds..."));

    // Paper v3 API: get version details including builds
    const QUrl url = m_endpoints.paperApiBase.resolved(
        QUrl(QString("projects/paper/versions/%1/builds").arg(m_version)));
    QNetworkRequest request = createRequest(url);
    m_currentReply = m_network->get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onPaperBuildsFetched(const QByteArray &data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        emit finished(false, tr("Failed to parse Paper builds response."));
        return;
    }

    // Fill v3 returns the builds directly as an array. Keep the object form
    // as a fallback for older compatible endpoints.
    QJsonArray builds = doc.isArray() ? doc.array() : doc.object()["builds"].toArray();
    if (builds.isEmpty()) {
        const QString apiMessage = doc.isObject() ? doc.object()["message"].toString() : QString();
        emit finished(false, apiMessage.isEmpty()
                               ? tr("No Paper builds found for version %1.").arg(m_version)
                               : tr("Paper download service: %1").arg(apiMessage));
        return;
    }

    // Use an explicitly selected build when supplied. Otherwise prefer the
    // newest stable build, falling back to the newest build of any channel.
    QJsonObject selectedBuild;
    int selectedNumber = -1;
    for (const QJsonValue &value : builds) {
        const QJsonObject build = value.toObject();
        const int number = build["id"].toInt(build["build"].toInt());
        if (!m_loaderVersion.isEmpty() && QString::number(number) == m_loaderVersion) {
            selectedBuild = build;
            break;
        }
        if (m_loaderVersion.isEmpty() && build["channel"].toString() == "STABLE"
            && number > selectedNumber) {
            selectedBuild = build;
            selectedNumber = number;
        }
    }
    if (selectedBuild.isEmpty() && m_loaderVersion.isEmpty()) {
        for (const QJsonValue &value : builds) {
            const QJsonObject build = value.toObject();
            const int number = build["id"].toInt(build["build"].toInt());
            if (number > selectedNumber) {
                selectedBuild = build;
                selectedNumber = number;
            }
        }
    }
    if (selectedBuild.isEmpty()) {
        emit finished(false, tr("Paper build %1 is not published for Minecraft %2.")
                                 .arg(m_loaderVersion, m_version));
        return;
    }
    const int buildNumber = selectedBuild["id"].toInt(selectedBuild["build"].toInt());
    const QString channel = selectedBuild["channel"].toString();
    m_resolvedLoaderVersion = m_loaderVersion.isEmpty()
        ? QString::number(buildNumber) : m_loaderVersion;

    QJsonObject downloads = selectedBuild["downloads"].toObject();

    // Try to find direct download URL in the response
    // v3 uses "server:default" key for the server jar
    QString downloadUrl;
    QByteArray sha256;
    if (downloads.contains("server:default")) {
        QJsonObject serverDownload = downloads["server:default"].toObject();
        downloadUrl = serverDownload["url"].toString();
        sha256 = serverDownload["checksums"].toObject()["sha256"].toString().toLatin1();
    } else if (downloads.contains("application")) {
        QJsonObject appDownload = downloads["application"].toObject();
        downloadUrl = appDownload["url"].toString();
        sha256 = appDownload["checksums"].toObject()["sha256"].toString().toLatin1();
    }

    if (downloadUrl.isEmpty() || sha256.isEmpty()) {
        emit finished(false, tr("Paper build %1 does not provide a server download.").arg(buildNumber));
        return;
    }

    emit statusMessage(tr("Downloading Paper %1 build %2%3...")
                       .arg(m_version)
                       .arg(buildNumber)
                       .arg(channel.isEmpty() ? QString() : " (" + channel + ")"));
    downloadFile(downloadUrl, m_targetJarPath, sha256, QCryptographicHash::Sha256);
}

// ==================== Fabric ====================

void ServerDownloader::fetchFabricInstaller()
{
    m_step = Step::FetchingFabricInstallerList;
    emit statusMessage(tr("Fetching Fabric installer version..."));

    QNetworkRequest request = createRequest(
        m_endpoints.fabricApiBase.resolved(QUrl("versions/installer")));
    m_currentReply = m_network->get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onFabricInstallerFetched(const QByteArray &data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        emit finished(false, tr("Failed to parse Fabric installer list."));
        return;
    }

    QJsonArray array = doc.array();
    if (array.isEmpty()) {
        emit finished(false, tr("Fabric installer list is empty."));
        return;
    }

    // Grab latest stable installer
    QString installerVer;
    for (const auto &val : array) {
        QJsonObject obj = val.toObject();
        if (obj["stable"].toBool()) {
            installerVer = obj["version"].toString();
            break;
        }
    }

    if (installerVer.isEmpty()) {
        installerVer = array.first().toObject()["version"].toString();
    }

    m_fabricInstallerVer = installerVer;
    fetchFabricLoader(installerVer);
}

void ServerDownloader::fetchFabricLoader(const QString &installerVer)
{
    m_step = Step::FetchingFabricLoaderList;
    emit statusMessage(tr("Fetching Fabric loader version..."));

    const QUrl url = m_endpoints.fabricApiBase.resolved(
        QUrl(QString("versions/loader/%1").arg(m_version)));
    QNetworkRequest request = createRequest(url);
    m_currentReply = m_network->get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onFabricLoaderFetched(const QString &installerVer, const QByteArray &data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        emit finished(false, tr("Failed to parse Fabric loader list."));
        return;
    }

    QJsonArray array = doc.array();
    if (array.isEmpty()) {
        emit finished(false, tr("No Fabric loader found for version %1.").arg(m_version));
        return;
    }

    QString loaderVer;
    for (const QJsonValue &value : array) {
        const QString candidate =
            value.toObject().value("loader").toObject().value("version").toString();
        if (m_loaderVersion.isEmpty() || candidate == m_loaderVersion) {
            loaderVer = candidate;
            break;
        }
    }
    if (loaderVer.isEmpty()) {
        emit finished(false, tr("Fabric loader %1 is not published for Minecraft %2.")
                                 .arg(m_loaderVersion, m_version));
        return;
    }
    m_resolvedLoaderVersion = loaderVer;

    // Construct direct download URL for the server launcher jar
    const QUrl jarUrl = m_endpoints.fabricApiBase.resolved(
        QUrl(QString("versions/loader/%1/%2/%3/server/jar")
                 .arg(m_version, loaderVer, installerVer)));

    emit statusMessage(tr("Downloading Fabric server (loader %1)...").arg(loaderVer));
    downloadFile(jarUrl.toString(), m_targetJarPath);
}

// ==================== Purpur ====================

void ServerDownloader::fetchPurpurBuilds()
{
    m_step = Step::FetchingPurpurBuilds;
    emit statusMessage(tr("Fetching Purpur builds..."));

    const QUrl url = m_endpoints.purpurApiBase.resolved(
        QUrl(QString("purpur/%1").arg(m_version)));
    QNetworkRequest request = createRequest(url);
    m_currentReply = m_network->get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onPurpurBuildsFetched(const QByteArray &data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        emit finished(false, tr("Failed to parse Purpur builds."));
        return;
    }

    QJsonObject obj = doc.object();
    QJsonObject buildsObj = obj["builds"].toObject();
    QString latestBuild = m_loaderVersion.isEmpty()
        ? buildsObj["latest"].toString() : m_loaderVersion;

    if (!m_loaderVersion.isEmpty()) {
        bool published = false;
        for (const QJsonValue &value : buildsObj["all"].toArray()) {
            const QString candidate = value.isString() ? value.toString() : QString::number(value.toInt());
            if (candidate == m_loaderVersion) {
                published = true;
                break;
            }
        }
        if (!published) {
            emit finished(false, tr("Purpur build %1 is not published for Minecraft %2.")
                                     .arg(m_loaderVersion, m_version));
            return;
        }
    }

    if (latestBuild.isEmpty()) {
        emit finished(false, tr("No Purpur builds found for version %1.").arg(m_version));
        return;
    }
    m_resolvedLoaderVersion = latestBuild;

    const QUrl jarUrl = m_endpoints.purpurApiBase.resolved(
        QUrl(QString("purpur/%1/%2/download").arg(m_version, latestBuild)));

    emit statusMessage(tr("Downloading Purpur build %1...").arg(latestBuild));
    downloadFile(jarUrl.toString(), m_targetJarPath);
}

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
            emit finished(false, error.isEmpty()
                ? tr("No Forge version found for Minecraft %1.").arg(m_version)
                : error);
            return;
        }
        downloadForgeInstaller(builds.first());
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        m_step = Step::Idle;
        emit finished(false, tr("Failed to parse Forge promotions."));
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
        emit finished(false, tr("No Forge version found for Minecraft %1.").arg(m_version));
        return;
    }

    downloadForgeInstaller(forgeVersion);
}

void ServerDownloader::downloadForgeInstaller(const QString &forgeVersion)
{
    m_resolvedLoaderVersion = forgeVersion;
    // Download the exact loader selected by the modpack when one is supplied.
    const QUrl installerUrl = m_endpoints.forgeMavenBase.resolved(
        QUrl(QString("net/minecraftforge/forge/%1-%2/forge-%1-%2-installer.jar")
                 .arg(m_version, forgeVersion)));

    QString installerPath = QDir(m_destinationDir).filePath("forge-installer.jar");

    m_step = Step::DownloadingForgeInstaller;
    emit statusMessage(tr("Downloading Forge %1 installer...").arg(forgeVersion));

    // Download installer to disk
    cleanUp();
    m_outputFile.reset(new QSaveFile(installerPath));
    m_fileWriteFailed = false;
    if (!m_outputFile->open(QIODevice::WriteOnly)) {
        emit finished(false, tr("Failed to open installer file for writing."));
        m_outputFile.reset();
        return;
    }

    QNetworkRequest request = createRequest(installerUrl);
    m_currentReply = m_network->get(request);

    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
    connect(m_currentReply, &QNetworkReply::downloadProgress, this, [this](qint64 bytesReceived, qint64 bytesTotal) {
        if (bytesTotal > 0) {
            int pct = static_cast<int>((bytesReceived * 50) / bytesTotal); // 0-50% for download
            emit progress(pct);
        }
    });
    connect(m_currentReply, &QNetworkReply::readyRead, this, [this]() {
        if (m_currentReply && m_outputFile && m_outputFile->isOpen()) {
            appendDownloadData(m_currentReply->readAll());
        }
    });
}

void ServerDownloader::onForgeInstallerDownloaded()
{
    QString installerPath = QDir(m_destinationDir).filePath("forge-installer.jar");
    emit statusMessage(tr("Running Forge installer (this may take a few minutes)..."));
    emit progress(55);

    // Run the installer in --installServer mode
    QProcess *installer = new QProcess(this);
    installer->setWorkingDirectory(m_destinationDir);

    auto completed = std::make_shared<bool>(false);
    connect(installer, &QProcess::errorOccurred, this,
            [this, installer, installerPath, completed](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || *completed) {
            return;
        }
        *completed = true;
        const QString processError = installer->errorString();
        QFile::remove(installerPath);
        installer->deleteLater();
        m_step = Step::Idle;
        emit finished(false, tr("Forge installer could not start: %1").arg(processError));
    });
    connect(installer, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, installer, installerPath, completed](int exitCode, QProcess::ExitStatus exitStatus) {
        if (*completed) {
            return;
        }
        *completed = true;
        installer->deleteLater();

        // Clean up installer
        QFile::remove(installerPath);

        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            m_step = Step::Idle;
            emit finished(false, tr("Forge installer failed with exit code %1.").arg(exitCode));
            return;
        }

        QString validationError;
        if (!validateLoaderInstallation(QStringLiteral("Forge"), &validationError)) {
            if (!m_loaderScriptExistedBeforeInstall) {
                QFile::remove(QDir(m_destinationDir).filePath(platformLoaderScriptName()));
            }
            m_step = Step::Idle;
            emit finished(false, validationError);
            return;
        }

        emit statusMessage(tr("Forge installation complete!"));
        emit progress(100);
        m_step = Step::Idle;
        emit finished(true);
    });

    // Find Java
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
            emit finished(false, error.isEmpty()
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
        emit finished(false, tr("Failed to parse NeoForge version list."));
        return;
    }

    QString error;
    const QStringList builds = parseAvailableBuilds(
        QStringLiteral("neoforge"), m_version, data, &error);
    if (builds.isEmpty()) {
        m_step = Step::Idle;
        emit finished(false, error.isEmpty()
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

    m_step = Step::DownloadingNeoForgeInstaller;
    emit statusMessage(tr("Downloading NeoForge %1 installer...").arg(neoforgeVersion));

    cleanUp();
    m_outputFile.reset(new QSaveFile(installerPath));
    m_fileWriteFailed = false;
    if (!m_outputFile->open(QIODevice::WriteOnly)) {
        emit finished(false, tr("Failed to open installer file for writing."));
        m_outputFile.reset();
        return;
    }

    QNetworkRequest request = createRequest(installerUrl);
    m_currentReply = m_network->get(request);

    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
    connect(m_currentReply, &QNetworkReply::downloadProgress, this, [this](qint64 bytesReceived, qint64 bytesTotal) {
        if (bytesTotal > 0) {
            int pct = static_cast<int>((bytesReceived * 50) / bytesTotal);
            emit progress(pct);
        }
    });
    connect(m_currentReply, &QNetworkReply::readyRead, this, [this]() {
        if (m_currentReply && m_outputFile && m_outputFile->isOpen()) {
            appendDownloadData(m_currentReply->readAll());
        }
    });
}

void ServerDownloader::onNeoForgeInstallerDownloaded()
{
    QString installerPath = QDir(m_destinationDir).filePath("neoforge-installer.jar");
    emit statusMessage(tr("Running NeoForge installer (this may take a few minutes)..."));
    emit progress(55);

    QProcess *installer = new QProcess(this);
    installer->setWorkingDirectory(m_destinationDir);

    auto completed = std::make_shared<bool>(false);
    connect(installer, &QProcess::errorOccurred, this,
            [this, installer, installerPath, completed](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || *completed) {
            return;
        }
        *completed = true;
        const QString processError = installer->errorString();
        QFile::remove(installerPath);
        installer->deleteLater();
        m_step = Step::Idle;
        emit finished(false, tr("NeoForge installer could not start: %1").arg(processError));
    });
    connect(installer, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, installer, installerPath, completed](int exitCode, QProcess::ExitStatus exitStatus) {
        if (*completed) {
            return;
        }
        *completed = true;
        installer->deleteLater();
        QFile::remove(installerPath);

        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            m_step = Step::Idle;
            emit finished(false, tr("NeoForge installer failed with exit code %1.").arg(exitCode));
            return;
        }

        QString validationError;
        if (!validateLoaderInstallation(QStringLiteral("NeoForge"), &validationError)) {
            if (!m_loaderScriptExistedBeforeInstall) {
                QFile::remove(QDir(m_destinationDir).filePath(platformLoaderScriptName()));
            }
            m_step = Step::Idle;
            emit finished(false, validationError);
            return;
        }

        emit statusMessage(tr("NeoForge installation complete!"));
        emit progress(100);
        m_step = Step::Idle;
        emit finished(true);
    });

    installer->start(m_javaPath.isEmpty() ? "java" : m_javaPath,
                     QStringList() << "-jar" << installerPath << "--installServer");
}
