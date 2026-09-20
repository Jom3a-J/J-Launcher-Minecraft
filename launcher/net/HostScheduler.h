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

#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <functional>

namespace Net {

/*! Policy bucket a remote host belongs to.
 *
 *  Classification is by exact host name (or an explicit registrable-domain suffix), never by
 *  substring matching, so that a host such as "notgithub.com.example.org" cannot inherit the
 *  policy of a provider it has nothing to do with.
 */
enum class HostClass {
    MinecraftResources,  //!< resources.download.minecraft.net
    MinecraftLibraries,  //!< libraries.minecraft.net
    FlameCdn,            //!< edge.forgecdn.net
    ModrinthCdn,         //!< cdn.modrinth.com
    FlameApi,            //!< api.curseforge.com
    ModrinthApi,         //!< api.modrinth.com / staging-api.modrinth.com
    LauncherMeta,        //!< the launcher's own metadata service
    AtlCdn,              //!< download.nodecdn.net
    CodeHosting,         //!< GitHub / GitLab / raw content hosts
    ForgeMaven,          //!< Forge and NeoForge Maven repositories
    Ftb,                 //!< Feed The Beast, permanently pinned to one request at a time
    Unknown,             //!< everything else
};

/*! How a request that held a permit ended. Drives the adaptive per-host limit. */
enum class HostOutcome {
    Success,          //!< clean completion; counts towards the ramp
    Failure,          //!< failed for a reason that says nothing about the host's capacity
    RateLimited,      //!< HTTP 429/503: halve the host and cool it down
    ConnectionReset,  //!< transport level reset: halve the host, no cooldown
    Aborted,          //!< cancelled by the user
};

/*! Process wide, provider aware admission control for network requests.
 *
 *  NetJob asks the scheduler for a permit before it starts a request and returns the permit once
 *  the request has finished. Because a single scheduler is shared by every NetJob, limits hold
 *  across simultaneous jobs instead of being a per job queue width.
 *
 *  Three limits are enforced, in this order:
 *    - a global cap (GlobalCap) on requests in flight anywhere;
 *    - a bulk cap (GlobalCap - ApiReserve) which keeps permits available for latency critical
 *      metadata and API calls, so that bulk file queues cannot starve them;
 *    - an adaptive per host limit, bounded by that host's hard provider ceiling.
 *
 *  Adaptive state deliberately lives only in memory: it is never persisted across restarts.
 *
 *  This class is not thread safe; like the rest of the task machinery it is used from the thread
 *  that owns the NetJobs (the GUI thread in the application).
 */
class HostScheduler : public QObject {
    Q_OBJECT

   public:
    /// Maximum number of requests in flight process wide.
    static constexpr int GlobalCap = 24;
    /// Permits inside GlobalCap that only latency critical metadata/API requests may use.
    static constexpr int ApiReserve = 4;
    /// Maximum number of bulk (non latency critical) requests in flight process wide.
    static constexpr int BulkCap = GlobalCap - ApiReserve;
    /// Level every adaptive host starts at, clamped by its ceiling.
    static constexpr int ColdStartLevel = 4;
    /// Clean completions required before a host is allowed one more concurrent request.
    static constexpr int CleanCompletionsPerStep = 10;
    /// The default value of the "NumberOfConcurrentDownloads" setting; the normal per host level.
    static constexpr int DefaultNormalLevel = 6;
    /// Hard ceiling for hosts we know nothing about.
    static constexpr int UnknownHostCeiling = 6;
    /// First cooldown applied to a rate limited host without a usable Retry-After header.
    static constexpr qint64 MinCooldownMs = 1000;
    /// Upper bound for any cooldown, including one derived from Retry-After.
    static constexpr qint64 MaxCooldownMs = 60 * 1000;
    /*! Requests a burst must contain before it is worth reporting.
     *
     *  Idle metadata polling goes through the scheduler too; without a floor the log would fill
     *  with two line reports that measure nothing.
     */
    static constexpr int StatsReportThreshold = 50;

    /// Opaque handle for a granted permit.
    using Permit = quint64;
    static constexpr Permit InvalidPermit = 0;

    explicit HostScheduler(QObject* parent = nullptr);
    ~HostScheduler() override;

    /*! The scheduler NetJob uses unless a different one is injected through its constructor.
     *
     *  This instance is created on first use and intentionally never destroyed. NetJobs are owned
     *  by many different objects and can still be returning permits while the application is
     *  being torn down, so the scheduler must outlive all of them. Tests do not need to touch it:
     *  they inject their own scheduler through NetJob's constructor instead.
     */
    static HostScheduler* global();

    static HostClass classify(const QUrl& url);
    static QString hostKey(const QUrl& url);
    static int hardCeiling(HostClass hostClass);
    static bool isPinned(HostClass hostClass);
    static bool isLatencyCritical(HostClass hostClass);

    /*! The download concurrency level, taken from the user's "NumberOfConcurrentDownloads"
     *  setting. Valid range is 1 to DefaultNormalLevel; larger values are clamped.
     *
     *  Provider ceilings are quoted at the default level of 6, which uses every provider's safe
     *  maximum. Lower levels scale all of those ceilings down proportionally; nothing can push a
     *  host above its hard ceiling.
     */
    void setNormalPerHostLevel(int level);
    int normalPerHostLevel() const { return m_normal_level; }

    /// Effective ceiling for a host: the hard provider ceiling, scaled by the user's setting.
    int ceilingFor(const QUrl& url) const;
    /// Number of requests the host is currently allowed to run concurrently.
    int limitFor(const QUrl& url) const;
    int inFlightFor(const QUrl& url) const;
    /// Milliseconds remaining of a host wide cooldown, or 0 when the host is not cooling down.
    qint64 cooldownRemainingFor(const QUrl& url) const;

    int globalInFlight() const { return m_global_in_flight; }
    int bulkInFlight() const { return m_bulk_in_flight; }
    int outstandingPermits() const { return static_cast<int>(m_permits.size()); }

    /*! Try to take a permit for a request to \a url.
     *
     *  Returns InvalidPermit when the request has to wait; the caller must leave the request in
     *  its queue (so queue and progress denominators stay stable) and retry once
     *  capacityAvailable() fires.
     */
    Permit tryAcquire(const QUrl& url, bool latencyCritical = false);

    /*! Give a permit back.
     *
     *  Releasing an unknown or already released permit is a no-op, so a double release can never
     *  corrupt the accounting. Retry-After is handled by reportRateLimited(), which is where a
     *  429/503 is observed; releasing with HostOutcome::RateLimited falls back to the bounded
     *  exponential cooldown.
     */
    void release(Permit permit, HostOutcome outcome);

    /*! Reports that \a url answered with 429/503 while the request is still running.
     *
     *  A request that retries a rate limited response internally does not finish for a while, so
     *  waiting for its permit to come back would let sibling requests keep hammering the host.
     *  This applies the halving and the cooldown immediately, without touching any permit.
     */
    void reportRateLimited(const QUrl& url, qint64 retryAfterSeconds = -1);

    /*! Re-charges an in-flight permit from the host it was taken for to the host of \a url.
     *
     *  Used when a request follows a redirect to a different host. The transfer is already open,
     *  so it is counted against the new host rather than blocked - which can briefly put that
     *  host above its limit, but new requests still respect the ceiling until it drains.
     */
    void migratePermit(Permit permit, const QUrl& url);

    /*! A human readable summary of what the last burst of downloading did, per host.
     *
     *  Only measurement: granted and denied admissions, how long requests actually took, and
     *  how often a host rate limited us. It is what tells apart "thousands of tiny files each
     *  costing a round trip" from "a few big files on a slow link" from "queued behind our own
     *  concurrency limit", which are three different problems with three different fixes.
     */
    QString statsReport() const;
    /// Forgets the collected measurements without touching adaptive state.
    void resetStats();

    /// Drops all adaptive state and accounting. Intended for tests.
    void reset();
    /// Replaces the millisecond clock used for cooldowns. Intended for tests.
    void setClock(std::function<qint64()> clock);

   signals:
    /*! Emitted when a permit was returned or a host wide cooldown expired.
     *
     *  Waiting NetJobs use this to re-run admission without polling.
     */
    void capacityAvailable();

   private:
    struct HostState {
        HostClass hostClass = HostClass::Unknown;
        int limit = 0;
        int inFlight = 0;
        int cleanCompletions = 0;
        int penalties = 0;
        qint64 cooldownUntil = 0;
    };

    struct PermitState {
        QString host;
        bool latencyCritical = false;
        qint64 acquiredAt = 0;
    };

    /// Pure measurement; nothing here feeds back into admission decisions.
    struct HostStats {
        int granted = 0;
        int denied = 0;
        int rateLimited = 0;
        qint64 serviceMsTotal = 0;
        qint64 serviceMsMax = 0;
    };

    HostState& stateFor(const QString& key, HostClass hostClass);
    int scaledCeiling(HostClass hostClass) const;
    int effectiveLimit(const HostState& state) const;
    void applyOutcome(HostState& state, HostOutcome outcome, qint64 retryAfterSeconds);
    void scheduleWakeup();
    void onWakeup();
    /// Logs statsReport() once the last request of a large enough burst has drained.
    void maybeReportStats();

    QHash<QString, HostState> m_hosts;
    QHash<Permit, PermitState> m_permits;
    QHash<QString, HostStats> m_stats;
    int m_stats_granted = 0;
    qint64 m_stats_started_at = 0;

    Permit m_next_permit = 0;
    int m_global_in_flight = 0;
    int m_bulk_in_flight = 0;
    int m_normal_level = DefaultNormalLevel;

    std::function<qint64()> m_clock;
    QTimer m_wakeup;
};

}  // namespace Net
