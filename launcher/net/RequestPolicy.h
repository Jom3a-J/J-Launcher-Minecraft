// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

/*! J Launcher's rules for how a NetRequest connects and which redirects it follows. */
namespace Net {

/*! Applies the opt-in HTTP/1 policy to the known CurseForge file CDN hosts. */
bool applyCdnHttp1TransportPolicy(QNetworkRequest& request, bool enabled);
/*! Applies the opt-in HTTP/1 policy to the exact Mojang file hosts. */
bool applyMojangHttp1TransportPolicy(QNetworkRequest& request, bool enabled);

/// True when the request carries credentials: authorization, cookies or an API key.
bool containsCredentials(const QNetworkRequest& request);

/// Redirects a request follows before it gives up.
inline constexpr int MaxRedirects = 10;

/// Where a redirect leads, or why it is not followed.
struct RedirectDecision {
    QUrl target;
    QString rejection;  //!< Empty when the redirect may be followed.
};

/*! Checks a Location header received for currentUrl. A redirect is refused when it is empty or
 *  invalid, downgrades HTTPS or changes to another scheme, takes credentials to another origin,
 *  or goes past MaxRedirects. */
RedirectDecision checkRedirect(const QUrl& currentUrl, const QByteArray& location, bool requestHadCredentials,
                               int redirectsFollowed);

}  // namespace Net
