// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (c) 2022 flowln <flowlnlnln@gmail.com>
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *  Copyright (C) 2023 TheKodeToad <TheKodeToad@proton.me>
 *  Copyright (C) 2023 Rachel Powers <508861+Ryex@users.noreply.github.com>
 *  Copyright (c) 2023 Trial97 <alexandru.tripon97@gmail.com>
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
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "NetRequest.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHttp1Configuration>
#include <QLocale>
#include <QNetworkReply>
#include <QUrl>
#include <array>
#include <cstdint>
#include <memory>

#if defined(LAUNCHER_APPLICATION)
#include "Application.h"
#include "settings/SettingsObject.h"
#endif
#include "BuildConfig.h"

#include "MMCTime.h"
#include "StringUtils.h"
#include "net/HostScheduler.h"
#include "logs/Privacy.h"
#include "net/NetUtils.h"

namespace Net {

namespace {
constexpr int MaxRedirects = 10;

int effectivePort(const QUrl& url)
{
    if (url.port() != -1) {
        return url.port();
    }
    if (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0) {
        return 443;
    }
    if (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0) {
        return 80;
    }
    return -1;
}

bool sameOrigin(const QUrl& first, const QUrl& second)
{
    return first.scheme().compare(second.scheme(), Qt::CaseInsensitive) == 0
        && first.host().compare(second.host(), Qt::CaseInsensitive) == 0
        && effectivePort(first) == effectivePort(second);
}

bool containsCredentials(const QNetworkRequest& request)
{
    static const std::array<QByteArray, 5> credentialHeaders = {
        QByteArrayLiteral("authorization"),
        QByteArrayLiteral("proxy-authorization"),
        QByteArrayLiteral("cookie"),
        QByteArrayLiteral("set-cookie"),
        QByteArrayLiteral("x-api-key"),
    };

    for (const auto& header : request.rawHeaderList()) {
        const auto lowerHeader = header.toLower();
        for (const auto credentialHeader : credentialHeaders) {
            if (lowerHeader == credentialHeader) {
                return true;
            }
        }
    }
    return false;
}

void applyHttp1TransportSettings(QNetworkRequest& request, int connectionCount)
{
    auto http1 = request.http1Configuration();
    http1.setNumberOfConnectionsPerHost(connectionCount);
    request.setHttp1Configuration(http1);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
}
}  // namespace

bool applyCdnHttp1TransportPolicy(QNetworkRequest& request, bool enabled)
{
    if (!enabled)
        return false;

    // Host classification is an exact allowlist; do not broaden this to a forgecdn.net suffix.
    if (HostScheduler::classify(request.url()) != HostClass::FlameCdn) {
        return false;
    }

    const int connectionCount = HostScheduler::hardCeiling(HostScheduler::classify(request.url()));
    applyHttp1TransportSettings(request, connectionCount);
    return true;
}

bool applyMojangHttp1TransportPolicy(QNetworkRequest& request, bool enabled)
{
    if (!enabled)
        return false;

    const QString host = request.url().host();
    const HostClass hostClass = HostScheduler::classify(request.url());
    int connectionCount = 0;
    if (hostClass == HostClass::MinecraftResources || hostClass == HostClass::MinecraftLibraries) {
        connectionCount = HostScheduler::hardCeiling(hostClass);
    } else if (host.compare(QStringLiteral("piston-data.mojang.com"), Qt::CaseInsensitive) == 0) {
        // Piston data remains in the Unknown scheduler class and keeps its existing ceiling.
        connectionCount = HostScheduler::UnknownHostCeiling;
    } else {
        return false;
    }

    applyHttp1TransportSettings(request, connectionCount);
    return true;
}

NetRequest::NetRequest() : Task()
{
    connect(&m_retryTimer, &QTimer::timeout, this, &NetRequest::executeTask);

    m_progressFlush.setSingleShot(true);
    m_progressFlush.setTimerType(Qt::CoarseTimer);
    connect(&m_progressFlush, &QTimer::timeout, this, &NetRequest::publishProgress);
}

QString NetRequest::formatRequestForLogging(const QNetworkRequest& request)
{
    return Privacy::formatNetworkRequest(request);
}

void NetRequest::addValidator(Validator* v)
{
    m_sink->addValidator(v);
}

void NetRequest::executeTask()
{
    m_cdnHttp1PolicyApplied = false;
    setStatus(tr("Requesting %1").arg(Privacy::sanitizeUrl(m_url, 80)));

    if (getState() == Task::State::AbortedByUser) {
        qCWarning(logCat) << getUid().toString()
                           << "Attempt to start an aborted Request:"
                           << Privacy::sanitizeUrl(m_url);
        emit aborted();
        emit finished();
        return;
    }

    QNetworkRequest request(m_url);
    m_state = m_sink->init(request);
    switch (m_state) {
        case State::Succeeded:
            qCDebug(logCat) << getUid().toString() << "Request cache hit"
                            << Privacy::sanitizeUrl(m_url);
            emit succeeded();
            emit finished();
            return;
        case State::Running:
            break;
        case State::Inactive:
        case State::Failed:
            m_failReason = m_sink->failReason();
            emit failed(m_failReason);
            emit finished();
            return;
        case State::AbortedByUser:
            emit aborted();
            emit finished();
            return;
    }

    bool trackTransportRedirects = false;
#if defined(LAUNCHER_APPLICATION)
    Application* application = APPLICATION_DYN;
    const bool cdnHttp1Enabled = application
        ? application->settings()->get("CdnHttp1Connections").toBool()
        : true;
    m_cdnHttp1PolicyApplied = applyCdnHttp1TransportPolicy(request, cdnHttp1Enabled);
    const bool mojangHttp1Enabled = application
        ? application->settings()->get("MojangHttp1Connections").toBool()
        : true;
    const bool mojangHttp1PolicyApplied = applyMojangHttp1TransportPolicy(request, mojangHttp1Enabled);
    trackTransportRedirects = m_cdnHttp1PolicyApplied || mojangHttp1PolicyApplied;
#endif

#if defined(LAUNCHER_APPLICATION)
    const auto user_agent = application ? application->getUserAgent() : BuildConfig.USER_AGENT;
#else
    const auto user_agent = BuildConfig.USER_AGENT;
#endif

    request.setHeader(QNetworkRequest::UserAgentHeader, user_agent.toUtf8());
    for (auto& header_proxy : m_headerProxies) {
        header_proxy->writeHeaders(request);
    }
    // Record this before handing the request to Qt. Redirect policy must not
    // depend on whether a backend preserves sensitive raw headers in
    // QNetworkReply::request().
    m_requestHadCredentials = containsCredentials(request);
    qCDebug(logCat) << getUid().toString() << "Running"
                    << formatRequestForLogging(request);

#if defined(LAUNCHER_APPLICATION)
    if (application)
        request.setTransferTimeout(application->settings()->get("RequestTimeout").toInt() * 1000);
    else
        request.setTransferTimeout();
#else
    request.setTransferTimeout();
#endif

    m_last_progress_time = m_clock.now();
    m_last_progress_bytes = 0;
    // A retry or a redirect starts the byte counts over, so the next update must not be held
    // back by the throttle of the attempt that was replaced.
    resetProgressThrottle();

    auto rep = getReply(request);
    if (rep == nullptr)  // it failed
        return;
    m_reply.reset(rep);
    if (trackTransportRedirects) {
        connect(rep, &QNetworkReply::redirected, this, [this](const QUrl& redirectedUrl) {
            if (m_url.host().compare(redirectedUrl.host(), Qt::CaseInsensitive) != 0)
                emit redirectedToNewHost(redirectedUrl);
        });
    }
    connect(rep, &QNetworkReply::uploadProgress, this, &NetRequest::onProgress);
    connect(rep, &QNetworkReply::downloadProgress, this, &NetRequest::onProgress);
    connect(rep, &QNetworkReply::finished, this, &NetRequest::downloadFinished);
    connect(rep, &QNetworkReply::errorOccurred, this, &NetRequest::downloadError);
    connect(rep, &QNetworkReply::sslErrors, this, &NetRequest::sslErrors);
    connect(rep, &QNetworkReply::readyRead, this, &NetRequest::downloadReadyRead);
}

void NetRequest::resetProgressThrottle()
{
    m_progressFlush.stop();
    m_progressClock.invalidate();
    m_pendingProgressReceived = 0;
    m_pendingProgressTotal = -1;
}

void NetRequest::onProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    m_pendingProgressReceived = bytesReceived;
    m_pendingProgressTotal = bytesTotal;

    // The completed value is always published exactly; everything in between is coalesced to
    // ProgressIntervalMs so that a large job does not spend its time formatting progress strings.
    const bool complete = bytesTotal > 0 && bytesReceived >= bytesTotal;
    if (!complete && m_progressClock.isValid()) {
        const qint64 elapsedSincePublish = m_progressClock.elapsed();
        if (elapsedSincePublish < ProgressIntervalMs) {
            if (!m_progressFlush.isActive())
                m_progressFlush.start(static_cast<int>(ProgressIntervalMs - elapsedSincePublish));
            return;
        }
    }

    publishProgress();
}

void NetRequest::publishProgress()
{
    m_progressFlush.stop();
    m_progressClock.start();

    const qint64 bytesReceived = m_pendingProgressReceived;
    const qint64 bytesTotal = m_pendingProgressTotal;

    auto now = m_clock.now();
    auto elapsed = now - m_last_progress_time;

    // use milliseconds for speed precision
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    auto bytes_received_since = bytesReceived - m_last_progress_bytes;
    auto dl_speed_bps = (double)bytes_received_since / elapsed_ms.count() * 1000;
    auto remaining_time_s = (bytesTotal - bytesReceived) / dl_speed_bps;

    //: Current amount of bytes downloaded, out of the total amount of bytes in the download
    QString dl_progress =
        tr("%1 / %2").arg(StringUtils::humanReadableFileSize(bytesReceived)).arg(StringUtils::humanReadableFileSize(bytesTotal));

    QString dl_speed_str;
    if (elapsed_ms.count() > 0) {
        auto str_eta = bytesTotal > 0 ? Time::humanReadableDuration(remaining_time_s) : tr("unknown");
        //: Download speed, in bytes per second (remaining download time in parenthesis)
        dl_speed_str = tr("%1 /s (%2)").arg(StringUtils::humanReadableFileSize(dl_speed_bps)).arg(str_eta);
    } else {
        //: Download speed at 0 bytes per second
        dl_speed_str = tr("0 B/s");
    }

    setDetails(dl_progress + "\n" + dl_speed_str);

    setProgress(bytesReceived, bytesTotal);
}

void NetRequest::downloadError(QNetworkReply::NetworkError error)
{
    if (const int status = replyStatusCode(); status == 429 /* Too Many Requests */ || status == 503 /* Service Unavailable */) {
        // Report this the moment it is seen. With AutoRetry the task keeps running through the
        // retry delay, so waiting for it to finish would let sibling requests keep pushing.
        emit rateLimited(m_url, retryAfterSeconds());
    }

    if (error == QNetworkReply::OperationCanceledError) {
        qCCritical(logCat) << getUid().toString() << "Aborted"
                           << Privacy::sanitizeUrl(m_url);
        m_state = State::Failed;
    } else if (replyStatusCode() == 429 /* HTTP Too Many Requests*/ && m_options & Option::AutoRetry) {
        qCDebug(logCat) << getUid().toString() << "Rate Limited!";
        int64_t delay = 10 * std::pow(2, m_retryCount);
        if (m_reply->hasRawHeader("Retry-After")) {
            const auto parsedDelay = Net::parseRetryAfterDelay(
                m_reply->rawHeader("Retry-After"), QDateTime::currentDateTimeUtc());
            if (parsedDelay) {
                delay = *parsedDelay;
            }
        }
        handleAutoRetry(delay);
    } else {
        if (m_options & Option::AcceptLocalFiles) {
            if (m_sink->hasLocalData()) {
                m_state = State::Succeeded;
                return;
            }
        }
        // error happened during download.
        qCCritical(logCat) << getUid().toString() << "Failed"
                           << Privacy::sanitizeUrl(m_url) << "with error"
                           << error;
        if (m_reply)
            qCCritical(logCat) << getUid().toString() << "HTTP status:"
                               << replyStatusCode()
                               << Privacy::sanitizeText(errorString());
        if (m_errorResponse.size() > 0)
            qCCritical(logCat) << getUid().toString()
                               << "Sanitized response excerpt:"
                               << Privacy::sanitizeResponseBody(m_errorResponse);
        m_state = State::Failed;
    }
}

void NetRequest::sslErrors(const QList<QSslError>& errors)
{
    int i = 1;
    for (auto error : errors) {
        qCCritical(logCat).nospace()
            << getUid().toString() << " Request "
            << Privacy::sanitizeUrl(m_url) << " SSL Error #" << i << ": "
            << Privacy::sanitizeText(error.errorString());
        auto cert = error.certificate();
        qCCritical(logCat) << getUid().toString()
                           << "Certificate in question:\n"
                           << Privacy::sanitizeText(cert.toText(), 4096);
        i++;
    }
}

auto NetRequest::handleRedirect() -> bool
{
    if (!m_reply->hasRawHeader("Location")) {
        return false;
    }

    const QByteArray redirectBytes = m_reply->rawHeader("Location");
    if (redirectBytes.isEmpty()) {
        m_state = State::Failed;
        m_redirectRejected = true;
        m_failReason = tr("Redirect rejected: the destination was empty.");
        qCWarning(logCat) << getUid().toString() << m_failReason;
        return false;
    }

    const QUrl currentUrl = m_reply->url().isValid() ? m_reply->url() : m_url;
    QUrl redirect(QString::fromUtf8(redirectBytes), QUrl::TolerantMode);
    if (!redirect.isValid()) {
        m_state = State::Failed;
        m_redirectRejected = true;
        m_failReason = tr("Redirect rejected: the destination was invalid.");
        qCWarning(logCat) << getUid().toString() << m_failReason;
        return false;
    }
    redirect = currentUrl.resolved(redirect);
    if (!redirect.isValid() || redirect.scheme().isEmpty() || redirect.host().isEmpty()) {
        m_state = State::Failed;
        m_redirectRejected = true;
        m_failReason = tr("Redirect rejected: the destination was invalid.");
        qCWarning(logCat) << getUid().toString() << m_failReason;
        return false;
    }

    const QString currentScheme = currentUrl.scheme().toLower();
    const QString redirectScheme = redirect.scheme().toLower();
    // Match NoLessSafeRedirectPolicy's permitted HTTP/HTTPS transitions. The credential-origin
    // check below is intentionally stricter than Qt's scheme-only redirect policy.
    const bool safeSchemeTransition =
        (currentScheme == QStringLiteral("http")
         && (redirectScheme == QStringLiteral("http") || redirectScheme == QStringLiteral("https")))
        || (currentScheme == QStringLiteral("https") && redirectScheme == QStringLiteral("https"));
    if (!safeSchemeTransition) {
        m_state = State::Failed;
        m_redirectRejected = true;
        if (currentScheme == QStringLiteral("https") && redirectScheme == QStringLiteral("http"))
            m_failReason = tr("Redirect rejected: HTTPS cannot be downgraded to HTTP.");
        else
            m_failReason = tr("Redirect rejected: the scheme transition is not permitted.");
        qCWarning(logCat) << getUid().toString() << m_failReason;
        return false;
    }

    if (m_requestHadCredentials && !sameOrigin(currentUrl, redirect)) {
        m_state = State::Failed;
        m_redirectRejected = true;
        m_failReason = tr("Redirect rejected: credentials cannot cross origins.");
        qCWarning(logCat) << getUid().toString() << m_failReason;
        return false;
    }

    if (m_redirectCount >= MaxRedirects) {
        m_state = State::Failed;
        m_redirectRejected = true;
        m_failReason = tr("Redirect rejected: too many redirects.");
        qCWarning(logCat) << getUid().toString() << m_failReason;
        return false;
    }

    const bool crossHost = redirect.host().compare(currentUrl.host(), Qt::CaseInsensitive) != 0;

    m_redirectCount++;
    m_url = redirect;
    qCDebug(logCat) << getUid().toString() << "Following redirect to"
                    << Privacy::sanitizeUrl(m_url);
    if (crossHost) {
        // The transfer is about to move to a different host; admission control has to follow it
        // so the destination's limit is not bypassed by redirected traffic.
        emit redirectedToNewHost(m_url);
    }
    executeTask();

    return true;
}

void NetRequest::handleAutoRetry(int64_t delay)
{
    m_retryCount++;
    if (delay > 60 || m_retryCount > 4) {
        /* 1 minute is too long to wait for retry, fail for now */
        m_state = State::Failed;
        auto retryAfter = QDateTime::currentDateTime().addSecs(delay);
        emitFailed(tr("Request Rate Limited for %n second(s): Retry After %1", "seconds", delay)
                       .arg(retryAfter.toLocalTime().toString(QLocale::system().dateTimeFormat(QLocale::ShortFormat))));
        return;
    } else {
        qCDebug(logCat) << getUid().toString() << "Retyring Request in" << delay << "seconds";
        setStatus(tr("Rate Limited: Waiting %n second(s)", "seconds", delay));
        m_retryTimer.setTimerType(Qt::VeryCoarseTimer);
        m_retryTimer.setSingleShot(true);
        m_retryTimer.setInterval(delay * 1000);
        m_retryTimer.start();
    }
}

void NetRequest::downloadFinished()
{
    // currently waiting for retry
    if (m_retryTimer.isActive()) {
        return;
    }

    // make sure a coalesced progress update is not lost when the transfer ends
    if (m_progressFlush.isActive()) {
        publishProgress();
    }

    // handle HTTP redirection first
    if (handleRedirect()) {
        qCDebug(logCat) << getUid().toString() << "Request redirected:"
                        << Privacy::sanitizeUrl(m_url);
        return;
    }

    if (m_redirectRejected) {
        m_sink->abort();
        emit failed(m_failReason);
        emit finished();
        return;
    }

    // if the download failed before this point ...
    if (m_state == State::Succeeded)  // pretend to succeed so we continue processing :)
    {
        qCDebug(logCat) << getUid().toString()
                        << "Request failed but we are allowed to proceed:"
                        << Privacy::sanitizeUrl(m_url);
        m_sink->abort();
        emit succeeded();
        emit finished();
        return;
    } else if (m_state == State::Failed) {
        qCDebug(logCat) << getUid().toString()
                        << "Request failed in previous step:"
                        << Privacy::sanitizeUrl(m_url);
        m_sink->abort();
        m_failReason = m_reply->errorString();
        emit failed(m_failReason);
        emit finished();
        return;
    } else if (m_state == State::AbortedByUser) {
        qCDebug(logCat) << getUid().toString()
                        << "Request aborted in previous step:"
                        << Privacy::sanitizeUrl(m_url);
        m_sink->abort();
        emit aborted();
        emit finished();
        return;
    }

    // make sure we got all the remaining data, if any
    auto data = m_reply->readAll();
    if (data.size()) {
        qCDebug(logCat) << getUid().toString() << "Writing extra" << data.size() << "bytes";
        m_state = m_sink->write(data);
        if (m_state != State::Succeeded) {
            qCDebug(logCat) << getUid().toString()
                            << "Request failed to write:"
                            << Privacy::sanitizeUrl(m_url);
            m_sink->abort();
            m_failReason = m_sink->failReason();
            emit failed(m_failReason);
            emit finished();
            return;
        }
    }

    // otherwise, finalize the whole graph
    m_state = m_sink->finalize(*m_reply.get());
    if (m_state != State::Succeeded) {
        qCDebug(logCat) << getUid().toString()
                        << "Request failed to finalize:"
                        << Privacy::sanitizeUrl(m_url);
        m_sink->abort();
        m_failReason = m_sink->failReason();
        emit failed(m_failReason);
        emit finished();
        return;
    }

    qCDebug(logCat) << getUid().toString() << "Request succeeded:"
                    << Privacy::sanitizeUrl(m_url);
    emit succeeded();
    emit finished();
}

void NetRequest::downloadReadyRead()
{
    if (m_state == State::Running) {
        auto data = m_reply->readAll();
        m_state = m_sink->write(data);
        if (replyStatusCode() >= 400) {
            constexpr qsizetype MaxErrorResponseBytes = 64 * 1024;
            const qsizetype remaining = MaxErrorResponseBytes - m_errorResponse.size();
            if (remaining > 0) {
                m_errorResponse.append(data.left(remaining));
            }
        }
        if (m_state == State::Failed) {
            qCCritical(logCat) << getUid().toString()
                               << "Failed to process response chunk:"
                               << Privacy::sanitizeText(m_sink->failReason());
        }
        // qDebug() << "Request" << m_url.toString() << "gained" << data.size() << "bytes";
    } else {
        qCCritical(logCat) << getUid().toString() << "Cannot write download data! illegal status" << m_status;
    }
}

auto NetRequest::abort() -> bool
{
    m_state = State::AbortedByUser;
    m_progressFlush.stop();
    if (m_reply) {
        disconnect(m_reply.get(), &QNetworkReply::errorOccurred, nullptr, nullptr);
        m_reply->abort();
    }
    return true;
}

int NetRequest::replyStatusCode() const
{
    return m_reply ? m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : -1;
}

QNetworkReply::NetworkError NetRequest::error() const
{
    return m_reply ? m_reply->error() : QNetworkReply::NoError;
}

qint64 NetRequest::retryAfterSeconds() const
{
    if (!m_reply || !m_reply->hasRawHeader("Retry-After")) {
        return -1;
    }
    const auto delay = Net::parseRetryAfterDelay(m_reply->rawHeader("Retry-After"), QDateTime::currentDateTimeUtc());
    return delay ? *delay : -1;
}

QUrl NetRequest::url() const
{
    return m_url;
}

QString NetRequest::errorString() const
{
    return m_reply ? m_reply->errorString() : "";
}

void NetRequest::enableAutoRetry(bool enable)
{
    if (enable) {
        m_options |= Option::AutoRetry;
    } else {
        m_options &= ~static_cast<int>(Option::AutoRetry);
    }
}

}  // namespace Net
