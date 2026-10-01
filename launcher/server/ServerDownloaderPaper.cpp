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

// Paper and Purpur: builds from the PaperMC and Purpur download APIs.

#include "ServerDownloader.h"
#include "ServerDownloaderShared.h"
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QRegularExpression>
#include <algorithm>

using namespace ServerDownloaderDetail;

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
        finishDownload(false, tr("Failed to parse Paper builds response."));
        return;
    }

    // Fill v3 returns the builds directly as an array. Keep the object form
    // as a fallback for older compatible endpoints.
    QJsonArray builds = doc.isArray() ? doc.array() : doc.object()["builds"].toArray();
    if (builds.isEmpty()) {
        const QString apiMessage = doc.isObject() ? doc.object()["message"].toString() : QString();
        finishDownload(false, apiMessage.isEmpty()
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
        finishDownload(false, tr("Paper build %1 is not published for Minecraft %2.")
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
        finishDownload(false, tr("Paper build %1 does not provide a server download.").arg(buildNumber));
        return;
    }

    emit statusMessage(tr("Downloading Paper %1 build %2%3...")
                       .arg(m_version)
                       .arg(buildNumber)
                       .arg(channel.isEmpty() ? QString() : " (" + channel + ")"));
    downloadFile(downloadUrl, m_targetJarPath, sha256, QCryptographicHash::Sha256);
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
        finishDownload(false, tr("Failed to parse Purpur builds."));
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
            finishDownload(false, tr("Purpur build %1 is not published for Minecraft %2.")
                                     .arg(m_loaderVersion, m_version));
            return;
        }
    }

    if (latestBuild.isEmpty()) {
        finishDownload(false, tr("No Purpur builds found for version %1.").arg(m_version));
        return;
    }
    m_resolvedLoaderVersion = latestBuild;

    m_pendingPurpurBuild = latestBuild;
    const QUrl metadataUrl = m_endpoints.purpurApiBase.resolved(
        QUrl(QString("purpur/%1/%2").arg(m_version, latestBuild)));
    m_step = Step::FetchingPurpurBuildInfo;
    emit statusMessage(tr("Fetching Purpur build checksum..."));
    m_currentReply = m_network->get(createRequest(metadataUrl));
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        handleReply(m_currentReply);
    });
}

void ServerDownloader::onPurpurBuildInfoFetched(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QByteArray checksum = doc.object().value(QStringLiteral("md5")).toString().toLatin1();
    static const QRegularExpression md5Pattern(QStringLiteral("^[0-9a-fA-F]{32}$"));
    if (!doc.isObject() || !md5Pattern.match(QString::fromLatin1(checksum)).hasMatch()) {
        finishDownload(false, tr("Purpur did not provide a valid checksum for build %1.")
                                 .arg(m_pendingPurpurBuild));
        return;
    }
    const QUrl jarUrl = m_endpoints.purpurApiBase.resolved(
        QUrl(QString("purpur/%1/%2/download").arg(m_version, m_pendingPurpurBuild)));
    emit statusMessage(tr("Downloading Purpur build %1...").arg(m_pendingPurpurBuild));
    downloadFile(jarUrl.toString(), m_targetJarPath, checksum, QCryptographicHash::Md5);
}
