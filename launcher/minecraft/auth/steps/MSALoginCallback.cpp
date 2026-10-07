// SPDX-License-Identifier: GPL-3.0-only

#include "MSALoginCallback.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>

#include "BuildConfig.h"

namespace {
QString safeOAuthErrorCode(const QString& error)
{
    // OAuth defines these as stable identifiers. Do not echo arbitrary values
    // from an authentication response into an uploadable log or UI message.
    static const QStringList allowed {
        QStringLiteral("access_denied"),
        QStringLiteral("consent_required"),
        QStringLiteral("interaction_required"),
        QStringLiteral("invalid_grant"),
        QStringLiteral("invalid_request"),
        QStringLiteral("invalid_scope"),
        QStringLiteral("login_required"),
        QStringLiteral("server_error"),
        QStringLiteral("temporarily_unavailable"),
        QStringLiteral("unauthorized_client"),
        QStringLiteral("unsupported_response_type"),
    };
    return allowed.contains(error) ? error : QStringLiteral("unknown");
}
}  // namespace

QString buildLoginCallbackPage(const QString& landingUrl)
{
    // The landing page is told nothing. It receives the URL below verbatim, with
    // no query string, fragment, authorization code or token appended, because
    // the token exchange has already happened locally by this point.
    return QString(R"XXX(
    <noscript>
      <meta http-equiv="Refresh" content="0; URL=%1" />
    </noscript>
    Login Successful, redirecting...
    <script>
      window.location.replace("%1");
    </script>
    )XXX")
        .arg(landingUrl);
}

QString buildLoginFailedPage()
{
    // Redirects nowhere. The completion page means "you are signed in", and
    // this is the path where that is untrue.
    //
    // Served by the loopback server rather than fetched from the web, so it
    // carries its own styling and its own copy of the logo. It loads nothing:
    // matching the completion page matters, but not at the cost of this page
    // depending on a network that may be the reason sign-in just failed.
    return QStringLiteral(R"XXX(
<style>
  :root {
    color-scheme: light dark;
    --bg: #f4f6fb; --card: #fff; --ink: #1a2233; --muted: #5a6478; --line: #e2e7f0;
  }
  @media (prefers-color-scheme: dark) {
    :root { --bg: #101725; --card: #182133; --ink: #e8ecf5; --muted: #98a3ba; --line: #26314a; }
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; min-height: 100vh; display: flex; align-items: center;
    justify-content: center; padding: 24px; background: var(--bg); color: var(--ink);
    font: 16px/1.6 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  }
  .card {
    width: 100%; max-width: 460px; background: var(--card); border: 1px solid var(--line);
    border-radius: 18px; padding: 40px 32px 32px; text-align: center;
  }
  .logo { width: 76px; height: 76px; margin-bottom: 22px; }
  h1 { margin: 0 0 12px; font-size: 1.55rem; font-weight: 650; letter-spacing: -0.01em; }
  p { margin: 0 0 14px; color: var(--muted); }
  p.lead { color: var(--ink); font-size: 1.02rem; }
  p.last { margin-bottom: 0; }
  @media (max-width: 420px) { .card { padding: 32px 22px 26px; border-radius: 14px; } h1 { font-size: 1.35rem; } }
</style>
<div class="card">
  <svg class="logo" viewBox="0 0 256 256" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="J Launcher">
    <defs><linearGradient id="j" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#63b3ed"/><stop offset="1" stop-color="#5a67d8"/>
    </linearGradient></defs>
    <rect width="256" height="256" rx="56" fill="#162033"/>
    <path d="M72 52h112v31h-27v79c0 31-20 48-49 48-21 0-37-8-48-23l23-21c7 9 14 13 23 13 12 0 19-6 19-20V83H72z" fill="url(#j)"/>
    <path d="M175 49l10 18 20 4-15 14 3 21-18-9-19 9 4-21-16-14 21-4z" fill="#f6e05e"/>
  </svg>
  <h1>Sign-in was not completed</h1>
  <p class="lead">You can close this tab and try again in J Launcher.</p>
  <p class="last">No account was added.</p>
</div>
    )XXX");
}

void configureLoginCallbackHandler(QOAuthHttpServerReplyHandler* handler)
{
    handler->setCallbackText(buildLoginCallbackPage(BuildConfig.LOGIN_CALLBACK_URL));

    // Qt emits this while answering the browser, before it writes the body, so
    // choosing the text here decides what that same response says. A cancelled
    // or denied sign-in arrives as an "error" parameter rather than a code.
    QObject::connect(handler, &QOAuthHttpServerReplyHandler::callbackReceived, handler, [handler](const QVariantMap& values) {
        const bool failed = values.contains(QStringLiteral("error")) || !values.contains(QStringLiteral("code"));
        handler->setCallbackText(failed ? buildLoginFailedPage() : buildLoginCallbackPage(BuildConfig.LOGIN_CALLBACK_URL));
    });
}

QString describeAuthFailure(QNetworkReply* reply)
{
    QStringList parts;

    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid())
        parts << QStringLiteral("HTTP %1").arg(status.toInt());

    parts << reply->errorString();

    // peek() rather than readAll(), so the reply buffer stays intact for the
    // base-class handler that runs after this.
    const QByteArray body = reply->peek(qMin(reply->bytesAvailable(), qint64(8192)));
    const QJsonObject error = QJsonDocument::fromJson(body).object();

    const QString errorCode = error.value("error").toString();
    if (!errorCode.isEmpty())
        parts << QStringLiteral("error=%1").arg(safeOAuthErrorCode(errorCode));

    QStringList codes;
    const QJsonArray errorCodes = error.value("error_codes").toArray();
    for (const QJsonValue& code : errorCodes) {
        if (code.isDouble()) {
            codes << QString::number(static_cast<qint64>(code.toDouble()));
        }
    }
    if (!codes.isEmpty())
        parts << QStringLiteral("error_codes=%1").arg(codes.join(QLatin1Char(',')));

    return parts.join(QStringLiteral("; "));
}

QString signInErrorMessage(const QString& oauthError)
{
    const QString safeError = safeOAuthErrorCode(oauthError);
    qWarning() << "Microsoft sign-in failed with OAuth error" << safeError;
    return QCoreApplication::translate("MSAStep", "Microsoft sign-in failed (%1).").arg(safeError);
}
