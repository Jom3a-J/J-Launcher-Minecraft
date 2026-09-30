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

// Fabric: the server launcher jar from Fabric's meta API.

#include "ServerDownloader.h"
#include "ServerDownloaderShared.h"
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <algorithm>

using namespace ServerDownloaderDetail;

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
        finishDownload(false, tr("Failed to parse Fabric installer list."));
        return;
    }

    QJsonArray array = doc.array();
    if (array.isEmpty()) {
        finishDownload(false, tr("Fabric installer list is empty."));
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
        finishDownload(false, tr("Failed to parse Fabric loader list."));
        return;
    }

    QJsonArray array = doc.array();
    if (array.isEmpty()) {
        finishDownload(false, tr("No Fabric loader found for version %1.").arg(m_version));
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
        finishDownload(false, tr("Fabric loader %1 is not published for Minecraft %2.")
                                 .arg(m_loaderVersion, m_version));
        return;
    }
    m_resolvedLoaderVersion = loaderVer;

    // Construct direct download URL for the server launcher jar
    const QUrl jarUrl = m_endpoints.fabricApiBase.resolved(
        QUrl(QString("versions/loader/%1/%2/%3/server/jar")
                 .arg(m_version, loaderVer, installerVer)));

    emit statusMessage(tr("Downloading Fabric server (loader %1)...").arg(loaderVer));
    m_fabricJarPendingValidation = true;
    downloadFile(jarUrl.toString(), m_targetJarPath);
}
