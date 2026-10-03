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
#include "ServerDownloaderProvider.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include "archive/ArchiveReader.h"

namespace {

class FabricServerProvider final : public ServerDownloaderProvider
{
public:
    using ServerDownloaderProvider::ServerDownloaderProvider;

    QUrl versionListUrl() const override
    {
        return endpoints().fabricApiBase.resolved(QUrl("versions/game"));
    }

    QUrl buildListUrl(const QString &minecraftVersion) const override
    {
        return endpoints().fabricApiBase.resolved(
            QUrl(QString("versions/loader/%1").arg(minecraftVersion)));
    }

    void start() override
    {
        m_step = Step::FetchingInstallers;
        status(tr("Fetching Fabric installer version..."));
        request(endpoints().fabricApiBase.resolved(QUrl("versions/installer")));
    }

    void onReply(const QByteArray &data) override
    {
        if (m_step == Step::FetchingInstallers)
            onInstallersFetched(data);
        else
            onLoadersFetched(data);
    }

    bool checkServerJar(QString *error) override
    {
        MMCZip::ArchiveReader archive(targetJarPath());
        if (!archive.goToFile(QStringLiteral("META-INF/MANIFEST.MF"))) {
            QFile::remove(targetJarPath());
            *error = tr("The downloaded Fabric server launcher is not a valid jar file.");
            return false;
        }
        return true;
    }

    QString currentStep() const override
    {
        return m_step == Step::FetchingInstallers ? tr("fetching Fabric installer versions")
                                                  : tr("fetching Fabric loader versions");
    }

private:
    void onInstallersFetched(const QByteArray &data)
    {
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isNull()) {
            fail(tr("Failed to parse Fabric installer list."));
            return;
        }

        QJsonArray array = doc.array();
        if (array.isEmpty()) {
            fail(tr("Fabric installer list is empty."));
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

        m_installerVersion = installerVer;
        m_step = Step::FetchingLoaders;
        status(tr("Fetching Fabric loader version..."));
        request(buildListUrl(version()));
    }

    void onLoadersFetched(const QByteArray &data)
    {
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isNull()) {
            fail(tr("Failed to parse Fabric loader list."));
            return;
        }

        QJsonArray array = doc.array();
        if (array.isEmpty()) {
            fail(tr("No Fabric loader found for version %1.").arg(version()));
            return;
        }

        const QString &requestedLoader = requestedLoaderVersion();
        QString loaderVer;
        for (const QJsonValue &value : array) {
            const QString candidate =
                value.toObject().value("loader").toObject().value("version").toString();
            if (requestedLoader.isEmpty() || candidate == requestedLoader) {
                loaderVer = candidate;
                break;
            }
        }
        if (loaderVer.isEmpty()) {
            fail(tr("Fabric loader %1 is not published for Minecraft %2.")
                     .arg(requestedLoader, version()));
            return;
        }
        setResolvedLoaderVersion(loaderVer);

        // Construct direct download URL for the server launcher jar
        const QUrl jarUrl = endpoints().fabricApiBase.resolved(
            QUrl(QString("versions/loader/%1/%2/%3/server/jar")
                     .arg(version(), loaderVer, m_installerVersion)));

        status(tr("Downloading Fabric server (loader %1)...").arg(loaderVer));
        downloadServerJar(jarUrl, QByteArray(), QCryptographicHash::Sha256);
    }

    enum class Step { FetchingInstallers, FetchingLoaders };
    Step m_step = Step::FetchingInstallers;
    QString m_installerVersion;
};

}  // namespace

std::unique_ptr<ServerDownloaderProvider> makeFabricServerProvider(ServerDownloader &downloader)
{
    return std::make_unique<FabricServerProvider>(downloader);
}
