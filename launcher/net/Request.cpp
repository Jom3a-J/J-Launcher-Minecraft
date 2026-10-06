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

#include "Request.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QIODevice>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QtMath>
#include <QUrl>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>

#ifdef LAUNCHER_APPLICATION
#include "Application.h"
#include "net/ApiHeaderProxy.h"
#include "net/ChecksumValidator.h"
#include "net/MetaCacheSink.h"
#include "settings/SettingsObject.h"
#else
#include "BuildConfig.h"
#endif
#include "net/ByteArraySink.h"
#include "net/FileSink.h"
#include "net/Logging.h"
#include "tasks/Task.h"

#include "MMCTime.h"
#include "StringUtils.h"
#include "net/HostScheduler.h"
#include "logs/Privacy.h"
#include "net/NetUtils.h"
#include "net/RequestPolicy.h"

namespace Net {

namespace {
auto logCatForMethod(HttpMethod method) -> Request::LogCatFunc
{
    switch (method.value()) {
        case HttpMethod::Get:
            return taskDownloadLogC;
        case HttpMethod::Post:
            return taskUploadLogC;
        default:
            break;
    }
    return taskNetLogC;
}
}  // namespace

Request::Request() : Request(Spec{}) {}

QString Request::formatRequestForLogging(const QNetworkRequest& request)
{
    return Privacy::formatNetworkRequest(request);
}

Request::Request(const QUrl& url, Options options, const QString& name)
    : Request(Spec{ .method = HttpMethod::Get, .url = url, .data = std::monostate{}, .options = options, .name = name })
{}

Request::Request(const QUrl& url, QByteArray postData, Options options)
    : Request(Spec{ .method = HttpMethod::Post, .url = url, .data = std::move(postData), .options = options })
{}

Request::Request(const Spec& spec) : m_options(spec.options), m_url(spec.url), m_httpMethod(spec.method), m_postData(spec.data)
{
    connect(&m_retryTimer, &QTimer::timeout, this, &Request::executeTask);
    m_stallTimer.setSingleShot(true);
    m_stallTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_stallTimer, &QTimer::timeout, this, &Request::onStallTimeout);

    m_progressFlush.setSingleShot(true);
    m_progressFlush.setTimerType(Qt::CoarseTimer);
    connect(&m_progressFlush, &QTimer::timeout, this, &Request::publishProgress);

    if (spec.name.isEmpty()) {
        setObjectName(QString("BYTES:") + Privacy::sanitizeUrl(m_url));
    } else {
        setObjectName(spec.name);
    }
    m_logCat = logCatForMethod(m_httpMethod);
#ifdef LAUNCHER_APPLICATION
    if (spec.options.testFlag(Option::AddAPIHeaders)) {
        addHeaderProxy(std::make_unique<ApiHeaderProxy>());
    }
#endif
}

void Request::addValidator(Validator* v)
{
    m_sink->addValidator(v);
}

void Request::executeTask()
{
    m_cdnHttp1PolicyApplied = false;
    setStatus(tr("Requesting %1").arg(Privacy::sanitizeUrl(m_url, 80)));

    if (m_network == nullptr) {
#ifdef LAUNCHER_APPLICATION
        m_network = APPLICATION->network();
#else
        qCCritical(m_logCat) << getUid().toString() << "No network manager set for request:" << Privacy::sanitizeUrl(m_url);
        emitFailed("No network manager set for request");
        return;
#endif
    }
    if (getState() == Task::State::AbortedByUser) {
        qCWarning(m_logCat) << getUid().toString() << "Attempt to start an aborted Request:" << Privacy::sanitizeUrl(m_url);
        emitAborted();
        return;
    }

    QNetworkRequest request(m_url);
    auto result = m_sink->init(request);
    if (!result) {
        emitFailed(result.error());
        return;
    }
    switch (*result) {
        case Sink::InitType::Ok:
            break;
        case Sink::InitType::CacheHit:
            qCDebug(m_logCat) << getUid().toString() << "Request cache hit" << Privacy::sanitizeUrl(m_url);
            emitSucceeded();
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
    const auto userAgent = application ? application->getUserAgent() : BuildConfig.USER_AGENT;
#else
    const auto userAgent = BuildConfig.USER_AGENT;
#endif
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent.toUtf8());
    for (auto& headerProxy : m_headerProxies) {
        headerProxy->writeHeaders(request);
    }
    // Record this before handing the request to Qt. Redirect policy must not
    // depend on whether a backend preserves sensitive raw headers in
    // QNetworkReply::request().
    m_requestHadCredentials = containsCredentials(request);
    // Qt follows redirects itself and would send these headers along to another host. Let it
    // follow only same-origin ones; any other redirect reaches handleRedirect(), which rejects it.
    if (m_requestHadCredentials)
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    qCDebug(m_logCat) << getUid().toString() << "Running"
                    << formatRequestForLogging(request);

#if defined(LAUNCHER_APPLICATION)
    if (application)
        request.setTransferTimeout(application->settings()->get("RequestTimeout").toInt() * 1000);
    else
        request.setTransferTimeout();
#else
    request.setTransferTimeout();
#endif

    m_lastProgressTime = std::chrono::steady_clock::now();
    m_lastProgressBytes = 0;
    // A retry or a redirect starts the byte counts over, so the next update must not be held
    // back by the throttle of the attempt that was replaced.
    resetProgressThrottle();
    m_stallAbortPending = false;

    auto* rep = getReply(request);
    if (rep == nullptr) {  // it failed
        return;
    }
    m_reply.reset(rep);
    if (trackTransportRedirects) {
        connect(rep, &QNetworkReply::redirected, this, [this](const QUrl& redirectedUrl) {
            if (m_url.host().compare(redirectedUrl.host(), Qt::CaseInsensitive) != 0)
                emit redirectedToNewHost(redirectedUrl);
        });
    }
    connect(rep, &QNetworkReply::uploadProgress, this, &Request::onProgress);
    connect(rep, &QNetworkReply::downloadProgress, this, &Request::onProgress);
    connect(rep, &QNetworkReply::finished, this, &Request::downloadFinished);
    connect(rep, &QNetworkReply::errorOccurred, this, &Request::downloadError);
    connect(rep, &QNetworkReply::sslErrors, this, &Request::sslErrors);
    connect(rep, &QNetworkReply::readyRead, this, &Request::downloadReadyRead);
#if defined(LAUNCHER_APPLICATION)
    m_stallTimeoutMs = application && supportsDownloadStallRetry()
        && (m_url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0
            || m_url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0)
        ? qRound(application->settings()->get("DownloadStallTimeout").toDouble() * 1000)
        : 0;
    if (m_stallTimeoutMs > 0)
        m_stallTimer.start(m_stallTimeoutMs);
#endif
}

void Request::onStallTimeout()
{
    if (!m_reply || m_state != State::Running || m_retryTimer.isActive())
        return;
    m_stallAbortPending = true;
    m_reply->abort();
}

void Request::scheduleStallRetry()
{
    ++m_stallRetryCount;
    const int delaySeconds = m_stallRetryCount == 1 ? 1 : 3;
    const QString retryMessage = QStringLiteral("Download stalled for %1 s, retrying (attempt %2/2)")
                                     .arg(QString::number(m_stallTimeoutMs / 1000.0, 'g', 3))
                                     .arg(m_stallRetryCount);
    qCWarning(m_logCat).noquote() << getUid().toString() << retryMessage << Privacy::sanitizeUrl(m_url);
    m_state = State::Running;
    resetProgressThrottle();
    m_lastProgressTime = std::chrono::steady_clock::now();
    m_lastProgressBytes = 0;
    m_retryTimer.setTimerType(Qt::PreciseTimer);
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(delaySeconds * 1000);
    m_retryTimer.start();
}

void Request::resetProgressThrottle()
{
    m_progressFlush.stop();
    m_progressClock.invalidate();
    m_pendingProgressReceived = 0;
    m_pendingProgressTotal = -1;
}

void Request::onProgress(qint64 bytesReceived, qint64 bytesTotal)
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

void Request::publishProgress()
{
    m_progressFlush.stop();
    m_progressClock.start();

    const qint64 bytesReceived = m_pendingProgressReceived;
    const qint64 bytesTotal = m_pendingProgressTotal;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - m_lastProgressTime;

    // use milliseconds for speed precision
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    auto bytesReceivedSince = bytesReceived - m_lastProgressBytes;
    auto dlSpeedBps = static_cast<double>(bytesReceivedSince) / static_cast<double>(elapsedMs.count()) * 1000;
    auto remainingTimeS = static_cast<double>(bytesTotal - bytesReceived) / dlSpeedBps;

    //: Current amount of bytes downloaded, out of the total amount of bytes in the download
    QString dlProgress = tr("%1 / %2")
                             .arg(StringUtils::humanReadableFileSize(static_cast<double>(bytesReceived)))
                             .arg(StringUtils::humanReadableFileSize(static_cast<double>(bytesTotal)));

    QString dlSpeedStr;
    if (elapsedMs.count() > 0) {
        auto strEta = bytesTotal > 0 ? Time::humanReadableDuration(remainingTimeS) : tr("unknown");
        //: Download speed, in bytes per second (remaining download time in parenthesis)
        dlSpeedStr = tr("%1 /s (%2)").arg(StringUtils::humanReadableFileSize(dlSpeedBps)).arg(strEta);
    } else {
        //: Download speed at 0 bytes per second
        dlSpeedStr = tr("0 B/s");
    }

    setDetails(dlProgress + "\n" + dlSpeedStr);

    setProgress(bytesReceived, bytesTotal);
}

void Request::downloadError(QNetworkReply::NetworkError error)
{
    m_stallTimer.stop();
    if (const int status = replyStatusCode(); status == 429 /* Too Many Requests */ || status == 503 /* Service Unavailable */) {
        // Report this the moment it is seen. With AutoRetry the task keeps running through the
        // retry delay, so waiting for it to finish would let sibling requests keep pushing.
        emit rateLimited(m_url, retryAfterSeconds());
    }

    if (error == QNetworkReply::OperationCanceledError && m_stallAbortPending) {
        m_stallAbortPending = false;
        if (m_stallRetryCount < 2) {
            scheduleStallRetry();
            return;
        }
        m_stallFailure = true;
        qCCritical(m_logCat) << getUid().toString() << "Download stalled after 2 retries"
                           << Privacy::sanitizeUrl(m_url);
        m_state = State::Failed;
    } else if (error == QNetworkReply::OperationCanceledError) {
        qCCritical(m_logCat) << getUid().toString() << "Aborted"
                           << Privacy::sanitizeUrl(m_url);
        m_state = State::Failed;
    } else if (replyStatusCode() == 429 /* HTTP Too Many Requests*/ && m_options.testFlag(Option::AutoRetry)) {
        qCDebug(m_logCat) << getUid().toString() << "Rate Limited!";
        auto delay = static_cast<int64_t>(10 * std::pow(2, m_retryCount));
        if (m_reply->hasRawHeader("Retry-After")) {
            const auto parsedDelay = Net::parseRetryAfterDelay(
                m_reply->rawHeader("Retry-After"), QDateTime::currentDateTimeUtc());
            if (parsedDelay) {
                delay = *parsedDelay;
            }
        }
        handleAutoRetry(delay);
    } else {
        if (m_options.testFlag(Option::AcceptLocalFiles)) {
            if (m_sink->hasLocalData()) {
                m_state = State::Succeeded;
                return;
            }
        }
        // error happened during download.
        qCCritical(m_logCat) << getUid().toString() << "Failed"
                           << Privacy::sanitizeUrl(m_url) << "with error"
                           << error;
        if (m_reply)
            qCCritical(m_logCat) << getUid().toString() << "HTTP status:"
                               << replyStatusCode()
                               << Privacy::sanitizeText(errorString());
        if (m_errorResponse.size() > 0)
            qCCritical(m_logCat) << getUid().toString()
                               << "Sanitized response excerpt:"
                               << Privacy::sanitizeResponseBody(m_errorResponse);
        m_state = State::Failed;
    }
}

void Request::sslErrors(const QList<QSslError>& errors)
{
    int i = 1;
    for (auto error : errors) {
        qCCritical(m_logCat).nospace()
            << getUid().toString() << " Request "
            << Privacy::sanitizeUrl(m_url) << " SSL Error #" << i << ": "
            << Privacy::sanitizeText(error.errorString());
        auto cert = error.certificate();
        qCCritical(m_logCat) << getUid().toString()
                           << "Certificate in question:\n"
                           << Privacy::sanitizeText(cert.toText(), 4096);
        i++;
    }
}

auto Request::handleRedirect() -> bool
{
    if (!m_reply->hasRawHeader("Location")) {
        return false;
    }

    const QUrl currentUrl = m_reply->url().isValid() ? m_reply->url() : m_url;
    const auto decision = checkRedirect(currentUrl, m_reply->rawHeader("Location"), m_requestHadCredentials, m_redirectCount);
    if (!decision.rejection.isEmpty()) {
        m_state = State::Failed;
        m_redirectRejected = true;
        m_failReason = decision.rejection;
        qCWarning(m_logCat) << getUid().toString() << m_failReason;
        return false;
    }
    const QUrl& redirect = decision.target;

    const bool crossHost = redirect.host().compare(currentUrl.host(), Qt::CaseInsensitive) != 0;

    m_redirectCount++;
    m_url = redirect;
    qCDebug(m_logCat) << getUid().toString() << "Following redirect to"
                    << Privacy::sanitizeUrl(m_url);
    if (crossHost) {
        // The transfer is about to move to a different host; admission control has to follow it
        // so the destination's limit is not bypassed by redirected traffic.
        emit redirectedToNewHost(m_url);
    }
    executeTask();

    return true;
}

void Request::handleAutoRetry(int64_t delay)
{
    m_retryCount++;
    if (delay > 60 || m_retryCount > 4) {
        /* 1 minute is too long to wait for retry, fail for now */
        m_state = State::Failed;
        auto retryAfter = QDateTime::currentDateTime().addSecs(delay);
        emitFailed(tr("Request Rate Limited for %n second(s): Retry After %1", "seconds", static_cast<int>(delay))
                       .arg(retryAfter.toLocalTime().toString(QLocale::system().dateTimeFormat(QLocale::ShortFormat))));
        return;
    }
    qCDebug(m_logCat) << getUid().toString() << "Retyring Request in" << delay << "seconds";
    setStatus(tr("Rate Limited: Waiting %n second(s)", "seconds", static_cast<int>(delay)));
    m_retryTimer.setTimerType(Qt::VeryCoarseTimer);
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(static_cast<int>(delay) * 1000);
    m_retryTimer.start();
}

void Request::downloadFinished()
{
    m_stallTimer.stop();
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
        qCDebug(m_logCat) << getUid().toString() << "Request redirected:"
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
        qCDebug(m_logCat) << getUid().toString()
                        << "Request failed but we are allowed to proceed:"
                        << Privacy::sanitizeUrl(m_url);
        m_sink->abort();
        emit succeeded();
        emit finished();
        return;
    } else if (m_state == State::Failed) {
        qCDebug(m_logCat) << getUid().toString()
                        << "Request failed in previous step:"
                        << Privacy::sanitizeUrl(m_url);
        m_sink->abort();
        m_failReason = m_reply->errorString();
        emit failed(m_failReason);
        emit finished();
        return;
    } else if (m_state == State::AbortedByUser) {
        qCDebug(m_logCat) << getUid().toString()
                        << "Request aborted in previous step:"
                        << Privacy::sanitizeUrl(m_url);
        m_sink->abort();
        emit aborted();
        emit finished();
        return;
    }

    // make sure we got all the remaining data, if any
    auto data = m_reply->readAll();
    if (!data.isEmpty()) {
        qCDebug(m_logCat) << getUid().toString() << "Writing extra" << data.size() << "bytes";
        auto result = m_sink->write(data);
        if (!result) {
            qCDebug(m_logCat) << getUid().toString() << "Request failed to write:" << Privacy::sanitizeUrl(m_url);
            m_sink->abort();
            emitFailed(result.error());
            return;
        }
    }

    // otherwise, finalize the whole graph
    auto result = m_sink->finalize(*m_reply);
    if (!result) {
        qCDebug(m_logCat) << getUid().toString() << "Request failed to finalize:" << Privacy::sanitizeUrl(m_url);
        m_sink->abort();
        emitFailed(result.error());
        return;
    }

    qCDebug(m_logCat) << getUid().toString() << "Request succeeded:" << Privacy::sanitizeUrl(m_url);
    emitSucceeded();
}

void Request::downloadReadyRead()
{
    if (m_state == State::Running) {
        auto data = m_reply->readAll();
        if (!data.isEmpty() && m_stallTimer.isActive())
            m_stallTimer.start(m_stallTimeoutMs);
        auto result = m_sink->write(data);
        if (replyStatusCode() >= 400) {
            constexpr qsizetype MaxErrorResponseBytes = 64 * 1024;
            const qsizetype remaining = MaxErrorResponseBytes - m_errorResponse.size();
            if (remaining > 0) {
                m_errorResponse.append(data.left(remaining));
            }
        }
        if (!result) {
            m_state = Task::State::Failed;
            m_failReason = result.error();
            qCCritical(m_logCat) << getUid().toString() << "Failed to process response chunk:"
                                 << Privacy::sanitizeText(m_failReason);
        }
        // qDebug() << "Request" << m_url.toString() << "gained" << data.size() << "bytes";
    } else {
        qCCritical(m_logCat) << getUid().toString() << "Cannot write download data! illegal status" << m_status;
    }
}

auto Request::abort() -> bool
{
    // A request waiting to retry has no live transfer to abort, so it reports the abort itself.
    const bool waitingForRetry = m_retryTimer.isActive();
    m_retryTimer.stop();
    m_state = State::AbortedByUser;
    m_stallTimer.stop();
    m_progressFlush.stop();
    if (waitingForRetry) {
        if (m_reply) {
            disconnect(m_reply.get(), &QNetworkReply::errorOccurred, nullptr, nullptr);
            disconnect(m_reply.get(), &QNetworkReply::finished, nullptr, nullptr);
            m_reply->abort();
        }
        m_sink->abort();
        emit aborted();
        emit finished();
        return true;
    }
    if (m_reply && !m_reply->isFinished()) {
        disconnect(m_reply.get(), &QNetworkReply::errorOccurred, nullptr, nullptr);
        m_reply->abort();
    } else {
        emit aborted();
        emit finished();
    }
    return true;
}

int Request::replyStatusCode() const
{
    return m_reply ? m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : -1;
}

QNetworkReply::NetworkError Request::error() const
{
    return m_reply ? m_reply->error() : QNetworkReply::NoError;
}

qint64 Request::retryAfterSeconds() const
{
    if (!m_reply || !m_reply->hasRawHeader("Retry-After")) {
        return -1;
    }
    const auto delay = Net::parseRetryAfterDelay(m_reply->rawHeader("Retry-After"), QDateTime::currentDateTimeUtc());
    return delay ? *delay : -1;
}

QUrl Request::url() const
{
    return m_url;
}

QString Request::errorString() const
{
    return m_reply ? m_reply->errorString() : "";
}

void Request::enableAutoRetry(bool enable)
{
    if (enable) {
        m_options |= Option::AutoRetry;
    } else {
        m_options &= ~static_cast<std::uint8_t>(Option::AutoRetry);
    }
}

QNetworkReply* Request::getReply(QNetworkRequest& request)
{
    if (m_httpMethod == HttpMethod::Get) {
        Q_ASSERT(std::holds_alternative<std::monostate>(m_postData));
        return m_network->get(request);
    }
    return std::visit(
        [this, &request](const auto& data) -> QNetworkReply* {
            using T = std::remove_cvref_t<decltype(data)>;
            const auto verb = m_httpMethod.toString().toUtf8();
            if constexpr (std::is_same_v<T, QByteArray>) {
                if (m_httpMethod == HttpMethod::Post && !request.hasRawHeader("Content-Type")) {
                    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
                }
                return m_network->sendCustomRequest(request, verb, data);
            } else if constexpr (std::is_same_v<T, ByteArrayFactory>) {
                if (m_httpMethod == HttpMethod::Post && !request.hasRawHeader("Content-Type")) {
                    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
                }
                auto body = data();
                if (!body) {
                    emitFailed(body.error());
                    return nullptr;
                }
                return m_network->sendCustomRequest(request, verb, body.value());

            } else if constexpr (std::is_same_v<T, std::monostate>) {
                return m_network->sendCustomRequest(request, verb);
            } else if constexpr (std::is_same_v<T, DeviceFactory>) {
                auto payload = data();
                if (!payload) {
                    emitFailed(payload.error());
                    return nullptr;
                }
                auto device = payload.value();
                if (device != nullptr) {
                    device->setParent(this);
                }
                return m_network->sendCustomRequest(request, verb, device);
            } else if constexpr (std::is_same_v<T, MultiPartFactory>) {
                auto payload = data();
                if (!payload) {
                    emitFailed(payload.error());
                    return nullptr;
                }
                auto multiPart = payload.value();
                if (multiPart != nullptr) {
                    multiPart->setParent(this);
                }
                return m_network->sendCustomRequest(request, verb, multiPart);
            }
        },
        m_postData);
}

#ifdef LAUNCHER_APPLICATION
auto Request::makeCached(const QUrl& url, MetaEntryPtr entry, Options options) -> Ptr
{
    auto dl = Ptr(new Request(url, options, QString("CACHE:") + Privacy::sanitizeUrl(url)));
    auto* md5Node = new ChecksumValidator(QCryptographicHash::Md5);
    auto* cachedNode = new MetaCacheSink(std::move(entry), md5Node, options.testFlag(Option::MakeEternal));
    dl->m_sink.reset(cachedNode);
    return dl;
}
#endif

auto Request::makeByteArray(const QUrl& url, QByteArray postData, Options options) -> std::pair<Ptr, QByteArray*>
{
    auto dl = Ptr(new Request(url, std::move(postData), options));

    auto sink = std::make_unique<ByteArraySink>();
    auto* response = sink->output();
    dl->m_sink = std::move(sink);

    return { dl, response };
}

auto Request::makeByteArray(const QUrl& url, Options options) -> std::pair<Ptr, QByteArray*>
{
    auto dl = Ptr(new Request(url, options));

    auto sink = std::make_unique<ByteArraySink>();
    auto* response = sink->output();
    dl->m_sink = std::move(sink);

    return { dl, response };
}

auto Request::makeFile(const QUrl& url, const QString& path, Options options) -> Ptr
{
    auto dl = Ptr(new Request(url, options, QString("FILE:") + Privacy::sanitizeUrl(url)));
    dl->m_sink = std::make_unique<FileSink>(path);

    return dl;
}

auto Request::makeCustomRequest(const Spec& spec) -> Ptr
{
    return Ptr(new Request(spec));
}

}  // namespace Net
