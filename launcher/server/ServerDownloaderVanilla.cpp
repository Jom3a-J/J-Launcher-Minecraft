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
#include "ServerDownloaderProvider.h"
#include "ServerDownloaderShared.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>

namespace ServerDownloaderDetail {

bool findVanillaVersionDetails(const QByteArray &manifest, const QString &version,
                               QString *detailsUrl, QString *error)
{
    QJsonDocument doc = QJsonDocument::fromJson(manifest);
    if (doc.isNull()) {
        *error = ServerDownloader::tr("Failed to parse version manifest.");
        return false;
    }

    QJsonArray versions = doc.object()["versions"].toArray();
    for (const auto &val : versions) {
        QJsonObject verObj = val.toObject();
        if (verObj["id"].toString() == version) {
            *detailsUrl = verObj["url"].toString();
            break;
        }
    }

    if (detailsUrl->isEmpty()) {
        *error = ServerDownloader::tr("Version '%1' not found in manifest.").arg(version);
        return false;
    }
    return true;
}

bool findVanillaServerJar(const QByteArray &details, QString *jarUrl, QByteArray *sha1,
                          QString *error)
{
    QJsonDocument doc = QJsonDocument::fromJson(details);
    if (doc.isNull()) {
        *error = ServerDownloader::tr("Failed to parse version details.");
        return false;
    }

    QJsonObject downloads = doc.object()["downloads"].toObject();
    QJsonObject server = downloads["server"].toObject();
    *jarUrl = server["url"].toString();
    *sha1 = server["sha1"].toString().toLatin1();

    if (jarUrl->isEmpty() || sha1->isEmpty()) {
        *error = ServerDownloader::tr("No server download URL found for this version.");
        return false;
    }
    return true;
}

}  // namespace ServerDownloaderDetail

using namespace ServerDownloaderDetail;

namespace {

class VanillaServerProvider final : public ServerDownloaderProvider
{
public:
    using ServerDownloaderProvider::ServerDownloaderProvider;

    QUrl versionListUrl() const override { return endpoints().vanillaManifest; }
    QUrl buildListUrl(const QString &) const override { return {}; }

    void start() override
    {
        m_step = Step::FetchingManifest;
        status(tr("Fetching Mojang version manifest..."));
        request(endpoints().vanillaManifest);
    }

    void onReply(const QByteArray &data) override
    {
        QString error;
        if (m_step == Step::FetchingManifest) {
            QString detailsUrl;
            if (!findVanillaVersionDetails(data, version(), &detailsUrl, &error)) {
                fail(error);
                return;
            }
            m_step = Step::FetchingVersionDetails;
            status(tr("Fetching version details..."));
            request(QUrl(detailsUrl));
            return;
        }

        QString jarUrl;
        QByteArray sha1;
        if (!findVanillaServerJar(data, &jarUrl, &sha1, &error)) {
            fail(error);
            return;
        }
        downloadServerJar(QUrl(jarUrl), sha1, QCryptographicHash::Sha1);
    }

    QString currentStep() const override
    {
        return m_step == Step::FetchingManifest ? tr("fetching the version manifest")
                                                : tr("fetching version details");
    }

private:
    enum class Step { FetchingManifest, FetchingVersionDetails };
    Step m_step = Step::FetchingManifest;
};

}  // namespace

std::unique_ptr<ServerDownloaderProvider> makeVanillaServerProvider(ServerDownloader &downloader)
{
    return std::make_unique<VanillaServerProvider>(downloader);
}
