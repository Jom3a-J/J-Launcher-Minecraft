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

#include <QTest>
#include <QVector>
#include <functional>
#include <memory>

#include "BuildConfig.h"
#include "net/HostScheduler.h"
#include "net/NetJob.h"
#include "net/NetRequest.h"

using Net::HostOutcome;
using Net::HostScheduler;

namespace {

QUrl resourcesUrl()
{
    return QUrl(BuildConfig.DEFAULT_RESOURCE_BASE + QStringLiteral("objects/aa/deadbeef"));
}

QUrl ftbUrl()
{
    return QUrl(BuildConfig.FTB_API_BASE_URL + QStringLiteral("/modpack/1"));
}

QUrl hostUrl(const QString& host)
{
    return QUrl(QStringLiteral("https://") + host + QStringLiteral("/a.jar"));
}

/*! A NetRequest that never touches the network.
 *
 *  From NetJob's point of view it is a real request - same type, same URL, same signals - but it
 *  only finishes when the test says so, which makes admission and permit accounting observable
 *  without any I/O.
 */
class StubRequest : public Net::NetRequest {
    Q_OBJECT

   public:
    using Ptr = shared_qobject_ptr<StubRequest>;

    explicit StubRequest(const QUrl& url, bool latencyCritical = false)
    {
        m_url = url;
        setObjectName(QStringLiteral("StubRequest"));
        setLatencyCritical(latencyCritical);
    }

    bool started() const { return m_started; }

    void succeed()
    {
        if (isRunning())
            emitSucceeded();
    }

    void fail(const QString& reason = QStringLiteral("stub failure"))
    {
        if (isRunning())
            emitFailed(reason);
    }

    /*! Reports byte progress the way a real transfer would. */
    void report(qint64 received, qint64 total) { onProgress(received, total); }

    /*! Mimics NetRequest seeing a 429/503 while it keeps running to retry internally. */
    void reportRateLimit(qint64 retryAfterSeconds) { emit rateLimited(m_url, retryAfterSeconds); }

    /*! Mimics NetRequest following a redirect onto a different host. */
    void redirectTo(const QUrl& url)
    {
        m_url = url;
        emit redirectedToNewHost(url);
    }

    bool abort() override
    {
        if (isRunning())
            emitAborted();
        return true;
    }

   private:
    QNetworkReply* getReply(QNetworkRequest&) override { return nullptr; }
    void executeTask() override { m_started = true; }

    bool m_started = false;
};

/*! Adds \a count stub requests for \a url to \a job and returns them. */
QVector<StubRequest::Ptr> addStubs(NetJob& job, const QUrl& url, int count)
{
    QVector<StubRequest::Ptr> requests;
    for (int i = 0; i < count; i++) {
        auto request = makeShared<StubRequest>(url);
        job.addNetAction(request);
        requests.append(request);
    }
    return requests;
}

int countStarted(const QVector<StubRequest::Ptr>& requests)
{
    int started = 0;
    for (const auto& request : requests) {
        if (request->started())
            started++;
    }
    return started;
}

bool waitFor(std::function<bool()> predicate, int timeoutMs = 3000)
{
    return QTest::qWaitFor(std::move(predicate), timeoutMs);
}

/*! Keeps succeeding whatever is running until \a job finishes. */
bool drainSucceeding(NetJob& job, const QVector<StubRequest::Ptr>& requests, int timeoutMs = 5000)
{
    return waitFor(
        [&] {
            for (const auto& request : requests)
                request->succeed();
            return job.isFinished();
        },
        timeoutMs);
}

}  // namespace

class NetJobSchedulingTest : public QObject {
    Q_OBJECT

   private slots:
    /*! Admission, not the job's queue width, is what limits a default job. */
    void test_perHostColdStartLimitsConcurrentRequests()
    {
        HostScheduler scheduler;
        NetJob job(QStringLiteral("test"), nullptr, -1, &scheduler);
        auto requests = addStubs(job, resourcesUrl(), 10);

        job.start();
        QVERIFY(waitFor([&] { return countStarted(requests) >= HostScheduler::ColdStartLevel; }));
        // Give any further admission attempts a chance to run before measuring.
        QTest::qWait(50);

        QCOMPARE(countStarted(requests), HostScheduler::ColdStartLevel);
        QCOMPARE(scheduler.inFlightFor(resourcesUrl()), HostScheduler::ColdStartLevel);

        // Requests waiting for a permit stay in the queue, so the job size - and with it the
        // progress denominator - does not move while they wait.
        QCOMPARE(job.size(), 10);

        QVERIFY(drainSucceeding(job, requests));
        QVERIFY(job.wasSuccessful());
        QCOMPARE(scheduler.globalInFlight(), 0);
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! An explicit per-job maximum still wins; this is what FTBPackInstallTask relies on. */
    void test_explicitJobMaximumIsHonoured()
    {
        HostScheduler scheduler;
        NetJob job(QStringLiteral("test"), nullptr, 1, &scheduler);
        auto requests = addStubs(job, resourcesUrl(), 5);

        job.start();
        QVERIFY(waitFor([&] { return countStarted(requests) >= 1; }));
        QTest::qWait(50);

        QCOMPARE(countStarted(requests), 1);
        QCOMPARE(scheduler.inFlightFor(resourcesUrl()), 1);

        QVERIFY(drainSucceeding(job, requests));
        QVERIFY(job.wasSuccessful());
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! FTB is pinned to one request by policy, even without an explicit per-job maximum. */
    void test_ftbIsPinnedToOneRequestByPolicy()
    {
        HostScheduler scheduler;
        NetJob job(QStringLiteral("test"), nullptr, -1, &scheduler);
        auto requests = addStubs(job, ftbUrl(), 4);

        job.start();
        QVERIFY(waitFor([&] { return countStarted(requests) >= 1; }));
        QTest::qWait(50);

        QCOMPARE(countStarted(requests), 1);

        QVERIFY(drainSucceeding(job, requests));
        QVERIFY(job.wasSuccessful());
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! A saturated host at the head of the queue must not stall other hosts behind it. */
    void test_noHeadOfLineBlockingAcrossHosts()
    {
        HostScheduler scheduler;
        NetJob job(QStringLiteral("test"), nullptr, -1, &scheduler);

        // Six requests to a host pinned to one, then three to a host with spare capacity.
        auto pinned = addStubs(job, ftbUrl(), 6);
        auto free = addStubs(job, resourcesUrl(), 3);

        job.start();
        QVERIFY2(waitFor([&] { return countStarted(free) == 3; }),
                 "requests to an idle host were blocked behind a saturated host");
        QCOMPARE(countStarted(pinned), 1);

        auto all = pinned;
        all.append(free);
        QVERIFY(drainSucceeding(job, all));
        QVERIFY(job.wasSuccessful());
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! Finishing a request frees its permit for a request that was waiting. */
    void test_permitsAreReleasedOnSuccess()
    {
        HostScheduler scheduler;
        NetJob job(QStringLiteral("test"), nullptr, -1, &scheduler);
        auto requests = addStubs(job, resourcesUrl(), 6);

        job.start();
        QVERIFY(waitFor([&] { return countStarted(requests) == HostScheduler::ColdStartLevel; }));

        requests[0]->succeed();
        QVERIFY(waitFor([&] { return countStarted(requests) == HostScheduler::ColdStartLevel + 1; }));
        QCOMPARE(scheduler.inFlightFor(resourcesUrl()), HostScheduler::ColdStartLevel);

        QVERIFY(drainSucceeding(job, requests));
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! A failing request gives its permit back too, across the job's own retry passes. */
    void test_permitsAreReleasedOnFailure()
    {
        HostScheduler scheduler;
        NetJob job(QStringLiteral("test"), nullptr, -1, &scheduler);
        job.setAskRetry(false);
        auto requests = addStubs(job, resourcesUrl(), 2);

        job.start();
        QVERIFY(waitFor(
            [&] {
                for (const auto& request : requests)
                    request->fail();
                return job.isFinished();
            },
            5000));

        QVERIFY(!job.wasSuccessful());
        QCOMPARE(scheduler.globalInFlight(), 0);
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! Aborting a job must not leak the permits its running requests held. */
    void test_permitsAreReleasedOnAbort()
    {
        HostScheduler scheduler;
        NetJob job(QStringLiteral("test"), nullptr, -1, &scheduler);
        auto requests = addStubs(job, resourcesUrl(), 8);

        job.start();
        QVERIFY(waitFor([&] { return countStarted(requests) == HostScheduler::ColdStartLevel; }));
        QCOMPARE(scheduler.globalInFlight(), HostScheduler::ColdStartLevel);

        QVERIFY(job.abort());
        QTest::qWait(50);

        QCOMPARE(scheduler.globalInFlight(), 0);
        QCOMPARE(scheduler.outstandingPermits(), 0);
        QCOMPARE(scheduler.inFlightFor(resourcesUrl()), 0);
    }

    /*! Destroying a job with requests still in flight must not leak permits either. */
    void test_permitsAreReleasedWhenTheJobIsDestroyed()
    {
        HostScheduler scheduler;
        QVector<StubRequest::Ptr> requests;
        {
            NetJob job(QStringLiteral("test"), nullptr, -1, &scheduler);
            requests = addStubs(job, resourcesUrl(), 4);
            job.start();
            QVERIFY(waitFor([&] { return countStarted(requests) == HostScheduler::ColdStartLevel; }));
            QCOMPARE(scheduler.globalInFlight(), HostScheduler::ColdStartLevel);
        }

        QCOMPARE(scheduler.globalInFlight(), 0);
        QCOMPARE(scheduler.outstandingPermits(), 0);

        for (const auto& request : requests)
            request->abort();
    }

    /*! A job that outlives an injected scheduler must degrade, not crash. */
    void test_jobSurvivesADestroyedInjectedScheduler()
    {
        auto scheduler = std::make_unique<HostScheduler>();
        NetJob job(QStringLiteral("test"), nullptr, -1, scheduler.get());
        auto requests = addStubs(job, resourcesUrl(), 6);

        job.start();
        QVERIFY(waitFor([&] { return countStarted(requests) == HostScheduler::ColdStartLevel; }));

        scheduler.reset();
        QVERIFY(job.scheduler() == nullptr);

        QVERIFY(drainSucceeding(job, requests));
        QVERIFY(job.wasSuccessful());
    }

    /*! Limits hold across simultaneous jobs, not just inside one. */
    void test_limitsHoldAcrossSimultaneousJobs()
    {
        HostScheduler scheduler;
        NetJob first(QStringLiteral("first"), nullptr, -1, &scheduler);
        NetJob second(QStringLiteral("second"), nullptr, -1, &scheduler);

        auto firstRequests = addStubs(first, resourcesUrl(), 4);
        auto secondRequests = addStubs(second, resourcesUrl(), 4);

        first.start();
        second.start();
        QVERIFY(waitFor([&] { return countStarted(firstRequests) + countStarted(secondRequests) >= HostScheduler::ColdStartLevel; }));
        QTest::qWait(60);

        QCOMPARE(countStarted(firstRequests) + countStarted(secondRequests), HostScheduler::ColdStartLevel);
        QCOMPARE(scheduler.inFlightFor(resourcesUrl()), HostScheduler::ColdStartLevel);

        QVERIFY(waitFor(
            [&] {
                for (const auto& request : firstRequests)
                    request->succeed();
                for (const auto& request : secondRequests)
                    request->succeed();
                return first.isFinished() && second.isFinished();
            },
            5000));
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! A cooling down host blocks admission, and recovers once the cooldown expires. */
    void test_hostCooldownBlocksAndThenRecovers()
    {
        HostScheduler scheduler;
        qint64 now = 1'000'000;
        scheduler.setClock([&now] { return now; });

        // Park the host on a cooldown before the job ever starts.
        const auto url = hostUrl(QStringLiteral("cooling.example.invalid"));
        const auto permit = scheduler.tryAcquire(url);
        QVERIFY(permit != HostScheduler::InvalidPermit);
        scheduler.reportRateLimited(url, 1 /* Retry-After: 1 */);
        scheduler.release(permit, HostOutcome::Failure);
        QCOMPARE(scheduler.cooldownRemainingFor(url), qint64(1000));

        NetJob job(QStringLiteral("test"), nullptr, -1, &scheduler);
        auto requests = addStubs(job, url, 2);
        job.start();
        QTest::qWait(60);

        QCOMPARE(countStarted(requests), 0);
        QCOMPARE(job.size(), 2);  // still queued: the denominator does not move

        // Let the cooldown lapse; the scheduler's wake-up re-drives the waiting job.
        now += 1500;
        QVERIFY2(waitFor([&] { return countStarted(requests) > 0; }, 5000), "job never recovered after the cooldown expired");

        QVERIFY(drainSucceeding(job, requests));
        QVERIFY(job.wasSuccessful());
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! A 429 that the request retries internally must stop siblings straight away. */
    void test_rateLimitWhileRunningStopsSiblingsImmediately()
    {
        HostScheduler scheduler;
        qint64 now = 1'000'000;
        scheduler.setClock([&now] { return now; });

        NetJob job(QStringLiteral("test"), nullptr, -1, &scheduler);
        auto requests = addStubs(job, resourcesUrl(), 8);

        job.start();
        QVERIFY(waitFor([&] { return countStarted(requests) == HostScheduler::ColdStartLevel; }));

        // The request stays running, exactly as it would while waiting out its retry delay.
        requests[0]->reportRateLimit(2);
        QCOMPARE(scheduler.cooldownRemainingFor(resourcesUrl()), qint64(2000));
        QCOMPARE(scheduler.inFlightFor(resourcesUrl()), HostScheduler::ColdStartLevel);

        // Finishing the other three must not let any of the queued requests start.
        for (int i = 1; i < HostScheduler::ColdStartLevel; i++)
            requests[i]->succeed();
        QTest::qWait(80);
        QCOMPARE(countStarted(requests), HostScheduler::ColdStartLevel);

        // Once the cooldown lapses the job picks up again.
        now += 2500;
        QVERIFY2(waitFor([&] { return countStarted(requests) > HostScheduler::ColdStartLevel; }, 5000),
                 "the job never resumed after the host cooldown expired");

        QVERIFY(drainSucceeding(job, requests));
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! A redirect to another host must be charged to that host, not the original one. */
    void test_crossHostRedirectIsChargedToTheDestination()
    {
        HostScheduler scheduler;
        NetJob job(QStringLiteral("test"), nullptr, -1, &scheduler);
        auto requests = addStubs(job, resourcesUrl(), 4);

        job.start();
        QVERIFY(waitFor([&] { return countStarted(requests) == HostScheduler::ColdStartLevel; }));
        QCOMPARE(scheduler.inFlightFor(resourcesUrl()), HostScheduler::ColdStartLevel);

        const auto destination = hostUrl(QStringLiteral("mirror.example.invalid"));
        requests[0]->redirectTo(destination);

        QCOMPARE(scheduler.inFlightFor(resourcesUrl()), HostScheduler::ColdStartLevel - 1);
        QCOMPARE(scheduler.inFlightFor(destination), 1);
        QCOMPARE(scheduler.globalInFlight(), HostScheduler::ColdStartLevel);

        QVERIFY(drainSucceeding(job, requests));
        QVERIFY(job.wasSuccessful());
        QCOMPARE(scheduler.globalInFlight(), 0);
        QCOMPARE(scheduler.inFlightFor(destination), 0);
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! Aggregate progress never goes backwards and ends on the exact value. */
    void test_progressIsMonotonicAndExactAtTheEnd()
    {
        HostScheduler scheduler;
        NetJob job(QStringLiteral("test"), nullptr, -1, &scheduler);
        auto requests = addStubs(job, resourcesUrl(), 12);

        QVector<QPair<qint64, qint64>> updates;
        connect(&job, &Task::progress, &job, [&updates](qint64 current, qint64 total) { updates.append({ current, total }); });

        job.start();
        QVERIFY(drainSucceeding(job, requests));
        QVERIFY(job.wasSuccessful());

        QVERIFY(!updates.isEmpty());
        qint64 previous = -1;
        for (const auto& update : updates) {
            QVERIFY2(update.first >= previous, "aggregate progress went backwards");
            QCOMPARE(update.second, qint64(12));
            previous = update.first;
        }
        QCOMPARE(updates.last().first, qint64(12));
        QCOMPARE(updates.last().second, qint64(12));
        QCOMPARE(job.getProgress(), qint64(12));
        QCOMPARE(job.getTotalProgress(), qint64(12));
    }

    /*! Per-request progress is throttled, but the completed value is always published exactly. */
    void test_perRequestProgressIsCoalescedButExact()
    {
        StubRequest request(resourcesUrl());

        QVector<QPair<qint64, qint64>> updates;
        connect(&request, &Task::progress, &request, [&updates](qint64 current, qint64 total) { updates.append({ current, total }); });

        // A burst of updates inside one throttle window collapses to a single publish.
        for (int i = 1; i <= 50; i++)
            request.report(i * 10, 1000);

        QVERIFY2(updates.size() <= 2, "progress updates were not coalesced");
        QCOMPARE(updates.first().first, qint64(10));

        // The completed value bypasses the throttle.
        const auto before = updates.size();
        request.report(1000, 1000);
        QCOMPARE(updates.size(), before + 1);
        QCOMPARE(updates.last().first, qint64(1000));
        QCOMPARE(updates.last().second, qint64(1000));

        qint64 previous = -1;
        for (const auto& update : updates) {
            QVERIFY2(update.first >= previous, "request progress went backwards");
            previous = update.first;
        }
    }

    /*! A throttled update is not lost: the flush timer publishes the latest value. */
    void test_coalescedProgressIsFlushedLater()
    {
        StubRequest request(resourcesUrl());

        QVector<qint64> updates;
        connect(&request, &Task::progress, &request, [&updates](qint64 current, qint64) { updates.append(current); });

        request.report(10, 1000);
        request.report(20, 1000);
        request.report(30, 1000);
        QVERIFY2(updates.size() <= 2, "progress updates were not coalesced");

        QVERIFY(waitFor([&] { return updates.last() == 30; }, 2000));
    }
};

QTEST_GUILESS_MAIN(NetJobSchedulingTest)

#include "NetJobScheduling_test.moc"
