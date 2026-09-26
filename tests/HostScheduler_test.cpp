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

#include <QSignalSpy>
#include <QTest>
#include <QVector>

#include "BuildConfig.h"
#include "net/HostScheduler.h"

using Net::HostClass;
using Net::HostOutcome;
using Net::HostScheduler;

namespace {

QUrl resourcesUrl(const QString& path = QStringLiteral("a"))
{
    return QUrl(BuildConfig.DEFAULT_RESOURCE_BASE + path);
}

QUrl librariesUrl()
{
    return QUrl(BuildConfig.LIBRARY_BASE + QStringLiteral("a.jar"));
}

QUrl flameCdnUrl()
{
    return QUrl(QStringLiteral("https://") + BuildConfig.FLAME_DOWNLOAD_HOST + QStringLiteral("/files/1/a.jar"));
}

QUrl mediafilezCdnUrl()
{
    return QUrl(QStringLiteral("https://mediafilez.forgecdn.net/files/1/a.jar"));
}

QUrl modrinthCdnUrl()
{
    return QUrl(QStringLiteral("https://") + BuildConfig.MODRINTH_DOWNLOAD_HOST + QStringLiteral("/data/a.jar"));
}

QUrl ftbUrl()
{
    return QUrl(BuildConfig.FTB_API_BASE_URL + QStringLiteral("/modpack/1"));
}

QUrl unknownUrl(const QString& host = QStringLiteral("files.example.invalid"))
{
    return QUrl(QStringLiteral("https://") + host + QStringLiteral("/a.jar"));
}

/*! Runs \a count clean completions so that a host ramps by \a count / CleanCompletionsPerStep. */
void completeCleanly(HostScheduler& scheduler, const QUrl& url, int count)
{
    for (int i = 0; i < count; i++) {
        const auto permit = scheduler.tryAcquire(url);
        QVERIFY(permit != HostScheduler::InvalidPermit);
        scheduler.release(permit, HostOutcome::Success);
    }
}

}  // namespace

class HostSchedulerTest : public QObject {
    Q_OBJECT

   private slots:
    void test_classifiesKnownHostsExactly()
    {
        QCOMPARE(HostScheduler::classify(resourcesUrl()), HostClass::MinecraftResources);
        QCOMPARE(HostScheduler::classify(librariesUrl()), HostClass::MinecraftLibraries);
        QCOMPARE(HostScheduler::classify(flameCdnUrl()), HostClass::FlameCdn);
        QCOMPARE(HostScheduler::classify(modrinthCdnUrl()), HostClass::ModrinthCdn);
        QCOMPARE(HostScheduler::classify(QUrl(BuildConfig.FLAME_BASE_URL + QStringLiteral("/mods"))), HostClass::FlameApi);
        QCOMPARE(HostScheduler::classify(QUrl(BuildConfig.MODRINTH_PROD_URL + QStringLiteral("/search"))), HostClass::ModrinthApi);
        QCOMPARE(HostScheduler::classify(QUrl(BuildConfig.MODRINTH_STAGING_URL + QStringLiteral("/search"))), HostClass::ModrinthApi);
        QCOMPARE(HostScheduler::classify(QUrl(BuildConfig.ATL_DOWNLOAD_SERVER_URL + QStringLiteral("a.zip"))), HostClass::AtlCdn);
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://github.com/o/r/a.jar"))), HostClass::CodeHosting);
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://raw.githubusercontent.com/o/r/a"))), HostClass::CodeHosting);
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://gitlab.com/o/r/a.jar"))), HostClass::CodeHosting);
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://maven.minecraftforge.net/a.jar"))), HostClass::ForgeMaven);
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://maven.neoforged.net/a.jar"))), HostClass::ForgeMaven);
        QCOMPARE(HostScheduler::classify(ftbUrl()), HostClass::Ftb);
        QCOMPARE(HostScheduler::classify(QUrl(BuildConfig.LEGACY_FTB_CDN_BASE_URL + QStringLiteral("a.zip"))), HostClass::Ftb);
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://files.feed-the-beast.com/a.zip"))), HostClass::Ftb);
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://cdn.feed-the-beast.com/a.zip"))), HostClass::Ftb);
        QCOMPARE(HostScheduler::classify(unknownUrl()), HostClass::Unknown);
    }

    void test_ftbHostsArePinnedWhileCurseForgeCanRunInParallel()
    {
        HostScheduler scheduler;
        const QUrl filesUrl(QStringLiteral("https://files.feed-the-beast.com/a.zip"));
        const QUrl cdnUrl(QStringLiteral("https://cdn.feed-the-beast.com/b.zip"));
        const QUrl curseForgeUrl(QStringLiteral("https://edge.forgecdn.net/files/1/a.jar"));
        QCOMPARE(HostScheduler::classify(curseForgeUrl), HostClass::FlameCdn);

        const auto filesPermit = scheduler.tryAcquire(filesUrl);
        QVERIFY(filesPermit != HostScheduler::InvalidPermit);
        QCOMPARE(scheduler.tryAcquire(filesUrl), HostScheduler::InvalidPermit);

        const auto cdnPermit = scheduler.tryAcquire(cdnUrl);
        QVERIFY(cdnPermit != HostScheduler::InvalidPermit);
        QCOMPARE(scheduler.tryAcquire(cdnUrl), HostScheduler::InvalidPermit);

        const auto curseForgePermit = scheduler.tryAcquire(curseForgeUrl);
        QVERIFY(curseForgePermit != HostScheduler::InvalidPermit);
        QCOMPARE(scheduler.inFlightFor(filesUrl), 1);
        QCOMPARE(scheduler.inFlightFor(cdnUrl), 1);
        QCOMPARE(scheduler.inFlightFor(curseForgeUrl), 1);

        scheduler.release(filesPermit, HostOutcome::Success);
        scheduler.release(cdnPermit, HostOutcome::Success);
        scheduler.release(curseForgePermit, HostOutcome::Success);
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! A host that merely contains a provider's name must not inherit its policy. */
    void test_classificationIsNotSubstringMatching()
    {
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://notgithub.com/a"))), HostClass::Unknown);
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://github.com.evil.invalid/a"))), HostClass::Unknown);
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://cdn.modrinth.com.evil.invalid/a"))), HostClass::Unknown);
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://evilgitlab.com/a"))), HostClass::Unknown);

        // A real sub domain of a code hosting domain still matches.
        QCOMPARE(HostScheduler::classify(QUrl(QStringLiteral("https://objects.githubusercontent.com/a"))), HostClass::CodeHosting);
    }

    void test_hardCeilings()
    {
        QCOMPARE(HostScheduler::hardCeiling(HostClass::MinecraftResources), 16);
        QCOMPARE(HostScheduler::hardCeiling(HostClass::MinecraftLibraries), 12);
        QCOMPARE(HostScheduler::hardCeiling(HostClass::FlameCdn), 16);
        QCOMPARE(HostScheduler::hardCeiling(HostClass::ModrinthCdn), 16);
        QCOMPARE(HostScheduler::hardCeiling(HostClass::FlameApi), 4);
        QCOMPARE(HostScheduler::hardCeiling(HostClass::ModrinthApi), 4);
        QCOMPARE(HostScheduler::hardCeiling(HostClass::LauncherMeta), 4);
        QCOMPARE(HostScheduler::hardCeiling(HostClass::AtlCdn), 4);
        QCOMPARE(HostScheduler::hardCeiling(HostClass::CodeHosting), 4);
        QCOMPARE(HostScheduler::hardCeiling(HostClass::ForgeMaven), 4);
        QCOMPARE(HostScheduler::hardCeiling(HostClass::Ftb), 1);
        QCOMPARE(HostScheduler::hardCeiling(HostClass::Unknown), 6);
    }

    void test_apiHostsAreLatencyCritical()
    {
        QVERIFY(HostScheduler::isLatencyCritical(HostClass::FlameApi));
        QVERIFY(HostScheduler::isLatencyCritical(HostClass::ModrinthApi));
        QVERIFY(HostScheduler::isLatencyCritical(HostClass::LauncherMeta));
        QVERIFY(!HostScheduler::isLatencyCritical(HostClass::FlameCdn));
        QVERIFY(!HostScheduler::isLatencyCritical(HostClass::MinecraftResources));
        QVERIFY(!HostScheduler::isLatencyCritical(HostClass::Unknown));
    }

    void test_coldStartsAtFourAndRampsToCeiling()
    {
        HostScheduler scheduler;
        const auto url = resourcesUrl();

        QCOMPARE(scheduler.limitFor(url), HostScheduler::ColdStartLevel);

        // Four permits, then the fifth has to wait.
        QVector<HostScheduler::Permit> permits;
        for (int i = 0; i < HostScheduler::ColdStartLevel; i++) {
            const auto permit = scheduler.tryAcquire(url);
            QVERIFY(permit != HostScheduler::InvalidPermit);
            permits.append(permit);
        }
        QCOMPARE(scheduler.tryAcquire(url), HostScheduler::InvalidPermit);
        for (const auto permit : permits)
            scheduler.release(permit, HostOutcome::Success);

        // Four clean completions happened above; six more make the first full step.
        completeCleanly(scheduler, url, HostScheduler::CleanCompletionsPerStep - HostScheduler::ColdStartLevel);
        QCOMPARE(scheduler.limitFor(url), HostScheduler::ColdStartLevel + 1);

        completeCleanly(scheduler, url, HostScheduler::CleanCompletionsPerStep);
        QCOMPARE(scheduler.limitFor(url), HostScheduler::ColdStartLevel + 2);
    }

    void test_flameCdnHasHigherColdStartThanOtherHosts()
    {
        HostScheduler scheduler;
        const auto flame = flameCdnUrl();
        const auto unknown = unknownUrl();

        QCOMPARE(scheduler.limitFor(flame), HostScheduler::FlameCdnColdStartLevel);
        QCOMPARE(scheduler.limitFor(unknown), HostScheduler::ColdStartLevel);
    }

    void test_flameCdnColdStartIsClampedToScaledCeiling()
    {
        HostScheduler scheduler;
        scheduler.setNormalPerHostLevel(2);
        const auto url = flameCdnUrl();

        // 16 * 2 / 6 rounds up to a ceiling of 6, below the Flame CDN cold start of 8.
        QCOMPARE(scheduler.ceilingFor(url), 6);
        QCOMPARE(scheduler.limitFor(url), 6);

        QVector<HostScheduler::Permit> permits;
        for (int i = 0; i < 6; i++) {
            const auto permit = scheduler.tryAcquire(url);
            QVERIFY(permit != HostScheduler::InvalidPermit);
            permits.append(permit);
        }
        QCOMPARE(scheduler.tryAcquire(url), HostScheduler::InvalidPermit);
        QCOMPARE(scheduler.inFlightFor(url), 6);
        for (const auto permit : permits)
            scheduler.release(permit, HostOutcome::Aborted);
    }

    void test_rampStopsAtTheCeiling()
    {
        HostScheduler scheduler;
        // An unknown host starts at 4 and must never ramp past 6.
        const auto url = unknownUrl();
        QCOMPARE(scheduler.limitFor(url), HostScheduler::ColdStartLevel);

        completeCleanly(scheduler, url, HostScheduler::CleanCompletionsPerStep * 20);
        QCOMPARE(scheduler.limitFor(url), HostScheduler::UnknownHostCeiling);
    }

    void test_perHostCapIsEnforcedAtTheCeiling()
    {
        HostScheduler scheduler;
        const auto url = flameCdnUrl();
        completeCleanly(scheduler, url, HostScheduler::CleanCompletionsPerStep * 20);
        QCOMPARE(scheduler.limitFor(url), HostScheduler::hardCeiling(HostClass::FlameCdn));

        QVector<HostScheduler::Permit> permits;
        for (int i = 0; i < HostScheduler::hardCeiling(HostClass::FlameCdn); i++) {
            const auto permit = scheduler.tryAcquire(url);
            QVERIFY(permit != HostScheduler::InvalidPermit);
            permits.append(permit);
        }
        QCOMPARE(scheduler.tryAcquire(url), HostScheduler::InvalidPermit);
        QCOMPARE(scheduler.inFlightFor(url), HostScheduler::hardCeiling(HostClass::FlameCdn));

        for (const auto permit : permits)
            scheduler.release(permit, HostOutcome::Success);
        QCOMPARE(scheduler.inFlightFor(url), 0);
    }

    void test_userSettingNeverExceedsProviderCeilings()
    {
        HostScheduler scheduler;

        // The default normal level reproduces the published ceilings exactly.
        QCOMPARE(scheduler.ceilingFor(resourcesUrl()), 16);
        QCOMPARE(scheduler.ceilingFor(unknownUrl()), 6);
        QCOMPARE(scheduler.ceilingFor(flameCdnUrl()), 16);

        // Asking for far more than any provider allows changes nothing.
        scheduler.setNormalPerHostLevel(HostScheduler::GlobalCap);
        QCOMPARE(scheduler.ceilingFor(resourcesUrl()), 16);
        QCOMPARE(scheduler.ceilingFor(unknownUrl()), 6);
        QCOMPARE(scheduler.ceilingFor(QUrl(BuildConfig.FLAME_BASE_URL)), 4);

        // Lowering it throttles every host, but never below one.
        scheduler.setNormalPerHostLevel(3);
        QCOMPARE(scheduler.ceilingFor(resourcesUrl()), 8);
        QCOMPARE(scheduler.ceilingFor(librariesUrl()), 6);
        QCOMPARE(scheduler.ceilingFor(unknownUrl()), 3);
        QCOMPARE(scheduler.ceilingFor(QUrl(BuildConfig.FLAME_BASE_URL)), 2);

        scheduler.setNormalPerHostLevel(1);
        QCOMPARE(scheduler.ceilingFor(unknownUrl()), 1);
        QCOMPARE(scheduler.ceilingFor(ftbUrl()), 1);
    }

    /*! Lowering the setting has to bite immediately, even for an already ramped host. */
    void test_loweringTheSettingClampsARampedHost()
    {
        HostScheduler scheduler;
        const auto url = resourcesUrl();
        completeCleanly(scheduler, url, HostScheduler::CleanCompletionsPerStep * 20);
        QCOMPARE(scheduler.limitFor(url), 16);

        scheduler.setNormalPerHostLevel(1);
        QCOMPARE(scheduler.limitFor(url), 3);
    }

    void test_globalCapIsEnforcedAcrossHosts()
    {
        HostScheduler scheduler;
        QVector<HostScheduler::Permit> permits;

        // Spread bulk requests over enough distinct hosts to reach the bulk cap.
        for (int host = 0; permits.size() < HostScheduler::BulkCap; host++) {
            const auto url = unknownUrl(QStringLiteral("host%1.example.invalid").arg(host));
            while (permits.size() < HostScheduler::BulkCap) {
                const auto permit = scheduler.tryAcquire(url);
                if (permit == HostScheduler::InvalidPermit)
                    break;
                permits.append(permit);
            }
        }

        QCOMPARE(scheduler.bulkInFlight(), HostScheduler::BulkCap);
        // No bulk request gets in beyond the bulk cap, however fresh the host is.
        QCOMPARE(scheduler.tryAcquire(unknownUrl(QStringLiteral("fresh.example.invalid"))), HostScheduler::InvalidPermit);

        for (const auto permit : permits)
            scheduler.release(permit, HostOutcome::Success);
        QCOMPARE(scheduler.globalInFlight(), 0);
        QCOMPARE(scheduler.bulkInFlight(), 0);
        QCOMPARE(scheduler.outstandingPermits(), 0);
    }

    /*! The API reserve keeps metadata moving while bulk queues are saturated. */
    void test_apiReserveSurvivesSaturatedBulkQueues()
    {
        HostScheduler scheduler;
        QVector<HostScheduler::Permit> bulk;

        for (int host = 0; bulk.size() < HostScheduler::BulkCap; host++) {
            const auto url = unknownUrl(QStringLiteral("bulk%1.example.invalid").arg(host));
            while (bulk.size() < HostScheduler::BulkCap) {
                const auto permit = scheduler.tryAcquire(url);
                if (permit == HostScheduler::InvalidPermit)
                    break;
                bulk.append(permit);
            }
        }
        QCOMPARE(scheduler.bulkInFlight(), HostScheduler::BulkCap);

        // The reserved permits are still there for API traffic.
        QVector<HostScheduler::Permit> api;
        const auto apiUrl = QUrl(BuildConfig.FLAME_BASE_URL + QStringLiteral("/mods"));
        for (int i = 0; i < HostScheduler::ApiReserve; i++) {
            const auto permit = scheduler.tryAcquire(apiUrl);
            QVERIFY2(permit != HostScheduler::InvalidPermit, "API request starved by bulk downloads");
            api.append(permit);
        }
        QCOMPARE(scheduler.globalInFlight(), HostScheduler::GlobalCap);
        // And the global cap still holds.
        QCOMPARE(scheduler.tryAcquire(QUrl(BuildConfig.MODRINTH_PROD_URL)), HostScheduler::InvalidPermit);

        for (const auto permit : bulk)
            scheduler.release(permit, HostOutcome::Success);
        for (const auto permit : api)
            scheduler.release(permit, HostOutcome::Success);
        QCOMPARE(scheduler.globalInFlight(), 0);
    }

    /*! An explicitly latency critical request reaches the reserve even on a bulk host. */
    void test_explicitLatencyCriticalFlagUsesTheReserve()
    {
        HostScheduler scheduler;
        QVector<HostScheduler::Permit> bulk;
        for (int host = 0; bulk.size() < HostScheduler::BulkCap; host++) {
            const auto url = unknownUrl(QStringLiteral("bulk%1.example.invalid").arg(host));
            while (bulk.size() < HostScheduler::BulkCap) {
                const auto permit = scheduler.tryAcquire(url);
                if (permit == HostScheduler::InvalidPermit)
                    break;
                bulk.append(permit);
            }
        }

        const auto metadata = unknownUrl(QStringLiteral("meta.example.invalid"));
        QCOMPARE(scheduler.tryAcquire(metadata, false), HostScheduler::InvalidPermit);
        const auto permit = scheduler.tryAcquire(metadata, true);
        QVERIFY(permit != HostScheduler::InvalidPermit);

        scheduler.release(permit, HostOutcome::Success);
        for (const auto held : bulk)
            scheduler.release(held, HostOutcome::Success);
    }

    void test_rateLimitHalvesAndCoolsDownWithRetryAfter()
    {
        HostScheduler scheduler;
        qint64 now = 1'000'000;
        scheduler.setClock([&now] { return now; });

        const auto url = resourcesUrl();
        completeCleanly(scheduler, url, HostScheduler::CleanCompletionsPerStep * 4);
        QCOMPARE(scheduler.limitFor(url), 8);

        const auto permit = scheduler.tryAcquire(url);
        QVERIFY(permit != HostScheduler::InvalidPermit);
        scheduler.reportRateLimited(url, 5 /* Retry-After: 5 */);
        scheduler.release(permit, HostOutcome::Failure);

        QCOMPARE(scheduler.limitFor(url), 4);
        QCOMPARE(scheduler.cooldownRemainingFor(url), qint64(5000));
        QCOMPARE(scheduler.tryAcquire(url), HostScheduler::InvalidPermit);

        // Another host is unaffected by one host's cooldown.
        const auto other = scheduler.tryAcquire(librariesUrl());
        QVERIFY(other != HostScheduler::InvalidPermit);
        scheduler.release(other, HostOutcome::Success);

        now += 4999;
        QCOMPARE(scheduler.tryAcquire(url), HostScheduler::InvalidPermit);

        now += 2;
        const auto recovered = scheduler.tryAcquire(url);
        QVERIFY2(recovered != HostScheduler::InvalidPermit, "host did not recover after its cooldown expired");
        scheduler.release(recovered, HostOutcome::Success);
    }

    /*! A request that retries a 429 internally keeps its permit, but siblings are stopped now. */
    void test_reportRateLimitedStopsSiblingsWhileTheRequestKeepsItsPermit()
    {
        HostScheduler scheduler;
        qint64 now = 1'000'000;
        scheduler.setClock([&now] { return now; });

        const auto url = resourcesUrl();
        const auto held = scheduler.tryAcquire(url);
        const auto sibling = scheduler.tryAcquire(url);
        QVERIFY(held != HostScheduler::InvalidPermit);
        QVERIFY(sibling != HostScheduler::InvalidPermit);
        QCOMPARE(scheduler.inFlightFor(url), 2);

        scheduler.reportRateLimited(url, 3);

        // The in-flight requests are untouched: they keep their permits and can finish retrying.
        QCOMPARE(scheduler.inFlightFor(url), 2);
        QCOMPARE(scheduler.globalInFlight(), 2);
        // But nothing new starts against that host until the cooldown lapses.
        QCOMPARE(scheduler.cooldownRemainingFor(url), qint64(3000));
        QCOMPARE(scheduler.tryAcquire(url), HostScheduler::InvalidPermit);
        QCOMPARE(scheduler.limitFor(url), 2);

        scheduler.release(held, HostOutcome::Success);
        scheduler.release(sibling, HostOutcome::Success);
        QCOMPARE(scheduler.tryAcquire(url), HostScheduler::InvalidPermit);

        now += 3001;
        const auto recovered = scheduler.tryAcquire(url);
        QVERIFY(recovered != HostScheduler::InvalidPermit);
        scheduler.release(recovered, HostOutcome::Success);
    }

    /*! A redirect across hosts re-charges the permit so the destination's ceiling still counts. */
    void test_migratePermitMovesInFlightAccountingToTheNewHost()
    {
        HostScheduler scheduler;
        const auto from = resourcesUrl();
        const auto to = flameCdnUrl();

        const auto permit = scheduler.tryAcquire(from);
        QVERIFY(permit != HostScheduler::InvalidPermit);
        QCOMPARE(scheduler.inFlightFor(from), 1);
        QCOMPARE(scheduler.inFlightFor(to), 0);

        scheduler.migratePermit(permit, to);

        QCOMPARE(scheduler.inFlightFor(from), 0);
        QCOMPARE(scheduler.inFlightFor(to), 1);
        // The request is still exactly one request in flight overall.
        QCOMPARE(scheduler.globalInFlight(), 1);
        QCOMPARE(scheduler.bulkInFlight(), 1);

        // The destination's ceiling now accounts for the redirected transfer.
        QVector<HostScheduler::Permit> extra;
        while (true) {
            const auto next = scheduler.tryAcquire(to);
            if (next == HostScheduler::InvalidPermit)
                break;
            extra.append(next);
        }
        QCOMPARE(scheduler.inFlightFor(to), HostScheduler::FlameCdnColdStartLevel);
        QCOMPARE(extra.size(), HostScheduler::FlameCdnColdStartLevel - 1);

        scheduler.release(permit, HostOutcome::Success);
        for (const auto next : extra)
            scheduler.release(next, HostOutcome::Success);
        QCOMPARE(scheduler.globalInFlight(), 0);
        QCOMPARE(scheduler.inFlightFor(to), 0);
    }

    /*! Flame CDN redirects keep their original charge so edge can ramp independently. */
    void test_flameCdnRedirectKeepsPermitOnAdmittingHost()
    {
        HostScheduler scheduler;
        const auto edge = flameCdnUrl();
        const auto mediafilez = mediafilezCdnUrl();

        QCOMPARE(scheduler.limitFor(edge), HostScheduler::FlameCdnColdStartLevel);
        const auto redirected = scheduler.tryAcquire(edge);
        QVERIFY(redirected != HostScheduler::InvalidPermit);
        scheduler.migratePermit(redirected, mediafilez);

        QCOMPARE(scheduler.inFlightFor(edge), 1);
        QCOMPARE(scheduler.inFlightFor(mediafilez), 0);

        // The redirected request's successful completion still ramps the host that admitted it.
        scheduler.release(redirected, HostOutcome::Success);
        completeCleanly(scheduler, edge, HostScheduler::CleanCompletionsPerStep - 1);
        QCOMPARE(scheduler.limitFor(edge), HostScheduler::FlameCdnColdStartLevel + 1);
        QCOMPARE(scheduler.inFlightFor(edge), 0);
        QCOMPARE(scheduler.inFlightFor(mediafilez), 0);
    }

    /*! In-flight redirects cannot consume the independent direct mediafilez allowance. */
    void test_flameCdnRedirectsLeaveDirectDestinationCapacityAvailable()
    {
        HostScheduler scheduler;
        const auto edge = flameCdnUrl();
        const auto mediafilez = mediafilezCdnUrl();
        QVector<HostScheduler::Permit> redirected;

        for (int i = 0; i < HostScheduler::FlameCdnColdStartLevel; i++) {
            const auto permit = scheduler.tryAcquire(edge);
            QVERIFY(permit != HostScheduler::InvalidPermit);
            redirected.append(permit);
            scheduler.migratePermit(permit, mediafilez);
        }

        QCOMPARE(scheduler.inFlightFor(edge), HostScheduler::FlameCdnColdStartLevel);
        QCOMPARE(scheduler.inFlightFor(mediafilez), 0);

        QVector<HostScheduler::Permit> direct;
        for (int i = 0; i < HostScheduler::FlameCdnColdStartLevel; i++) {
            const auto permit = scheduler.tryAcquire(mediafilez);
            QVERIFY(permit != HostScheduler::InvalidPermit);
            direct.append(permit);
        }
        QCOMPARE(scheduler.tryAcquire(mediafilez), HostScheduler::InvalidPermit);
        QCOMPARE(scheduler.inFlightFor(mediafilez), HostScheduler::FlameCdnColdStartLevel);

        for (const auto permit : redirected)
            scheduler.release(permit, HostOutcome::Success);
        for (const auto permit : direct)
            scheduler.release(permit, HostOutcome::Success);
        QCOMPARE(scheduler.globalInFlight(), 0);
    }

    /*! Redirects to a host outside the Flame CDN class still move their charge. */
    void test_flameCdnPermitStillMovesToUnknownHost()
    {
        HostScheduler scheduler;
        const auto from = flameCdnUrl();
        const auto to = unknownUrl();

        const auto permit = scheduler.tryAcquire(from);
        QVERIFY(permit != HostScheduler::InvalidPermit);
        scheduler.migratePermit(permit, to);

        QCOMPARE(scheduler.inFlightFor(from), 0);
        QCOMPARE(scheduler.inFlightFor(to), 1);
        QCOMPARE(scheduler.globalInFlight(), 1);

        scheduler.release(permit, HostOutcome::Success);
        QCOMPARE(scheduler.inFlightFor(to), 0);
        QCOMPARE(scheduler.globalInFlight(), 0);
    }

    /*! Migrating an unknown permit, or one that is not actually moving, changes nothing. */
    void test_migratePermitIgnoresNoOpsAndUnknownPermits()
    {
        HostScheduler scheduler;
        const auto url = resourcesUrl();
        const auto permit = scheduler.tryAcquire(url);

        scheduler.migratePermit(permit, resourcesUrl(QStringLiteral("b")));  // same host
        QCOMPARE(scheduler.inFlightFor(url), 1);

        scheduler.migratePermit(987654, flameCdnUrl());  // never issued
        QCOMPARE(scheduler.inFlightFor(url), 1);
        QCOMPARE(scheduler.inFlightFor(flameCdnUrl()), 0);
        QCOMPARE(scheduler.globalInFlight(), 1);

        scheduler.release(permit, HostOutcome::Success);
        QCOMPARE(scheduler.globalInFlight(), 0);
    }

    void test_rateLimitWithoutRetryAfterBacksOffExponentiallyAndIsBounded()
    {
        HostScheduler scheduler;
        qint64 now = 0;
        scheduler.setClock([&now] { return now; });

        const auto url = unknownUrl();
        const auto penalise = [&] {
            now += HostScheduler::MaxCooldownMs;  // clear any previous cooldown first
            const auto permit = scheduler.tryAcquire(url);
            QVERIFY(permit != HostScheduler::InvalidPermit);
            scheduler.reportRateLimited(url, -1 /* no Retry-After */);
            scheduler.release(permit, HostOutcome::Failure);
        };

        penalise();
        QCOMPARE(scheduler.cooldownRemainingFor(url), HostScheduler::MinCooldownMs);
        penalise();
        QCOMPARE(scheduler.cooldownRemainingFor(url), HostScheduler::MinCooldownMs * 2);
        penalise();
        QCOMPARE(scheduler.cooldownRemainingFor(url), HostScheduler::MinCooldownMs * 4);

        for (int i = 0; i < 12; i++)
            penalise();
        QCOMPARE(scheduler.cooldownRemainingFor(url), HostScheduler::MaxCooldownMs);
    }

    void test_serviceUnavailableCooldownIsBoundedByTheMaximum()
    {
        HostScheduler scheduler;
        qint64 now = 0;
        scheduler.setClock([&now] { return now; });

        const auto url = unknownUrl();
        const auto permit = scheduler.tryAcquire(url);
        // A hostile Retry-After must not park a host for hours.
        scheduler.reportRateLimited(url, 86400);
        scheduler.release(permit, HostOutcome::Failure);
        QCOMPARE(scheduler.cooldownRemainingFor(url), HostScheduler::MaxCooldownMs);
    }

    void test_connectionResetHalvesWithoutCooldown()
    {
        HostScheduler scheduler;
        qint64 now = 0;
        scheduler.setClock([&now] { return now; });

        const auto url = librariesUrl();
        completeCleanly(scheduler, url, HostScheduler::CleanCompletionsPerStep * 4);
        QCOMPARE(scheduler.limitFor(url), 8);

        const auto permit = scheduler.tryAcquire(url);
        scheduler.release(permit, HostOutcome::ConnectionReset);

        QCOMPARE(scheduler.limitFor(url), 4);
        QCOMPARE(scheduler.cooldownRemainingFor(url), qint64(0));
        const auto next = scheduler.tryAcquire(url);
        QVERIFY2(next != HostScheduler::InvalidPermit, "a reset must not put the host into a cooldown");
        scheduler.release(next, HostOutcome::Success);
    }

    void test_plainFailuresDoNotChangeTheLimit()
    {
        HostScheduler scheduler;
        const auto url = librariesUrl();
        completeCleanly(scheduler, url, HostScheduler::CleanCompletionsPerStep);
        QCOMPARE(scheduler.limitFor(url), 5);

        for (int i = 0; i < 5; i++) {
            const auto permit = scheduler.tryAcquire(url);
            scheduler.release(permit, i % 2 ? HostOutcome::Failure : HostOutcome::Aborted);
        }
        QCOMPARE(scheduler.limitFor(url), 5);
        QCOMPARE(scheduler.inFlightFor(url), 0);
    }

    void test_ftbIsPinnedToOneAndNeverRamps()
    {
        HostScheduler scheduler;
        const auto url = ftbUrl();

        QCOMPARE(scheduler.limitFor(url), 1);
        const auto first = scheduler.tryAcquire(url);
        QVERIFY(first != HostScheduler::InvalidPermit);
        QCOMPARE(scheduler.tryAcquire(url), HostScheduler::InvalidPermit);
        scheduler.release(first, HostOutcome::Success);

        completeCleanly(scheduler, url, HostScheduler::CleanCompletionsPerStep * 20);
        QCOMPARE(scheduler.limitFor(url), 1);

        // Raising the setting to the maximum cannot unpin it either.
        scheduler.setNormalPerHostLevel(HostScheduler::GlobalCap);
        QCOMPARE(scheduler.limitFor(url), 1);
        QCOMPARE(scheduler.ceilingFor(url), 1);
    }

    void test_doubleReleaseIsIgnored()
    {
        HostScheduler scheduler;
        const auto url = resourcesUrl();
        const auto permit = scheduler.tryAcquire(url);

        scheduler.release(permit, HostOutcome::Success);
        QCOMPARE(scheduler.globalInFlight(), 0);

        // Releasing the same permit again, or one that was never issued, must not corrupt anything.
        scheduler.release(permit, HostOutcome::Success);
        scheduler.release(123456, HostOutcome::Failure);
        QCOMPARE(scheduler.globalInFlight(), 0);
        QCOMPARE(scheduler.bulkInFlight(), 0);
        QCOMPARE(scheduler.inFlightFor(url), 0);
    }

    void test_capacityAvailableIsEmittedOnRelease()
    {
        HostScheduler scheduler;
        QSignalSpy spy(&scheduler, &HostScheduler::capacityAvailable);

        const auto permit = scheduler.tryAcquire(resourcesUrl());
        QCOMPARE(spy.count(), 0);
        scheduler.release(permit, HostOutcome::Success);
        QCOMPARE(spy.count(), 1);
    }

    void test_resetClearsAdaptiveState()
    {
        HostScheduler scheduler;
        const auto url = resourcesUrl();
        completeCleanly(scheduler, url, HostScheduler::CleanCompletionsPerStep * 3);
        QVERIFY(scheduler.limitFor(url) > HostScheduler::ColdStartLevel);
        const auto held = scheduler.tryAcquire(url);
        Q_UNUSED(held)

        scheduler.reset();

        QCOMPARE(scheduler.limitFor(url), HostScheduler::ColdStartLevel);
        QCOMPARE(scheduler.globalInFlight(), 0);
        QCOMPARE(scheduler.outstandingPermits(), 0);
        QCOMPARE(scheduler.normalPerHostLevel(), HostScheduler::DefaultNormalLevel);
    }

    /*! The process wide instance has to be usable and stable for callers that do not inject one. */
    void test_globalInstanceIsStable()
    {
        QVERIFY(HostScheduler::global() != nullptr);
        QCOMPARE(HostScheduler::global(), HostScheduler::global());
    }
};

QTEST_GUILESS_MAIN(HostSchedulerTest)

#include "HostScheduler_test.moc"
