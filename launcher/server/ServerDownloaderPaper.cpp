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
#include "ServerDownloaderProvider.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QRegularExpression>

namespace {

// ==================== Paper (v3 API) ====================

class PaperServerProvider final : public ServerDownloaderProvider
{
public:
    using ServerDownloaderProvider::ServerDownloaderProvider;

    QUrl versionListUrl() const override
    {
        return endpoints().paperApiBase.resolved(QUrl("projects/paper"));
    }

    QUrl buildListUrl(const QString &minecraftVersion) const override
    {
        return endpoints().paperApiBase.resolved(
            QUrl(QString("projects/paper/versions/%1/builds").arg(minecraftVersion)));
    }

    void start() override
    {
        status(tr("Fetching PaperMC builds..."));
        // Paper v3 API: get version details including builds
        request(buildListUrl(version()));
    }

    void onReply(const QByteArray &data) override
    {
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isNull()) {
            fail(tr("Failed to parse Paper builds response."));
            return;
        }

        // Fill v3 returns the builds directly as an array. Keep the object form
        // as a fallback for older compatible endpoints.
        QJsonArray builds = doc.isArray() ? doc.array() : doc.object()["builds"].toArray();
        if (builds.isEmpty()) {
            const QString apiMessage = doc.isObject() ? doc.object()["message"].toString() : QString();
            fail(apiMessage.isEmpty()
                     ? tr("No Paper builds found for version %1.").arg(version())
                     : tr("Paper download service: %1").arg(apiMessage));
            return;
        }

        // Use an explicitly selected build when supplied. Otherwise prefer the
        // newest stable build, falling back to the newest build of any channel.
        const QString &requestedBuild = requestedLoaderVersion();
        QJsonObject selectedBuild;
        int selectedNumber = -1;
        for (const QJsonValue &value : builds) {
            const QJsonObject build = value.toObject();
            const int number = build["id"].toInt(build["build"].toInt());
            if (!requestedBuild.isEmpty() && QString::number(number) == requestedBuild) {
                selectedBuild = build;
                break;
            }
            if (requestedBuild.isEmpty() && build["channel"].toString() == "STABLE"
                && number > selectedNumber) {
                selectedBuild = build;
                selectedNumber = number;
            }
        }
        if (selectedBuild.isEmpty() && requestedBuild.isEmpty()) {
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
            fail(tr("Paper build %1 is not published for Minecraft %2.")
                     .arg(requestedBuild, version()));
            return;
        }
        const int buildNumber = selectedBuild["id"].toInt(selectedBuild["build"].toInt());
        const QString channel = selectedBuild["channel"].toString();
        setResolvedLoaderVersion(requestedBuild.isEmpty() ? QString::number(buildNumber) : requestedBuild);

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
            fail(tr("Paper build %1 does not provide a server download.").arg(buildNumber));
            return;
        }

        status(tr("Downloading Paper %1 build %2%3...")
                   .arg(version())
                   .arg(buildNumber)
                   .arg(channel.isEmpty() ? QString() : " (" + channel + ")"));
        downloadServerJar(QUrl(downloadUrl), sha256, QCryptographicHash::Sha256);
    }

    QString currentStep() const override { return tr("fetching provider builds"); }
};

// ==================== Purpur ====================

class PurpurServerProvider final : public ServerDownloaderProvider
{
public:
    using ServerDownloaderProvider::ServerDownloaderProvider;

    QUrl versionListUrl() const override
    {
        return endpoints().purpurApiBase.resolved(QUrl("purpur"));
    }

    QUrl buildListUrl(const QString &minecraftVersion) const override
    {
        return endpoints().purpurApiBase.resolved(QUrl(QString("purpur/%1").arg(minecraftVersion)));
    }

    void start() override
    {
        m_step = Step::FetchingBuilds;
        status(tr("Fetching Purpur builds..."));
        request(buildListUrl(version()));
    }

    void onReply(const QByteArray &data) override
    {
        if (m_step == Step::FetchingBuilds)
            onBuildsFetched(data);
        else
            onBuildInfoFetched(data);
    }

    bool onRequestFailed(int, const QString &error) override
    {
        if (m_step != Step::FetchingBuildInfo)
            return false;
        stopActiveWork();
        fail(tr("Failed to fetch the Purpur checksum for build %1: %2").arg(m_build, error));
        return true;
    }

    QString currentStep() const override
    {
        return m_step == Step::FetchingBuilds ? tr("fetching provider builds") : tr("server setup");
    }

private:
    void onBuildsFetched(const QByteArray &data)
    {
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isNull()) {
            fail(tr("Failed to parse Purpur builds."));
            return;
        }

        const QString &requestedBuild = requestedLoaderVersion();
        QJsonObject obj = doc.object();
        QJsonObject buildsObj = obj["builds"].toObject();
        QString latestBuild = requestedBuild.isEmpty()
            ? buildsObj["latest"].toString() : requestedBuild;

        if (!requestedBuild.isEmpty()) {
            bool published = false;
            for (const QJsonValue &value : buildsObj["all"].toArray()) {
                const QString candidate = value.isString() ? value.toString() : QString::number(value.toInt());
                if (candidate == requestedBuild) {
                    published = true;
                    break;
                }
            }
            if (!published) {
                fail(tr("Purpur build %1 is not published for Minecraft %2.")
                         .arg(requestedBuild, version()));
                return;
            }
        }

        if (latestBuild.isEmpty()) {
            fail(tr("No Purpur builds found for version %1.").arg(version()));
            return;
        }
        setResolvedLoaderVersion(latestBuild);

        m_build = latestBuild;
        m_step = Step::FetchingBuildInfo;
        status(tr("Fetching Purpur build checksum..."));
        request(endpoints().purpurApiBase.resolved(
            QUrl(QString("purpur/%1/%2").arg(version(), latestBuild))));
    }

    void onBuildInfoFetched(const QByteArray &data)
    {
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        const QByteArray checksum = doc.object().value(QStringLiteral("md5")).toString().toLatin1();
        static const QRegularExpression md5Pattern(QStringLiteral("^[0-9a-fA-F]{32}$"));
        if (!doc.isObject() || !md5Pattern.match(QString::fromLatin1(checksum)).hasMatch()) {
            fail(tr("Purpur did not provide a valid checksum for build %1.").arg(m_build));
            return;
        }
        const QUrl jarUrl = endpoints().purpurApiBase.resolved(
            QUrl(QString("purpur/%1/%2/download").arg(version(), m_build)));
        status(tr("Downloading Purpur build %1...").arg(m_build));
        downloadServerJar(jarUrl, checksum, QCryptographicHash::Md5);
    }

    enum class Step { FetchingBuilds, FetchingBuildInfo };
    Step m_step = Step::FetchingBuilds;
    QString m_build;
};

}  // namespace

std::unique_ptr<ServerDownloaderProvider> makePaperServerProvider(ServerDownloader &downloader)
{
    return std::make_unique<PaperServerProvider>(downloader);
}

std::unique_ptr<ServerDownloaderProvider> makePurpurServerProvider(ServerDownloader &downloader)
{
    return std::make_unique<PurpurServerProvider>(downloader);
}
