// SPDX-License-Identifier: GPL-3.0-only
/*
 *  J Launcher - Minecraft Launcher
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QTest>

#include "Application.h"
#include "BuildConfig.h"
#include "modplatform/modrinth/ModrinthDownloadPolicy.h"
#include "net/ApiHeaderProxy.h"
#include "net/HostScheduler.h"
#include "net/NetJob.h"
#include "net/SegmentedDownload.h"

namespace {

constexpr qint64 Threshold = Net::SegmentedDownload::MinSegmentedSize;
/// The file from the log this whole change exists for: 139,191,552 bytes of Physics Mod.
constexpr qint64 PhysicsModSize = 139191552LL;

/*! Parses a one-entry "files" array the way ModrinthCreationTask::parseManifest does. */
QJsonValue fileSizeValueOf(const QByteArray& entryJson)
{
    const auto document = QJsonDocument::fromJson(entryJson);
    return document.object().value(QStringLiteral("fileSize"));
}

QUrl officialUrl(const QString& name = QStringLiteral("physics.jar"))
{
    return QUrl(QStringLiteral("https://%1/data/AANobbMI/versions/xxx/%2").arg(BuildConfig.MODRINTH_DOWNLOAD_HOST, name));
}

}  // namespace

class ModrinthDownloadPolicyTest final : public QObject {
    Q_OBJECT

   private slots:
    /*! Only an exact, positive, in-range JSON number is a size. Everything else is "no hint",
     *  never an error - a pack that gets this field wrong still installs. */
    void parsesOnlyExactPositiveFileSizes()
    {
        using namespace Modrinth;

        // The real thing.
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":139191552})")), PhysicsModSize);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":1})")), 1LL);

        // Missing, and every wrong JSON type.
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"path":"mods/a.jar"})")), UnknownFileSize);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":"139191552"})")), UnknownFileSize);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":null})")), UnknownFileSize);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":true})")), UnknownFileSize);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":[139191552]})")), UnknownFileSize);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":{"bytes":139191552}})")), UnknownFileSize);

        // Non-positive.
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":0})")), UnknownFileSize);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":-1})")), UnknownFileSize);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":-139191552})")), UnknownFileSize);

        // Fractional.
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":1.5})")), UnknownFileSize);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":-0.5})")), UnknownFileSize);

        // The exactness boundary. 2^53-1 still round trips; from 2^53 up, neighbouring integers
        // share a double - 9007199254740993 reads back as 9007199254740992 - so neither is a size.
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":9007199254740991})")), 9007199254740991LL);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":9007199254740992})")), UnknownFileSize);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":9007199254740993})")), UnknownFileSize);
        QCOMPARE(parseFileSize(fileSizeValueOf(R"({"fileSize":1e300})")), UnknownFileSize);
    }

    /*! The size half of the decision, at the boundary. */
    void segmentsOnlyAtOrAboveTheThreshold()
    {
        using namespace Modrinth;
        const QUrl url = officialUrl();

        QVERIFY(!shouldSegment(UnknownFileSize, url, 4));
        QVERIFY(!shouldSegment(0, url, 4));
        QVERIFY(!shouldSegment(1, url, 4));
        QVERIFY(!shouldSegment(Threshold - 1, url, 4));
        QVERIFY(shouldSegment(Threshold, url, 4));
        QVERIFY(shouldSegment(Threshold + 1, url, 4));
        QVERIFY(shouldSegment(PhysicsModSize, url, 4));
    }

    /*! The host half. A mirror may be any file server at all, so it keeps the ordinary path. */
    void segmentsOnlyOnTheOfficialHttpsHost()
    {
        using namespace Modrinth;

        QVERIFY(shouldSegment(PhysicsModSize, officialUrl(), 4));

        QUrl insecure = officialUrl();
        insecure.setScheme(QStringLiteral("http"));
        QVERIFY(!shouldSegment(PhysicsModSize, insecure, 4));

        QUrl lookalike = officialUrl();
        lookalike.setHost(BuildConfig.MODRINTH_DOWNLOAD_HOST + QStringLiteral(".example"));
        QVERIFY(!shouldSegment(PhysicsModSize, lookalike, 4));

        QUrl wrongPort = officialUrl();
        wrongPort.setPort(444);
        QVERIFY(!shouldSegment(PhysicsModSize, wrongPort, 4));

        QVERIFY(!shouldSegment(PhysicsModSize, QUrl(QStringLiteral("https://mirror.example/physics.jar")), 4));
        QVERIFY(!shouldSegment(PhysicsModSize, QUrl(), 4));
    }

    /*! The setting half. One or zero segments is the user switching the whole thing off. */
    void theSettingCanTurnSegmentationOff()
    {
        using namespace Modrinth;
        const QUrl url = officialUrl();

        QVERIFY(!shouldSegment(PhysicsModSize, url, 0));
        QVERIFY(!shouldSegment(PhysicsModSize, url, 1));
        QVERIFY(!shouldSegment(PhysicsModSize, url, -3));
        QVERIFY(shouldSegment(PhysicsModSize, url, 2));
        QVERIFY(shouldSegment(PhysicsModSize, url, Net::SegmentedDownload::DefaultSegments));
    }

    /*! Modrinth metadata goes to the official CDN and nowhere else, which is what keeps a
     *  redirected segment from telling a third party what is being installed. */
    void emitsDownloadMetadataOnlyForTheOfficialHost()
    {
        const Net::ModrinthDownloadMeta meta{ .reason = QStringLiteral("modpack"),
                                              .gameVersion = QStringLiteral("1.20.1"),
                                              .loader = QStringLiteral("fabric"),
                                              .dependentOn = QStringLiteral("abcdefgh") };
        const Net::ApiHeaderProxy proxy(meta);

        const auto metadataIn = [&proxy](const QUrl& url) {
            QByteArray found;
            for (const auto& header : proxy.headers(QNetworkRequest(url))) {
                if (header.headerName == QByteArrayLiteral("modrinth-download-meta")) {
                    found = header.headerValue;
                }
            }
            return found;
        };

        const QByteArray sent = metadataIn(officialUrl());
        QVERIFY(!sent.isEmpty());
        const auto decoded = QJsonDocument::fromJson(sent).object();
        QCOMPARE(decoded.value(QStringLiteral("reason")).toString(), QStringLiteral("modpack"));
        QCOMPARE(decoded.value(QStringLiteral("game_version")).toString(), QStringLiteral("1.20.1"));
        QCOMPARE(decoded.value(QStringLiteral("loader")).toString(), QStringLiteral("fabric"));
        QCOMPARE(decoded.value(QStringLiteral("dependent_on")).toString(), QStringLiteral("abcdefgh"));

        QUrl insecure = officialUrl();
        insecure.setScheme(QStringLiteral("http"));
        QVERIFY(metadataIn(insecure).isEmpty());
        QVERIFY(metadataIn(QUrl(QStringLiteral("https://mirror.example/physics.jar"))).isEmpty());
        QVERIFY(metadataIn(QUrl(QStringLiteral("http://127.0.0.1:8080/physics.jar"))).isEmpty());

        // An empty metadata block is not sent at all, so an ordinary download is unchanged.
        const Net::ApiHeaderProxy bare{ Net::ModrinthDownloadMeta{} };
        for (const auto& header : bare.headers(QNetworkRequest(officialUrl()))) {
            QVERIFY(header.headerName != QByteArrayLiteral("modrinth-download-meta"));
        }
    }

    /*! The enqueue helper is what production uses for the first URL and for every fallback, so
     *  this drives exactly the code path the creation task takes. */
    void enqueuesASegmentedTaskOnlyForLargeOfficialFiles()
    {
        QNetworkAccessManager network;
        Net::HostScheduler scheduler;
        auto job = makeShared<NetJob>(QStringLiteral("policy"), &network, -1, &scheduler);

        const auto enqueue = [&job](qint64 size, const QUrl& url, const QString& path, int segments) {
            Modrinth::PackFileDownload file{ .url = url,
                                             .path = path,
                                             .declaredSize = size,
                                             .hashAlgorithm = QCryptographicHash::Sha512,
                                             .hash = QByteArray(64, '\x01') };
            return Modrinth::enqueuePackFileDownload(job, file, Net::ModrinthDownloadMeta{ .reason = "modpack" }, segments);
        };

        auto big = enqueue(PhysicsModSize, officialUrl(), QStringLiteral("mods/physics.jar"), 4);
        auto* segmented = qobject_cast<Net::SegmentedDownload*>(big.get());
        QVERIFY2(segmented, "a large official file should have been segmented");
        QCOMPARE(segmented->targetPath(), QStringLiteral("mods/physics.jar"));
        QCOMPARE(segmented->url(), officialUrl());

        // Small, mirrored, and switched off: all three stay ordinary requests.
        QVERIFY(!qobject_cast<Net::SegmentedDownload*>(
            enqueue(Threshold - 1, officialUrl(QStringLiteral("small.jar")), QStringLiteral("mods/small.jar"), 4).get()));
        QVERIFY(!qobject_cast<Net::SegmentedDownload*>(
            enqueue(Modrinth::UnknownFileSize, officialUrl(), QStringLiteral("mods/unknown.jar"), 4).get()));
        QVERIFY(!qobject_cast<Net::SegmentedDownload*>(
            enqueue(PhysicsModSize, QUrl(QStringLiteral("https://mirror.example/physics.jar")), QStringLiteral("mods/m.jar"), 4)
                .get()));
        QVERIFY(!qobject_cast<Net::SegmentedDownload*>(
            enqueue(PhysicsModSize, officialUrl(), QStringLiteral("mods/off.jar"), 1).get()));

        // Every one of them is a unit of work in the job, segmented or not.
        QCOMPARE(job->size(), 5);
        QCOMPARE(scheduler.outstandingPermits(), 0);

        QVERIFY(!Modrinth::enqueuePackFileDownload(nullptr, {}, Net::ModrinthDownloadMeta{}, 4));
    }

    /*! A fallback URL is re-decided rather than inheriting the failed attempt's shape, and it
     *  keeps the destination, the checksum algorithm and the metadata of what it replaces. */
    void fallbackUrlIsRedecidedAgainstTheSamePolicy()
    {
        QNetworkAccessManager network;
        Net::HostScheduler scheduler;
        auto job = makeShared<NetJob>(QStringLiteral("fallback"), &network, -1, &scheduler);

        const Net::ModrinthDownloadMeta meta{ .reason = QStringLiteral("modpack") };
        Modrinth::PackFileDownload file{ .url = officialUrl(),
                                         .path = QStringLiteral("mods/physics.jar"),
                                         .declaredSize = PhysicsModSize,
                                         .hashAlgorithm = QCryptographicHash::Sha512,
                                         .hash = QByteArray(64, '\x02') };

        auto first = Modrinth::enqueuePackFileDownload(job, file, meta, 4);
        QVERIFY(qobject_cast<Net::SegmentedDownload*>(first.get()));

        // A second official URL for the same file: still large, still official, still segmented,
        // and still aimed at the same place on disk.
        file.url = officialUrl(QStringLiteral("physics-mirror-a.jar"));
        auto second = Modrinth::enqueuePackFileDownload(job, file, meta, 4);
        auto* replacement = qobject_cast<Net::SegmentedDownload*>(second.get());
        QVERIFY2(replacement, "the replacement for a large official file should also be segmented");
        QCOMPARE(replacement->url(), file.url);
        QCOMPARE(replacement->targetPath(), file.path);

        // A fallback onto a mirror drops back to an ordinary request for the same destination.
        file.url = QUrl(QStringLiteral("https://mirror.example/physics.jar"));
        auto third = Modrinth::enqueuePackFileDownload(job, file, meta, 4);
        auto* ordinary = qobject_cast<Net::NetRequest*>(third.get());
        QVERIFY2(ordinary, "a mirrored fallback should be an ordinary network request");
        QVERIFY(!qobject_cast<Net::SegmentedDownload*>(third.get()));

        QCOMPARE(job->size(), 3);
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir dataDirectory;
    if (!dataDirectory.isValid()) {
        return 1;
    }

    QByteArray applicationName(argv[0]);
    QByteArray directoryOption("--dir");
    QByteArray directoryPath = dataDirectory.path().toUtf8();
    char* applicationArguments[] = {
        applicationName.data(), directoryOption.data(), directoryPath.data(), nullptr,
    };
    int applicationArgumentCount = 3;
    Application application(applicationArgumentCount, applicationArguments);
    ModrinthDownloadPolicyTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ModrinthDownloadPolicy_test.moc"
