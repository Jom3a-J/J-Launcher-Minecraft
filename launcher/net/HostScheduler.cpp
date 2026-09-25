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

#include "net/HostScheduler.h"

#include <QDateTime>
#include <QDebug>
#include <algorithm>
#include <utility>

#include "BuildConfig.h"

namespace Net {

namespace {

QString hostOf(const QString& urlString)
{
    return QUrl(urlString).host().toLower();
}

/*! Exact host name to policy bucket.
 *
 *  Built from the BuildConfig endpoint constants wherever one exists so that a configured
 *  endpoint and its scheduling policy cannot drift apart.
 */
const QHash<QString, HostClass>& exactHostTable()
{
    static const QHash<QString, HostClass> table = [] {
        QHash<QString, HostClass> hosts;
        auto add = [&hosts](const QString& host, HostClass hostClass) {
            if (!host.isEmpty())
                hosts.insert(host.toLower(), hostClass);
        };

        add(hostOf(BuildConfig.DEFAULT_RESOURCE_BASE), HostClass::MinecraftResources);
        add(hostOf(BuildConfig.LIBRARY_BASE), HostClass::MinecraftLibraries);
        // The edge endpoint redirects file bytes to these CDN hosts; they share the same file policy.
        const QStringList flameCdnHosts = {
            BuildConfig.FLAME_DOWNLOAD_HOST,
            QStringLiteral("mediafilez.forgecdn.net"),
            QStringLiteral("media.forgecdn.net"),
        };
        for (const auto& host : flameCdnHosts)
            add(host, HostClass::FlameCdn);
        add(BuildConfig.MODRINTH_DOWNLOAD_HOST, HostClass::ModrinthCdn);
        add(hostOf(BuildConfig.FLAME_BASE_URL), HostClass::FlameApi);
        add(hostOf(BuildConfig.MODRINTH_PROD_URL), HostClass::ModrinthApi);
        add(hostOf(BuildConfig.MODRINTH_STAGING_URL), HostClass::ModrinthApi);
        add(hostOf(BuildConfig.ATL_DOWNLOAD_SERVER_URL), HostClass::AtlCdn);

        // Code hosting mirrors that mrpack files are allowed to point at.
        for (const auto& host : BuildConfig.MODRINTH_MRPACK_HOSTS) {
            if (host.compare(BuildConfig.MODRINTH_DOWNLOAD_HOST, Qt::CaseInsensitive) != 0)
                add(host, HostClass::CodeHosting);
        }

        // Forge and NeoForge Maven repositories.
        add(hostOf(BuildConfig.LEGACY_FMLLIBS_BASE_URL), HostClass::ForgeMaven);
        add(QStringLiteral("maven.minecraftforge.net"), HostClass::ForgeMaven);
        add(QStringLiteral("files.minecraftforge.net"), HostClass::ForgeMaven);
        add(QStringLiteral("maven.neoforged.net"), HostClass::ForgeMaven);

        // Feed The Beast blocks parallel requests, so it is pinned to one.
        add(hostOf(BuildConfig.FTB_API_BASE_URL), HostClass::Ftb);
        add(hostOf(BuildConfig.LEGACY_FTB_CDN_BASE_URL), HostClass::Ftb);

        // The launcher's own metadata service is latency critical, so it is listed after the
        // code hosting entries: it keeps its own policy even when it lives on such a domain.
        add(hostOf(BuildConfig.META_URL), HostClass::LauncherMeta);

        return hosts;
    }();
    return table;
}

bool matchesDomain(const QString& host, const QString& domain)
{
    if (host == domain)
        return true;
    if (host.size() <= domain.size())
        return false;
    return host.endsWith(domain) && host.at(host.size() - domain.size() - 1) == QLatin1Char('.');
}

const QStringList& codeHostingDomains()
{
    static const QStringList domains = {
        QStringLiteral("github.com"), QStringLiteral("githubusercontent.com"), QStringLiteral("github.io"),
        QStringLiteral("gitlab.com"), QStringLiteral("gitlab.io"),
    };
    return domains;
}

const QStringList& ftbDomains()
{
    static const QStringList domains = { QStringLiteral("feed-the-beast.com") };
    return domains;
}

}  // namespace

HostScheduler::HostScheduler(QObject* parent) : QObject(parent), m_clock([] { return QDateTime::currentMSecsSinceEpoch(); })
{
    m_wakeup.setSingleShot(true);
    m_wakeup.setTimerType(Qt::CoarseTimer);
    connect(&m_wakeup, &QTimer::timeout, this, &HostScheduler::onWakeup);
}

HostScheduler::~HostScheduler() = default;

HostScheduler* HostScheduler::global()
{
    // Intentionally leaked. NetJobs are owned by many different objects and can still be handing
    // permits back while the application is being torn down, so this must outlive all of them and
    // must not be destroyed while static destructors elsewhere can still reach it.
    static HostScheduler* instance = new HostScheduler();
    return instance;
}

QString HostScheduler::hostKey(const QUrl& url)
{
    const QString host = url.host().toLower();
    if (!host.isEmpty())
        return host;
    // Host-less URLs (local files, data URLs) share one bucket rather than bypassing admission.
    return QStringLiteral("<no-host>");
}

HostClass HostScheduler::classify(const QUrl& url)
{
    const QString host = url.host().toLower();
    if (host.isEmpty())
        return HostClass::Unknown;

    const auto& table = exactHostTable();
    const auto exact = table.constFind(host);
    if (exact != table.constEnd())
        return *exact;

    for (const auto& domain : ftbDomains()) {
        if (matchesDomain(host, domain))
            return HostClass::Ftb;
    }
    for (const auto& domain : codeHostingDomains()) {
        if (matchesDomain(host, domain))
            return HostClass::CodeHosting;
    }

    return HostClass::Unknown;
}

int HostScheduler::hardCeiling(HostClass hostClass)
{
    switch (hostClass) {
        case HostClass::MinecraftResources:
            return 16;
        case HostClass::MinecraftLibraries:
            return 12;
        case HostClass::FlameCdn:
        case HostClass::ModrinthCdn:
            // Measured against a real 175 file, 255 MB modpack: 6 at a time gave 1.3-1.7 MB/s,
            // 24 gave 2.6 MB/s, and 48 was both slower and started losing files. These CDNs
            // serve far more than the 8 they were given; 16 sits inside the bulk cap and leaves
            // room for other hosts in the same install.
            return 16;
        case HostClass::FlameApi:
        case HostClass::ModrinthApi:
        case HostClass::LauncherMeta:
        case HostClass::AtlCdn:
        case HostClass::CodeHosting:
        case HostClass::ForgeMaven:
            return 4;
        case HostClass::Ftb:
            return 1;
        case HostClass::Unknown:
            return UnknownHostCeiling;
    }
    return UnknownHostCeiling;
}

bool HostScheduler::isPinned(HostClass hostClass)
{
    return hostClass == HostClass::Ftb;
}

bool HostScheduler::isLatencyCritical(HostClass hostClass)
{
    switch (hostClass) {
        case HostClass::FlameApi:
        case HostClass::ModrinthApi:
        case HostClass::LauncherMeta:
            return true;
        default:
            return false;
    }
}

void HostScheduler::setNormalPerHostLevel(int level)
{
    // Above the default level every provider is already at its own hard ceiling, so there is
    // nothing higher to ask for.
    const int clamped = qBound(1, level, DefaultNormalLevel);
    if (clamped == m_normal_level)
        return;
    m_normal_level = clamped;
    // Existing hosts keep their adaptive level; effectiveLimit() re-clamps it against the new
    // ceiling, so lowering the setting takes effect immediately and raising it only allows the
    // usual ramp to continue.
    emit capacityAvailable();
}

int HostScheduler::scaledCeiling(HostClass hostClass) const
{
    const int hard = hardCeiling(hostClass);
    if (isPinned(hostClass))
        return 1;
    // Ceilings are quoted at the default normal level of 6 and scale with the user's setting,
    // rounding up so that lowering the setting never silently disables a host entirely.
    const int scaled = (hard * m_normal_level + DefaultNormalLevel - 1) / DefaultNormalLevel;
    return qBound(1, scaled, hard);
}

int HostScheduler::effectiveLimit(const HostState& state) const
{
    if (isPinned(state.hostClass))
        return 1;
    return qBound(1, state.limit, scaledCeiling(state.hostClass));
}

HostScheduler::HostState& HostScheduler::stateFor(const QString& key, HostClass hostClass)
{
    auto it = m_hosts.find(key);
    if (it != m_hosts.end())
        return *it;

    HostState fresh;
    fresh.hostClass = hostClass;
    fresh.limit = isPinned(hostClass) ? 1 : qMin(ColdStartLevel, scaledCeiling(hostClass));
    return *m_hosts.insert(key, fresh);
}

int HostScheduler::ceilingFor(const QUrl& url) const
{
    return scaledCeiling(classify(url));
}

int HostScheduler::limitFor(const QUrl& url) const
{
    const auto it = m_hosts.constFind(hostKey(url));
    if (it == m_hosts.constEnd()) {
        const HostClass hostClass = classify(url);
        return isPinned(hostClass) ? 1 : qMin(ColdStartLevel, scaledCeiling(hostClass));
    }
    return effectiveLimit(*it);
}

int HostScheduler::inFlightFor(const QUrl& url) const
{
    const auto it = m_hosts.constFind(hostKey(url));
    return it == m_hosts.constEnd() ? 0 : it->inFlight;
}

qint64 HostScheduler::cooldownRemainingFor(const QUrl& url) const
{
    const auto it = m_hosts.constFind(hostKey(url));
    if (it == m_hosts.constEnd())
        return 0;
    return qMax<qint64>(0, it->cooldownUntil - m_clock());
}

HostScheduler::Permit HostScheduler::tryAcquire(const QUrl& url, bool latencyCritical)
{
    const HostClass hostClass = classify(url);
    const bool api = latencyCritical || isLatencyCritical(hostClass);
    const QString key = hostKey(url);

    // Every refusal is counted against the host that was asked for, whichever limit turned it
    // away, so a queue starved by the global cap is distinguishable from one the host itself
    // is throttling.
    if (m_global_in_flight >= GlobalCap) {
        m_stats[key].denied++;
        return InvalidPermit;
    }
    if (!api && m_bulk_in_flight >= BulkCap) {
        m_stats[key].denied++;
        return InvalidPermit;
    }

    HostState& state = stateFor(key, hostClass);

    if (state.cooldownUntil > m_clock()) {
        m_stats[key].denied++;
        return InvalidPermit;
    }
    if (state.inFlight >= effectiveLimit(state)) {
        m_stats[key].denied++;
        return InvalidPermit;
    }

    state.inFlight++;
    m_global_in_flight++;
    if (!api)
        m_bulk_in_flight++;

    const qint64 now = m_clock();
    if (m_stats_granted == 0)
        m_stats_started_at = now;
    auto& grantStats = m_stats[key];
    grantStats.granted++;
    // Separates "the limit held us back" from "the job never asked for more": if peak in flight
    // stays well under the permitted limit, admission was never the constraint.
    grantStats.maxInFlight = std::max(grantStats.maxInFlight, state.inFlight);
    grantStats.maxLimit = std::max(grantStats.maxLimit, effectiveLimit(state));
    m_stats_granted++;

    const Permit permit = ++m_next_permit;
    m_permits.insert(permit, PermitState{ key, api, now });
    return permit;
}

void HostScheduler::release(Permit permit, HostOutcome outcome)
{
    const auto permitIt = m_permits.find(permit);
    if (permitIt == m_permits.end())
        return;  // unknown or already released; never double count

    const PermitState info = *permitIt;
    m_permits.erase(permitIt);

    if (m_global_in_flight > 0)
        m_global_in_flight--;
    if (!info.latencyCritical && m_bulk_in_flight > 0)
        m_bulk_in_flight--;

    auto hostIt = m_hosts.find(info.host);
    if (hostIt != m_hosts.end()) {
        if (hostIt->inFlight > 0)
            hostIt->inFlight--;
        applyOutcome(*hostIt, outcome, -1);
    }

    const qint64 serviceMs = std::max<qint64>(0, m_clock() - info.acquiredAt);
    auto& stats = m_stats[info.host];
    stats.serviceMsTotal += serviceMs;
    stats.serviceMsMax = std::max(stats.serviceMsMax, serviceMs);
    maybeReportStats();

    emit capacityAvailable();
}

void HostScheduler::maybeReportStats()
{
    // Report once the whole burst has drained, so the numbers cover a complete install rather
    // than an arbitrary moment inside one.
    if (m_global_in_flight > 0 || !m_permits.isEmpty())
        return;
    if (m_stats_granted < StatsReportThreshold) {
        resetStats();
        return;
    }
    qInfo().noquote() << statsReport();
    resetStats();
}

QString HostScheduler::statsReport() const
{
    const qint64 wallMs = std::max<qint64>(0, m_clock() - m_stats_started_at);
    QStringList lines;
    lines.append(QStringLiteral("Download report: %1 requests in %2 s")
                     .arg(m_stats_granted)
                     .arg(wallMs / 1000.0, 0, 'f', 1));

    QList<QString> hosts = m_stats.keys();
    std::sort(hosts.begin(), hosts.end(), [this](const QString& a, const QString& b) {
        return m_stats.value(a).serviceMsTotal > m_stats.value(b).serviceMsTotal;
    });
    for (const QString& host : hosts) {
        const HostStats& stats = m_stats.value(host);
        if (stats.granted == 0 && stats.denied == 0)
            continue;
        lines.append(QStringLiteral("  %1: %2 granted, %3 refused, %4 rate limited, "
                                    "peak %8 in flight of %9 allowed, "
                                    "avg %5 ms, max %6 ms, %7 s of transfer")
                         .arg(host)
                         .arg(stats.granted)
                         .arg(stats.denied)
                         .arg(stats.rateLimited)
                         .arg(stats.granted > 0 ? stats.serviceMsTotal / stats.granted : 0)
                         .arg(stats.serviceMsMax)
                         .arg(stats.serviceMsTotal / 1000.0, 0, 'f', 1)
                         .arg(stats.maxInFlight)
                         .arg(stats.maxLimit));
    }
    return lines.join(QLatin1Char('\n'));
}

void HostScheduler::resetStats()
{
    m_stats.clear();
    m_stats_granted = 0;
    m_stats_started_at = 0;
}

void HostScheduler::reportRateLimited(const QUrl& url, qint64 retryAfterSeconds)
{
    const QString key = hostKey(url);
    m_stats[key].rateLimited++;
    HostState& state = stateFor(key, classify(url));
    applyOutcome(state, HostOutcome::RateLimited, retryAfterSeconds);
}

void HostScheduler::migratePermit(Permit permit, const QUrl& url)
{
    const auto permitIt = m_permits.find(permit);
    if (permitIt == m_permits.end())
        return;

    const QString newKey = hostKey(url);
    if (permitIt->host == newKey)
        return;

    auto previousIt = m_hosts.find(permitIt->host);
    if (previousIt != m_hosts.end() && previousIt->inFlight > 0)
        previousIt->inFlight--;

    HostState& destination = stateFor(newKey, classify(url));
    destination.inFlight++;
    permitIt->host = newKey;

    // The host the request came from just lost an in-flight request, so someone may fit now.
    emit capacityAvailable();
}

void HostScheduler::applyOutcome(HostState& state, HostOutcome outcome, qint64 retryAfterSeconds)
{
    const bool pinned = isPinned(state.hostClass);

    switch (outcome) {
        case HostOutcome::Success: {
            if (pinned)
                break;  // FTB never ramps
            if (++state.cleanCompletions >= CleanCompletionsPerStep) {
                state.cleanCompletions = 0;
                state.penalties = 0;
                if (state.limit < scaledCeiling(state.hostClass))
                    state.limit++;
            }
            break;
        }
        case HostOutcome::Failure:
        case HostOutcome::Aborted:
            // Says nothing about how much traffic the host is willing to take.
            break;
        case HostOutcome::ConnectionReset: {
            state.cleanCompletions = 0;
            if (!pinned)
                state.limit = qMax(1, state.limit / 2);
            break;
        }
        case HostOutcome::RateLimited: {
            state.cleanCompletions = 0;
            if (!pinned)
                state.limit = qMax(1, state.limit / 2);

            qint64 cooldown;
            if (retryAfterSeconds >= 0) {
                cooldown = qBound<qint64>(0, retryAfterSeconds * 1000, MaxCooldownMs);
            } else {
                const int shift = qMin(state.penalties, 16);
                cooldown = qMin<qint64>(MinCooldownMs << shift, MaxCooldownMs);
            }
            state.penalties = qMin(state.penalties + 1, 16);
            state.cooldownUntil = qMax(state.cooldownUntil, m_clock() + cooldown);
            scheduleWakeup();
            break;
        }
    }
}

void HostScheduler::scheduleWakeup()
{
    const qint64 now = m_clock();
    qint64 soonest = -1;
    for (const auto& state : std::as_const(m_hosts)) {
        if (state.cooldownUntil <= now)
            continue;
        if (soonest < 0 || state.cooldownUntil < soonest)
            soonest = state.cooldownUntil;
    }
    if (soonest < 0)
        return;

    const int interval = static_cast<int>(qBound<qint64>(1, soonest - now, MaxCooldownMs));
    if (m_wakeup.isActive() && m_wakeup.remainingTime() >= 0 && m_wakeup.remainingTime() <= interval)
        return;
    m_wakeup.start(interval);
}

void HostScheduler::onWakeup()
{
    emit capacityAvailable();
    scheduleWakeup();
}

void HostScheduler::reset()
{
    m_wakeup.stop();
    m_hosts.clear();
    m_permits.clear();
    m_next_permit = 0;
    m_global_in_flight = 0;
    m_bulk_in_flight = 0;
    m_normal_level = DefaultNormalLevel;
    resetStats();
}

void HostScheduler::setClock(std::function<qint64()> clock)
{
    m_clock = clock ? std::move(clock) : std::function<qint64()>([] { return QDateTime::currentMSecsSinceEpoch(); });
}

}  // namespace Net
