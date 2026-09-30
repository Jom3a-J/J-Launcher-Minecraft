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

// Vanilla: Mojang's official server jar, verified against the version manifest.

#include "ServerDownloader.h"
#include "ServerDownloaderShared.h"
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QUrl>
#include <algorithm>

using namespace ServerDownloaderDetail;

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
        finishDownload(false, tr("Failed to parse version manifest."));
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
        finishDownload(false, tr("Version '%1' not found in manifest.").arg(m_version));
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
        finishDownload(false, tr("Failed to parse version details."));
        return;
    }

    QJsonObject downloads = doc.object()["downloads"].toObject();
    QJsonObject server = downloads["server"].toObject();
    QString jarUrl = server["url"].toString();
    QByteArray sha1 = server["sha1"].toString().toLatin1();

    if (jarUrl.isEmpty() || sha1.isEmpty()) {
        finishDownload(false, tr("No server download URL found for this version."));
        return;
    }

    if (m_fetchingLegacyForgeServerJar) {
        m_fetchingLegacyForgeServerJar = false;
        m_legacyForgeServerJarPath = QDir(m_destinationDir).filePath(
            QStringLiteral("minecraft_server.%1.jar").arg(m_version));
        if (fileMatchesSha1(m_legacyForgeServerJarPath, sha1)) {
            beginForgeInstallerDownload(m_pendingForgeVersion, m_pendingForgeMavenVersion);
            return;
        }
        m_downloadingLegacyForgeServerJar = true;
        downloadFile(jarUrl, m_legacyForgeServerJarPath, sha1, QCryptographicHash::Sha1);
        return;
    }

    downloadFile(jarUrl, m_targetJarPath, sha1, QCryptographicHash::Sha1);
}
