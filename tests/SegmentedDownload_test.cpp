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

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

#include "Application.h"
#include "FileSystem.h"
#include "RangeHttpServer.h"
#include "net/ApiHeaderProxy.h"
#include "net/ChecksumValidator.h"
#include "net/HostScheduler.h"
#include "net/PartFile.h"
#include "net/RawHeaderProxy.h"
#include "net/SegmentedDownload.h"

using Net::HostScheduler;
using Net::SegmentedDownload;

namespace {

/*! Big enough to be split.
 *
 *  33 MiB clears SegmentedDownload::MinSegmentedSize (32 MiB), and what is left after the 1 MiB
 *  discovery chunk still gives every segment more than MinSegmentSize (8 MiB).
 */
constexpr qint64 LargeSize = 33LL * 1024 * 1024;
/// Leaves eight full minimum-size segments after Discovery has consumed its first 1 MiB chunk.
constexpr qint64 EightSegmentSize = SegmentedDownload::MaxSegments * SegmentedDownload::MinSegmentSize
    + SegmentedDownload::DiscoveryChunk;
/// Below the threshold, and below the discovery chunk, so it arrives in a single request.
constexpr qint64 TinySize = 64LL * 1024;
/// Above the discovery chunk but below the threshold: fetched, but never split.
constexpr qint64 MediumSize = 4LL * 1024 * 1024;

/// A deterministic body, so every assertion is reproducible.
QByteArray makeBody(qint64 size, quint32 seed)
{
    QByteArray body(static_cast<int>(size), Qt::Uninitialized);
    quint32 x = seed ? seed : 1u;
    for (qint64 i = 0; i < size; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        body[static_cast<int>(i)] = static_cast<char>(x & 0xFFu);
    }
    return body;
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QString sha1Of(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
}

RangeHttpServer::Resource simpleResource(QByteArray body)
{
    RangeHttpServer::Resource resource;
    resource.body = std::move(body);
    resource.etag = "\"v1\"";
    return resource;
}

/*! Collects everything a test wants to know about one run of a SegmentedDownload. */
class Harness {
   public:
    Harness() : m_dir()
    {
        m_network.setProxy(QNetworkProxy::NoProxy);
    }

    bool valid() const { return m_dir.isValid(); }
    QString target(const QString& name = QStringLiteral("pack.zip")) const { return FS::PathCombine(m_dir.path(), name); }
    QNetworkAccessManager* network() { return &m_network; }
    HostScheduler* scheduler() { return &m_scheduler; }

    SegmentedDownload::Ptr create(const QUrl& url, int segments = 4, const QString& name = QStringLiteral("pack.zip"))
    {
        auto task = SegmentedDownload::makeFile(url, target(name), &m_network, &m_scheduler, segments);
        QObject::connect(task.get(), &Task::progress, task.get(), [this](qint64 current, qint64 total) {
            m_progress.append({ current, total });
        });
        return task;
    }

    bool run(const SegmentedDownload::Ptr& task, int timeoutMs = 60000)
    {
        task->start();
        return QTest::qWaitFor([&task] { return task->isFinished(); }, timeoutMs);
    }

    const QList<QPair<qint64, qint64>>& progress() const { return m_progress; }

    /// Lets queued deleteLater()s run while the network manager and scheduler are still alive.
    void settle(SegmentedDownload::Ptr& task)
    {
        task.reset();
        for (int i = 0; i < 6; i++) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QTest::qWait(10);
        }
    }

   private:
    QTemporaryDir m_dir;
    QNetworkAccessManager m_network;
    HostScheduler m_scheduler;
    QList<QPair<qint64, qint64>> m_progress;
};

/*! Ensures a task's deferred deletes are drained even if a QtTest assertion returns early. */
class TaskSettler {
   public:
    TaskSettler(Harness& harness, SegmentedDownload::Ptr& task) : m_harness(harness), m_task(task) {}
    ~TaskSettler() { m_harness.settle(m_task); }

    TaskSettler(const TaskSettler&) = delete;
    TaskSettler& operator=(const TaskSettler&) = delete;

   private:
    Harness& m_harness;
    SegmentedDownload::Ptr& m_task;
};

/*! Proves the served ranges tile [0, size) exactly - no gap, no overlap, nothing extra. */
bool rangesTileExactly(QList<QPair<qint64, qint64>> ranges, qint64 size, QString* why)
{
    std::sort(ranges.begin(), ranges.end());
    qint64 covered = 0;
    for (const auto& range : ranges) {
        if (range.first > covered) {
            *why = QStringLiteral("gap before byte %1").arg(range.first);
            return false;
        }
        if (range.first < covered) {
            *why = QStringLiteral("overlap at byte %1").arg(range.first);
            return false;
        }
        covered = range.second + 1;
    }
    if (covered != size) {
        *why = QStringLiteral("covered %1 of %2 bytes").arg(covered).arg(size);
        return false;
    }
    return true;
}

}  // namespace

class SegmentedDownloadTest final : public QObject {
    Q_OBJECT

   private slots:
    /*! The core case: one large file, several ranges, and bytes that match exactly.
     *
     *  The segment count is three rather than four because 127.0.0.1 classifies as an unknown
     *  host (hard ceiling six) and a segmented download may never take more than half of a host's
     *  pool. That cap is the point - see capIsTheProviderCeilingNotTheSetting below.
     */
    void multiPartAssemblyCoversTheFileExactly()
    {
        const QByteArray body = makeBody(LargeSize, 0xA5A5A5A5u);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY2(harness.run(task), "the segmented download never finished");
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));

        QCOMPARE(readAll(harness.target()), body);
        QCOMPARE(task->contentLength(), LargeSize);
        QVERIFY2(task->segmentsUsed() >= 2, "the file was not split at all");
        QVERIFY(!task->ranUnsegmented());

        // One discovery request plus one per segment, and together they cover the file exactly.
        QCOMPARE(server.rangeRequestCount(), 1 + task->segmentsUsed());
        QString why;
        QVERIFY2(rangesTileExactly(server.servedRanges(), LargeSize, &why), qPrintable(why));
        QVERIFY2(server.peakConcurrency() >= 2, "the segments did not actually run at the same time");

        // Nothing is left behind, and no permit leaked.
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        QCOMPARE(harness.scheduler()->globalInFlight(), 0);
        harness.settle(task);
    }

    /*! A server that ignores Range must cost exactly one request and nothing else. */
    void ignoredRangeFallsBackToASingleStream()
    {
        const QByteArray body = makeBody(LargeSize, 0x12345678u);
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(body);
        resource.ignoreRange = true;
        server.serve("/pack.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()), body);

        // The discovery reply *is* the download: it was never restarted.
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(task->ranUnsegmented());
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        harness.settle(task);
    }

    /*! "Accept-Ranges: none" is honoured even though the bytes would have been splittable. */
    void acceptRangesNoneDisablesSplitting()
    {
        const QByteArray body = makeBody(LargeSize, 0x0BADC0DEu);
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(body);
        resource.acceptRanges = false;
        server.serve("/pack.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()), body);
        QCOMPARE(server.requestCount(), 1);
        harness.settle(task);
    }

    /*! A file smaller than the discovery chunk arrives whole, in one request. */
    void tinyFileIsOneRequest()
    {
        const QByteArray body = makeBody(TinySize, 0x11111111u);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()), body);
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(task->ranUnsegmented());
        harness.settle(task);
    }

    /*! Between the discovery chunk and the threshold: fetched in full, never run in parallel. */
    void fileBelowTheThresholdIsNeverSplit()
    {
        const QByteArray body = makeBody(MediumSize, 0x22222222u);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()), body);
        QVERIFY(task->ranUnsegmented());
        QCOMPARE(server.peakConcurrency(), 1);
        QString why;
        QVERIFY2(rangesTileExactly(server.servedRanges(), MediumSize, &why), qPrintable(why));
        harness.settle(task);
    }

    /*! A 206 that describes the wrong bytes must not be written anywhere. */
    void wrongContentRangeFailsClosed()
    {
        const QByteArray body = makeBody(LargeSize, 0x33333333u);
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(body);
        resource.badContentRange = true;
        server.serve("/pack.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));

        QVERIFY(harness.run(task));
        // A malformed ranged response is never written at the claimed offset. The coordinator
        // may either fail closed or discard it and retry as an ordinary 200 stream; both are safe.
        if (task->wasSuccessful()) {
            QCOMPARE(readAll(harness.target()), body);
            QVERIFY(task->ranUnsegmented());
        } else {
            QVERIFY(!QFileInfo::exists(harness.target()));
            QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));
        }
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        harness.settle(task);
    }

    /*! More bytes than the range covers is a protocol violation, not extra data. */
    void overlongSegmentBodyFailsClosed()
    {
        const QByteArray body = makeBody(LargeSize, 0x44444444u);
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(body);
        resource.overlongBody = true;
        server.serve("/pack.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));

        QVERIFY(harness.run(task));
        QVERIFY(!task->wasSuccessful());
        QVERIFY2(task->failReason().contains(QStringLiteral("more data")), qPrintable(task->failReason()));
        QVERIFY(!QFileInfo::exists(harness.target()));
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));
        harness.settle(task);
    }

    /*! Without a concrete total there is nothing safe to split, so it degrades to one stream. */
    void unknownTotalDegradesToOneStream()
    {
        const QByteArray body = makeBody(LargeSize, 0x55555555u);
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(body);
        resource.unknownTotal = true;
        server.serve("/pack.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()), body);
        QVERIFY(task->ranUnsegmented());
        harness.settle(task);
    }

    /*! A content coding would make every byte offset meaningless, so ranges are abandoned. */
    void contentEncodingDegradesToOneStream()
    {
        const QByteArray body = makeBody(LargeSize, 0x66666666u);
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(body);
        // The test body is intentionally not compressed. Use an unknown transform marker so Qt
        // does not attempt to decode invalid gzip bytes; the downloader must still disable ranges
        // for every non-identity Content-Encoding.
        resource.contentEncoding = "x-test-transform";
        server.serve("/pack.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));

        QVERIFY(harness.run(task));
        // Either it fell back cleanly and matches, or it refused; what it must never do is
        // assemble something that is neither.
        if (task->wasSuccessful()) {
            QCOMPARE(readAll(harness.target()), body);
            QVERIFY(task->ranUnsegmented());
        } else {
            QVERIFY(!QFileInfo::exists(harness.target()));
        }
        harness.settle(task);
    }

    /*! multipart/byteranges is not something the sink can place in the file. */
    void multipartRangeDegradesToOneStream()
    {
        const QByteArray body = makeBody(LargeSize, 0x77777777u);
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(body);
        resource.multipart = true;
        server.serve("/pack.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()), body);
        QVERIFY(task->ranUnsegmented());
        harness.settle(task);
    }

    /*! The entity changing under us must leave no part of the old version behind. */
    void entityChangeRestartsAndLeavesNoStaleTail()
    {
        const QByteArray original = makeBody(LargeSize, 0x88888888u);
        // Deliberately shorter, so a stale tail would be visible as extra bytes at the end.
        const QByteArray replacement = makeBody(LargeSize / 2, 0x99999999u);

        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(original));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(replacement)));

        // Swap the file the moment the discovery request has been answered, so the segments carry
        // an If-Range the server no longer matches.
        QObject::connect(task.get(), &Task::progress, task.get(), [&server, &replacement]() {
            if (auto* resource = server.route("/pack.zip")) {
                if (resource->etag != "\"v2\"") {
                    resource->body = replacement;
                    resource->etag = "\"v2\"";
                }
            }
        });

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()).size(), replacement.size());
        QCOMPARE(readAll(harness.target()), replacement);
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));
        harness.settle(task);
    }

    /*! A truncated transfer resumes from the byte it stopped at, not from the start. */
    void truncatedTransferResumesFromTheCursor()
    {
        const QByteArray body = makeBody(LargeSize, 0xAAAAAAAAu);
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(body);
        // Cut the very first response short; everything after it behaves normally.
        resource.dropAfter = 4096;
        resource.dropCount = 1;
        server.serve("/pack.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()), body);

        // The retry asked for a range that starts where the dropped one stopped, so the bytes
        // that did arrive were not thrown away.
        const auto& requests = server.requests();
        QVERIFY(requests.size() >= 2);
        bool sawResume = false;
        for (const auto& record : requests) {
            if (record.range.startsWith("bytes=") && !record.range.startsWith("bytes=0-"))
                sawResume = true;
        }
        QVERIFY2(sawResume, "no request resumed from a non-zero offset");
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        harness.settle(task);
    }

    /*! Cancelling mid transfer promotes nothing and hands every permit back. */
    void cancellationPromotesNothingAndReleasesPermits()
    {
        const QByteArray body = makeBody(LargeSize, 0xBBBBBBBBu);
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(body);
        // Never finishes, so the transfer is reliably still running when we cancel it.
        resource.stallAfter = 2048;
        server.serve("/pack.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));
        QSignalSpy aborted(task.get(), &Task::aborted);

        task->start();
        QVERIFY2(QTest::qWaitFor([&server] { return server.requestCount() >= 1; }, 10000), "the download never started");
        QTest::qWait(150);

        QVERIFY(task->abort());
        QVERIFY(QTest::qWaitFor([&task] { return task->isFinished(); }, 10000));

        QCOMPARE(aborted.count(), 1);
        QVERIFY(!task->wasSuccessful());
        QVERIFY2(!QFileInfo::exists(harness.target()), "a cancelled download left a file at the target path");
        // The bytes stay for as long as the folder that holds them does.
        QVERIFY(QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));

        QTest::qWait(100);
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        QCOMPARE(harness.scheduler()->globalInFlight(), 0);
        harness.settle(task);
    }

    /*! A wrong checksum must never reach the target path. */
    void checksumFailureDoesNotPromote()
    {
        const QByteArray body = makeBody(LargeSize, 0xCCCCCCCCu);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(QByteArrayLiteral("something else"))));

        QVERIFY(harness.run(task));
        QVERIFY(!task->wasSuccessful());
        QVERIFY2(!QFileInfo::exists(harness.target()), "a download that failed its checksum was promoted anyway");
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        harness.settle(task);
    }

    /*! A correct checksum is still checked against the bytes that ended up on disk. */
    void checksumIsVerifiedOverTheAssembledFile()
    {
        const QByteArray body = makeBody(LargeSize, 0xDDDDDDDDu);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(sha1Of(readAll(harness.target())), sha1Of(body));
        harness.settle(task);
    }

    /*! Redirects still work, and the destination is never handed our API credentials. */
    void redirectIsFollowedWithoutLeakingCredentials()
    {
        const QByteArray body = makeBody(MediumSize, 0xEEEEEEEEu);
        RangeHttpServer origin;
        RangeHttpServer destination;
        QVERIFY(origin.start());
        QVERIFY(destination.start());
        destination.serve("/real.zip", simpleResource(body));
        origin.redirect("/pack.zip", destination.url("/real.zip"));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(origin.url("/pack.zip"));
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()), body);
        QVERIFY(origin.requestCount() >= 1);
        QVERIFY(destination.requestCount() >= 1);
        QCOMPARE(destination.authorizedRequestCount(), 0);
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        harness.settle(task);
    }

    /*! A redirect that leads nowhere fails without leaving anything behind. */
    void brokenRedirectFailsCleanly()
    {
        RangeHttpServer origin;
        RangeHttpServer destination;
        QVERIFY(origin.start());
        QVERIFY(destination.start());
        // The destination serves nothing at that path.
        origin.redirect("/pack.zip", destination.url("/missing.zip"));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(origin.url("/pack.zip"));

        QVERIFY(harness.run(task));
        QVERIFY(!task->wasSuccessful());
        QVERIFY(!QFileInfo::exists(harness.target()));
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));
        harness.settle(task);
    }

    /*! Turning the setting off must be indistinguishable from an ordinary download. */
    void settingOfOneRunsAnOrdinaryDownload()
    {
        const QByteArray body = makeBody(LargeSize, 0xF0F0F0F0u);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"), 1);
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()), body);

        // No discovery, no Range header, no part file - just the one request.
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(server.rangeRequestCount(), 0);
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));
        QVERIFY(task->ranUnsegmented());
        harness.settle(task);
    }

    void settingOfZeroRunsAnOrdinaryDownload()
    {
        const QByteArray body = makeBody(MediumSize, 0x0F0F0F0Fu);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"), 0);

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()), body);
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(server.rangeRequestCount(), 0);
        harness.settle(task);
    }

    /*! The setting is an upper bound, and so is the provider ceiling; the lower one wins. */
    void capIsTheLowerOfTheSettingAndTheProviderCeiling()
    {
        const QByteArray body = makeBody(LargeSize, 0x1A2B3C4Du);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());

        // The setting is the binding constraint here.
        auto limited = harness.create(server.url("/pack.zip"), 2, QStringLiteral("two.zip"));
        QVERIFY(harness.run(limited));
        QVERIFY2(limited->wasSuccessful(), qPrintable(limited->failReason()));
        QCOMPARE(limited->segmentsUsed(), 2);
        QCOMPARE(readAll(harness.target(QStringLiteral("two.zip"))), body);
        harness.settle(limited);

        // Asking for more than the host allows cannot raise it: an unknown host has a hard
        // ceiling of six, and half of that is three.
        const int ceiling = harness.scheduler()->ceilingFor(server.url("/pack.zip"));
        QCOMPARE(ceiling, HostScheduler::UnknownHostCeiling);

        RangeHttpServer second;
        QVERIFY(second.start());
        second.serve("/pack.zip", simpleResource(body));
        auto greedy = harness.create(second.url("/pack.zip"), SegmentedDownload::MaxSegments, QStringLiteral("many.zip"));
        QVERIFY(harness.run(greedy));
        QVERIFY2(greedy->wasSuccessful(), qPrintable(greedy->failReason()));
        QCOMPARE(greedy->segmentsUsed(), qMax(1, ceiling / 2));
        QVERIFY(greedy->segmentsUsed() < SegmentedDownload::MaxSegments);
        QCOMPARE(readAll(harness.target(QStringLiteral("many.zip"))), body);
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        harness.settle(greedy);
    }

    /*! Every live segment holds a real permit, and the host limit is never exceeded. */
    void segmentsConsumeRealPermits()
    {
        const QByteArray body = makeBody(LargeSize, 0x5A5A5A5Au);
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(body);
        // Let the 1 MiB discovery request finish; stall the larger segment responses so their
        // permits remain observable during the assertion.
        resource.stallAfter = 2 * 1024 * 1024;
        server.serve("/pack.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));

        task->start();
        // Wait until the fan out has happened: discovery, then one connection per segment.
        QVERIFY2(QTest::qWaitFor([&server] { return server.requestCount() >= 2; }, 15000), "the download never fanned out");
        QTest::qWait(300);

        const QUrl url = server.url("/pack.zip");
        const int inFlight = harness.scheduler()->inFlightFor(url);
        QVERIFY2(inFlight >= 2, "the segments were not each admitted with their own permit");
        QVERIFY2(inFlight <= harness.scheduler()->limitFor(url), "admission let the host exceed its limit");
        QCOMPARE(harness.scheduler()->outstandingPermits(), inFlight);

        QVERIFY(task->abort());
        QVERIFY(QTest::qWaitFor([&task] { return task->isFinished(); }, 10000));
        QTest::qWait(100);
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        harness.settle(task);
    }

    /*! Aggregate progress never goes backwards and ends on the exact size. */
    void progressIsMonotonicAndExact()
    {
        const QByteArray body = makeBody(LargeSize, 0x24242424u);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));

        QVERIFY(!harness.progress().isEmpty());
        qint64 previous = -1;
        for (const auto& update : harness.progress()) {
            QVERIFY2(update.first >= previous, "aggregate progress went backwards");
            previous = update.first;
        }
        QCOMPARE(harness.progress().last().first, LargeSize);
        QCOMPARE(harness.progress().last().second, LargeSize);
        QCOMPARE(task->getProgress(), LargeSize);
        QCOMPARE(task->getTotalProgress(), LargeSize);
        harness.settle(task);
    }

    /*! A target a part file cannot sit next to still downloads, the ordinary way. */
    void unusableTargetPathFallsBackInsteadOfFailing()
    {
        const QByteArray body = makeBody(MediumSize, 0x31313131u);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        const QString longName = QString(230, QLatin1Char('n')) + QStringLiteral(".zip");
        QVERIFY(!Net::PartFile::isUsableFor(harness.target(longName)));

        auto task = harness.create(server.url("/pack.zip"), 4, longName);
        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target(longName)), body);
        QCOMPARE(server.rangeRequestCount(), 0);
        harness.settle(task);
    }

    /*! A CDN may refuse Range at its edge host but serve the ordinary GET after redirect. */
    void rangedForbiddenOrMissingRetriesUnrangedAndValidates()
    {
        const QByteArray body = makeBody(MediumSize, 0x43434343u);
        RangeHttpServer server;
        QVERIFY(server.start());

        Harness harness;
        QVERIFY(harness.valid());
        for (const int status : { 403, 404 }) {
            const int firstRequest = server.requests().size();
            const QString route = QStringLiteral("/range-%1.zip").arg(status);
            auto resource = simpleResource(body);
            resource.rangeStatus = status;
            server.serve(route.toUtf8(), resource);

            const QString targetName = QStringLiteral("range-%1.zip").arg(status);
            auto task = harness.create(server.url(route.toUtf8()), 4, targetName);
            [[maybe_unused]] TaskSettler settleTask(harness, task);
            task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));
            QVERIFY2(harness.run(task), qPrintable(task->failReason()));
            QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
            QCOMPARE(readAll(harness.target(targetName)), body);
            QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target(targetName))));

            const auto requests = server.requests();
            QCOMPARE(requests.size() - firstRequest, 2);
            QCOMPARE(requests.at(firstRequest).status, status);
            QVERIFY(requests.at(firstRequest).range.startsWith("bytes="));
            QCOMPARE(requests.at(firstRequest + 1).status, 200);
            QVERIFY(requests.at(firstRequest + 1).range.isEmpty());
            QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        }
    }

    /*! A ranged refusal at edge can split after the plain retry reveals a range-capable redirect. */
    void redirectedRangeCapableTargetIsSplitAndValidated()
    {
        const QByteArray body = makeBody(EightSegmentSize, 0x45454545u);
        RangeHttpServer server;
        QVERIFY(server.start());

        RangeHttpServer::Resource edgeResource;
        QUrl resolvedUrl(QStringLiteral("http://mediafilez.forgecdn.net/files/resolved.zip"));
        edgeResource.body = resolvedUrl.toEncoded();
        edgeResource.rangeStatus = 404;
        edgeResource.forcedStatus = 302;
        server.serve("/files/edge.zip", edgeResource);
        server.serve("/files/resolved.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        const QUrl proxyUrl = server.url("/");
        harness.network()->setProxy(
            QNetworkProxy(QNetworkProxy::HttpProxy, proxyUrl.host(), static_cast<quint16>(proxyUrl.port())));
        const QUrl edgeUrl(QStringLiteral("http://edge.forgecdn.net/files/edge.zip"));
        auto task = harness.create(edgeUrl);
        [[maybe_unused]] TaskSettler settleTask(harness, task);
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY2(harness.run(task, 120000), qPrintable(task->failReason()));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(task->segmentsUsed(), SegmentedDownload::MaxSegments);
        QCOMPARE(readAll(harness.target()), body);
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));

        const auto requests = server.requests();
        QList<RangeHttpServer::RequestRecord> edgeRequests;
        QList<RangeHttpServer::RequestRecord> targetRequests;
        for (const auto& request : requests) {
            if (request.host.compare("edge.forgecdn.net", Qt::CaseInsensitive) == 0)
                edgeRequests.append(request);
            else if (request.host.compare("mediafilez.forgecdn.net", Qt::CaseInsensitive) == 0)
                targetRequests.append(request);
        }
        QCOMPARE(edgeRequests.size(), 2);
        QCOMPARE(edgeRequests.at(0).status, 404);
        QVERIFY(edgeRequests.at(0).range.startsWith("bytes="));
        QCOMPARE(edgeRequests.at(1).status, 302);
        QVERIFY(edgeRequests.at(1).range.isEmpty());

        QCOMPARE(targetRequests.size(), task->segmentsUsed() + 2);
        QCOMPARE(targetRequests.first().status, 200);
        QVERIFY(targetRequests.first().range.isEmpty());
        QVERIFY(targetRequests.at(1).range.startsWith("bytes="));
        QVERIFY(targetRequests.at(1).ifRange.isEmpty());
        QCOMPARE(std::count_if(targetRequests.cbegin(), targetRequests.cend(), [](const auto& request) {
                     return !request.range.isEmpty();
                 }),
                 task->segmentsUsed() + 1);
        for (int i = 1; i < targetRequests.size(); i++) {
            QVERIFY(targetRequests.at(i).range.startsWith("bytes="));
            QCOMPARE(targetRequests.at(i).status, 206);
            if (i > 1)
                QCOMPARE(targetRequests.at(i).ifRange, QByteArrayLiteral("\"v1\""));
        }
        QString why;
        QVERIFY2(rangesTileExactly(server.servedRanges(), EightSegmentSize, &why), qPrintable(why));
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
    }

    /*! A redirected target without range support keeps the original single-stream fallback. */
    void redirectedTargetWithoutRangesStaysSingleStream()
    {
        const QByteArray body = makeBody(LargeSize, 0x46464646u);
        RangeHttpServer server;
        QVERIFY(server.start());

        RangeHttpServer::Resource edgeResource;
        const QUrl resolvedUrl(QStringLiteral("http://mediafilez.forgecdn.net/files/ordinary.zip"));
        edgeResource.body = resolvedUrl.toEncoded();
        edgeResource.rangeStatus = 404;
        edgeResource.forcedStatus = 302;
        server.serve("/files/edge-ordinary.zip", edgeResource);
        auto targetResource = simpleResource(body);
        targetResource.acceptRanges = false;
        server.serve("/files/ordinary.zip", targetResource);

        Harness harness;
        QVERIFY(harness.valid());
        const QUrl proxyUrl = server.url("/");
        harness.network()->setProxy(
            QNetworkProxy(QNetworkProxy::HttpProxy, proxyUrl.host(), static_cast<quint16>(proxyUrl.port())));
        auto task = harness.create(QUrl(QStringLiteral("http://edge.forgecdn.net/files/edge-ordinary.zip")));
        [[maybe_unused]] TaskSettler settleTask(harness, task);
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY2(harness.run(task, 120000), qPrintable(task->failReason()));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(task->segmentsUsed(), 1);
        QCOMPARE(readAll(harness.target()), body);
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));

        const auto requests = server.requests();
        QList<RangeHttpServer::RequestRecord> edgeRequests;
        QList<RangeHttpServer::RequestRecord> targetRequests;
        for (const auto& request : requests) {
            if (request.host.compare("edge.forgecdn.net", Qt::CaseInsensitive) == 0)
                edgeRequests.append(request);
            else if (request.host.compare("mediafilez.forgecdn.net", Qt::CaseInsensitive) == 0)
                targetRequests.append(request);
        }
        QCOMPARE(edgeRequests.size(), 2);
        QCOMPARE(edgeRequests.first().status, 404);
        QCOMPARE(edgeRequests.last().status, 302);
        QCOMPARE(targetRequests.size(), 1);
        QCOMPARE(targetRequests.first().status, 200);
        QVERIFY(targetRequests.first().range.isEmpty());
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
    }

    /*! If the resolved target refuses its own ranged probe, retry it once as a single stream. */
    void redirectedTargetRangeRefusalFallsBackToSingleStream()
    {
        const QByteArray body = makeBody(LargeSize, 0x47474747u);
        RangeHttpServer server;
        QVERIFY(server.start());

        Harness harness;
        QVERIFY(harness.valid());
        const QUrl proxyUrl = server.url("/");
        harness.network()->setProxy(
            QNetworkProxy(QNetworkProxy::HttpProxy, proxyUrl.host(), static_cast<quint16>(proxyUrl.port())));

        for (const int status : { 404, 416 }) {
            const int firstRequest = server.requests().size();
            const QString suffix = QString::number(status);
            const QByteArray edgePath = QStringLiteral("/files/edge-refused-%1.zip").arg(suffix).toUtf8();
            const QByteArray targetPath = QStringLiteral("/files/target-refused-%1.zip").arg(suffix).toUtf8();
            const QString targetName = QStringLiteral("target-refused-%1.zip").arg(suffix);
            const QUrl resolvedUrl(QStringLiteral("http://mediafilez.forgecdn.net%1").arg(QString::fromUtf8(targetPath)));
            const QUrl edgeUrl(QStringLiteral("http://edge.forgecdn.net%1").arg(QString::fromUtf8(edgePath)));

            RangeHttpServer::Resource edgeResource;
            edgeResource.body = resolvedUrl.toEncoded();
            edgeResource.rangeStatus = 404;
            edgeResource.forcedStatus = 302;
            server.serve(edgePath, edgeResource);

            auto targetResource = simpleResource(body);
            // The full response advertises byte ranges, but the target refuses the ranged probe.
            targetResource.acceptRanges = true;
            targetResource.rangeStatus = status;
            server.serve(targetPath, targetResource);

            auto task = harness.create(edgeUrl, 4, targetName);
            [[maybe_unused]] TaskSettler settleTask(harness, task);
            task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

            QVERIFY2(harness.run(task, 120000), qPrintable(task->failReason()));
            QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
            QCOMPARE(task->segmentsUsed(), 1);
            QCOMPARE(readAll(harness.target(targetName)), body);
            QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target(targetName))));
            QCOMPARE(harness.scheduler()->outstandingPermits(), 0);

            // Five HTTP messages cover four logical requests: the unranged edge GET redirects to
            // a target GET, followed by target Discovery and its single-stream retry.
            const auto requests = server.requests();
            QCOMPARE(requests.size() - firstRequest, 5);
            const auto& edgeDiscovery = requests.at(firstRequest);
            QCOMPARE(edgeDiscovery.host.toLower(), QByteArrayLiteral("edge.forgecdn.net"));
            QCOMPARE(edgeDiscovery.target, edgePath);
            QCOMPARE(edgeDiscovery.status, 404);
            QVERIFY(edgeDiscovery.range.startsWith("bytes="));
            const auto& edgeRetry = requests.at(firstRequest + 1);
            QCOMPARE(edgeRetry.host.toLower(), QByteArrayLiteral("edge.forgecdn.net"));
            QCOMPARE(edgeRetry.target, edgePath);
            QCOMPARE(edgeRetry.status, 302);
            QVERIFY(edgeRetry.range.isEmpty());

            const auto& redirectedProbe = requests.at(firstRequest + 2);
            QCOMPARE(redirectedProbe.host.toLower(), QByteArrayLiteral("mediafilez.forgecdn.net"));
            QCOMPARE(redirectedProbe.target, targetPath);
            QCOMPARE(redirectedProbe.status, 200);
            QVERIFY(redirectedProbe.range.isEmpty());
            const auto& targetDiscovery = requests.at(firstRequest + 3);
            QCOMPARE(targetDiscovery.host.toLower(), QByteArrayLiteral("mediafilez.forgecdn.net"));
            QCOMPARE(targetDiscovery.target, targetPath);
            QCOMPARE(targetDiscovery.status, status);
            QVERIFY(targetDiscovery.range.startsWith("bytes="));
            const auto& targetRetry = requests.at(firstRequest + 4);
            QCOMPARE(targetRetry.host.toLower(), QByteArrayLiteral("mediafilez.forgecdn.net"));
            QCOMPARE(targetRetry.target, targetPath);
            QCOMPARE(targetRetry.status, 200);
            QVERIFY(targetRetry.range.isEmpty());
        }
    }

    /*! An unranged retry still reports a truly missing file and never promotes the part. */
    void rangedNotFoundAndUnrangedNotFoundFails()
    {
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(makeBody(MediumSize, 0x44444444u));
        resource.rangeStatus = 404;
        resource.forcedStatus = 404;
        server.serve("/missing.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/missing.zip"));
        [[maybe_unused]] TaskSettler settleTask(harness, task);

        QVERIFY(harness.run(task));
        QVERIFY(!task->wasSuccessful());
        const auto requests = server.requests();
        QCOMPARE(requests.size(), 2);
        QCOMPARE(requests.first().status, 404);
        QVERIFY(requests.first().range.startsWith("bytes="));
        QCOMPARE(requests.last().status, 404);
        QVERIFY(requests.last().range.isEmpty());
        QVERIFY(!QFileInfo::exists(harness.target()));
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
    }

    /*! 416 means the range was refused outright; the file still has to arrive. */
    void rangeNotSatisfiableFallsBack()
    {
        const QByteArray body = makeBody(MediumSize, 0x42424242u);
        RangeHttpServer server;
        QVERIFY(server.start());
        auto resource = simpleResource(body);
        resource.forcedStatus = 416;
        server.serve("/pack.zip", resource);

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));

        task->start();
        QVERIFY(QTest::qWaitFor([&task] { return task->isFinished(); }, 30000));
        // The retry is unranged; the stub server still answers 416, so this ends in a clean
        // failure rather than a promoted file.
        QVERIFY(!QFileInfo::exists(harness.target()));
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));
        QVERIFY2(server.requests().size() >= 2, "the 416 was not retried without a range");
        QVERIFY(server.requests().last().range.isEmpty());
        harness.settle(task);
    }

    /*! A missing file fails without creating anything. */
    void missingFileFailsWithoutTouchingTheTarget()
    {
        RangeHttpServer server;
        QVERIFY(server.start());

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/nope.zip"));

        QVERIFY(harness.run(task));
        QVERIFY(!task->wasSuccessful());
        QVERIFY(!QFileInfo::exists(harness.target()));
        QVERIFY(!QFileInfo::exists(Net::PartFile::partPathFor(harness.target())));
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        harness.settle(task);
    }

    /*! Caller supplied header proxies reach every request the task makes. */
    void requestDecoratorReachesEverySegment()
    {
        const QByteArray body = makeBody(LargeSize, 0x13131313u);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));
        task->setRequestDecorator([](Net::NetRequest& request) {
            auto headers = std::make_unique<Net::RawHeaderProxy>();
            headers->addHeader("Authorization", "Bearer segmented-test");
            request.addHeaderProxy(std::move(headers));
        });

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target()), body);
        QCOMPARE(server.authorizedRequestCount(), server.requestCount());
        QVERIFY(server.requestCount() >= 2);
        harness.settle(task);
    }

    /*! Modrinth download metadata has to ride on the discovery request and on every segment.
     *
     *  The real ApiHeaderProxy scopes that header to the official CDN, which loopback is not, so
     *  the header itself is put on by a raw proxy here. What is under test is the plumbing the
     *  metadata overload of makeApiFile() depends on: one decorator, every request.
     */
    void modrinthMetadataReachesEverySegment()
    {
        const QByteArray metadata = R"({"reason":"modpack","game_version":"1.20.1","loader":"fabric"})";
        const QByteArray body = makeBody(LargeSize, 0x15151515u);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/physics.jar", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/physics.jar"), 4, QStringLiteral("physics.jar"));
        task->setRequestDecorator([metadata](Net::NetRequest& request) {
            auto headers = std::make_unique<Net::RawHeaderProxy>();
            headers->addHeader("modrinth-download-meta", metadata);
            request.addHeaderProxy(std::move(headers));
        });
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(harness.target(QStringLiteral("physics.jar"))), body);

        QVERIFY2(task->segmentsUsed() >= 2, "the file was not split at all");
        QCOMPARE(server.rangeRequestCount(), 1 + task->segmentsUsed());
        QCOMPARE(server.requestCount(), 1 + task->segmentsUsed());
        for (const auto& record : server.requests()) {
            QCOMPARE(record.modrinthMeta, metadata);
        }
        harness.settle(task);
    }

    /*! The production metadata overload must not hand that metadata to a host that is not the
     *  official Modrinth CDN - and loopback is exactly such a host. */
    void modrinthMetadataIsNotSentToOtherHosts()
    {
        const QByteArray body = makeBody(LargeSize, 0x16161616u);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/physics.jar", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        const Net::ModrinthDownloadMeta meta{ .reason = QStringLiteral("modpack"),
                                              .gameVersion = QStringLiteral("1.20.1"),
                                              .loader = QStringLiteral("fabric"),
                                              .dependentOn = QStringLiteral("abcdefgh") };
        const QString target = harness.target(QStringLiteral("physics.jar"));
        auto task = SegmentedDownload::makeApiFile(server.url("/physics.jar"), target, harness.network(), harness.scheduler(), 4, meta);
        task->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, sha1Of(body)));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        QCOMPARE(readAll(target), body);

        QVERIFY2(task->segmentsUsed() >= 2, "the file was not split at all");
        QVERIFY(server.requestCount() >= 2);
        for (const auto& record : server.requests()) {
            QVERIFY2(record.modrinthMeta.isEmpty(), "Modrinth metadata was sent to a host that is not Modrinth's CDN");
        }
        QCOMPARE(server.authorizedRequestCount(), 0);
        QCOMPARE(harness.scheduler()->outstandingPermits(), 0);
        harness.settle(task);
    }

    /*! Segments ask for an unencoded body, because a coded one would break every offset. */
    void segmentsAskForIdentityEncoding()
    {
        const QByteArray body = makeBody(LargeSize, 0x14141414u);
        RangeHttpServer server;
        QVERIFY(server.start());
        server.serve("/pack.zip", simpleResource(body));

        Harness harness;
        QVERIFY(harness.valid());
        auto task = harness.create(server.url("/pack.zip"));

        QVERIFY(harness.run(task));
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        for (const auto& record : server.requests()) {
            QCOMPARE(record.acceptEncoding, QByteArrayLiteral("identity"));
        }
        harness.settle(task);
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
    SegmentedDownloadTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "SegmentedDownload_test.moc"
