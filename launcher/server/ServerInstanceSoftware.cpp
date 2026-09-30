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

// Downloading and installing the server software itself.

#include "ServerInstance.h"
#include "ServerDownloader.h"
#include "Application.h"
#include "settings/SettingsObject.h"
#include <QDir>

bool ServerInstance::prepareServerSoftware()
{
    if (m_status == ServerStatus::Downloading || m_status == ServerStatus::Running
        || m_status == ServerStatus::Starting || m_status == ServerStatus::Stopping
        || m_serverPackImportInProgress) {
        return false;
    }
    if (hasLaunchTarget()) {
        appendLog(tr("[DOWNLOAD] Server software is already installed."));
        return true;
    }
    if (m_serverDirectory.trimmed().isEmpty()
        || (!QDir(m_serverDirectory).exists()
            && !QDir().mkpath(m_serverDirectory))) {
        const QString message = tr("The server folder could not be prepared for software installation.");
        appendLog("[DOWNLOAD ERROR] " + message);
        setStatus(ServerStatus::Error);
        emit serverError(message);
        return false;
    }

    const int requiredJava = qMax(
        requiredJavaVersion(), recommendedJavaMajor(m_version, m_loaderType));
    int detectedJava = 0;
    const QString javaPath = compatibleJavaPath(requiredJava, &detectedJava);
    if (!javaPath.isEmpty()) {
        m_javaPath = javaPath;
        return downloadServerJar(javaPath, false);
    }
    if (requiredJava > 0) {
        if (auto *application = APPLICATION_DYN;
            application && application->settings()->get("AutomaticJavaDownload").toBool()) {
            return installCompatibleJava(requiredJava, false, true);
        }
    }
    const QString message = requiredJava > 0
        ? tr("Server software needs Java %1 for installation. Configure Java or enable automatic Java downloads.")
              .arg(requiredJava)
        : tr("Server software installation needs a configured Java runtime.");
    appendLog("[DOWNLOAD ERROR] " + message);
    setStatus(ServerStatus::Error);
    emit serverError(message);
    return false;
}

bool ServerInstance::downloadServerJar(const QString &javaPath, bool startAfterDownload)
{
    return beginServerDownload(m_version, m_loaderVersion, javaPath, startAfterDownload,
                               false, m_loaderVersion.trimmed().isEmpty());
}

bool ServerInstance::downloadServerJarForVersion(const QString &targetVersion,
                                                 const QString &javaPath,
                                                 bool startAfterDownload)
{
    const QString normalizedVersion = targetVersion.trimmed();
    if (normalizedVersion.isEmpty()) {
        return false;
    }
    // Provider build identifiers belong to one Minecraft version. Let the
    // provider select a compatible build when changing Minecraft versions and
    // clear the old build metadata only after the new download succeeds.
    return beginServerDownload(normalizedVersion, QString(), javaPath, startAfterDownload, true, true);
}

bool ServerInstance::downloadServerBuild(const QString &targetBuild, const QString &javaPath,
                                         bool startAfterDownload)
{
    const QString normalizedBuild = targetBuild.trimmed();
    if (normalizedBuild.isEmpty()) return false;
    return beginServerDownload(m_version, normalizedBuild, javaPath, startAfterDownload, false, true);
}

bool ServerInstance::beginServerDownload(const QString &targetVersion,
                                         const QString &targetLoaderVersion,
                                         const QString &javaPath, bool startAfterDownload,
                                         bool commitTargetVersion, bool commitTargetLoaderVersion)
{
    if (m_status == ServerStatus::Downloading || m_status == ServerStatus::Running
        || m_status == ServerStatus::Starting || m_status == ServerStatus::Stopping
        || m_serverPackImportInProgress) {
        return false;
    }

    setStatus(ServerStatus::Downloading);
    m_downloadCancelRequested = false;

    if (m_downloader) {
        m_downloader->deleteLater();
    }

    m_downloader = m_providerEndpoints
        ? new ServerDownloader(*m_providerEndpoints, this)
        : new ServerDownloader(this);

    connect(m_downloader, &ServerDownloader::statusMessage, this, [this](const QString &msg) {
        QString formatted = QString("[DOWNLOAD] %1").arg(msg);
        appendLog(formatted);
        emit outputReceived(formatted);
    });

    connect(m_downloader, &ServerDownloader::progress, this, [this](int pct) {
        QString formatted = QString("[DOWNLOAD] Progress: %1%").arg(pct);
        appendLog(formatted);
        emit outputReceived(formatted);
    });

    connect(m_downloader, &ServerDownloader::finished, this,
            [this, startAfterDownload, targetVersion, targetLoaderVersion,
             commitTargetVersion, commitTargetLoaderVersion](bool success, const QString &err) {
        const bool cancelled = m_downloadCancelRequested;
        m_downloadCancelRequested = false;
        if (cancelled) {
            const QString formatted = tr("[DOWNLOAD] Download cancelled.");
            appendLog(formatted);
            emit outputReceived(formatted);
            m_downloader->deleteLater();
            m_downloader = nullptr;
            setStatus(ServerStatus::Stopped);
            emit serverSoftwareDownloadFinished(targetVersion, false, true, QString());
            return;
        }
        if (success) {
            const QString resolvedLoaderVersion = m_downloader->resolvedLoaderVersion();
            QString formatted = "[DOWNLOAD] Server software installed successfully!";
            appendLog(formatted);
            emit outputReceived(formatted);
            m_downloader->deleteLater();
            m_downloader = nullptr;
            if (commitTargetVersion) {
                setVersion(targetVersion);
            }
            if (commitTargetLoaderVersion) {
                setLoaderVersion(targetLoaderVersion.isEmpty()
                                     ? resolvedLoaderVersion : targetLoaderVersion);
            }
            setStatus(ServerStatus::Stopped);
            emit serverSoftwareDownloadFinished(targetVersion, true, false, QString());
            if (startAfterDownload) {
                start();
            }
        } else {
            QString formatted = QString("[DOWNLOAD ERROR] %1").arg(err);
            appendLog(formatted);
            emit outputReceived(formatted);
            m_downloader->deleteLater();
            m_downloader = nullptr;
            setStatus(ServerStatus::Error);
            emit serverError(err);
            emit serverSoftwareDownloadFinished(targetVersion, false, false, err);
        }
    });

    m_downloader->startDownload(targetVersion, m_loaderType, m_serverDirectory,
                                javaPath, targetLoaderVersion);
    return true;
}

bool ServerInstance::cancelDownload()
{
    if (m_status != ServerStatus::Downloading) {
        return false;
    }
    if (m_javaInstallTask) {
        return m_javaInstallTask->abort();
    }
    if (!m_downloader) {
        return false;
    }
    m_downloadCancelRequested = true;
    m_downloader->cancel();
    return true;
}
