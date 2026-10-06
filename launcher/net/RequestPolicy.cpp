// SPDX-License-Identifier: GPL-3.0-only

#include "RequestPolicy.h"

#include "net/HostScheduler.h"

#include <QCoreApplication>
#include <QHttp1Configuration>

#include <array>

namespace Net {

namespace {
/// These messages are reported by Request, so they keep its translation context.
QString tr(const char* text)
{
    return QCoreApplication::translate("Net::Request", text);
}

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
        for (const auto& credentialHeader : credentialHeaders) {
            if (lowerHeader == credentialHeader) {
                return true;
            }
        }
    }
    return false;
}

RedirectDecision checkRedirect(const QUrl& currentUrl, const QByteArray& location, bool requestHadCredentials,
                               int redirectsFollowed)
{
    if (location.isEmpty()) {
        return { {}, tr("Redirect rejected: the destination was empty.") };
    }

    QUrl redirect(QString::fromUtf8(location), QUrl::TolerantMode);
    if (!redirect.isValid()) {
        return { {}, tr("Redirect rejected: the destination was invalid.") };
    }
    redirect = currentUrl.resolved(redirect);
    if (!redirect.isValid() || redirect.scheme().isEmpty() || redirect.host().isEmpty()) {
        return { {}, tr("Redirect rejected: the destination was invalid.") };
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
        if (currentScheme == QStringLiteral("https") && redirectScheme == QStringLiteral("http"))
            return { {}, tr("Redirect rejected: HTTPS cannot be downgraded to HTTP.") };
        return { {}, tr("Redirect rejected: the scheme transition is not permitted.") };
    }

    if (requestHadCredentials && !sameOrigin(currentUrl, redirect)) {
        return { {}, tr("Redirect rejected: credentials cannot cross origins.") };
    }

    if (redirectsFollowed >= MaxRedirects) {
        return { {}, tr("Redirect rejected: too many redirects.") };
    }

    return { redirect, {} };
}

}  // namespace Net
