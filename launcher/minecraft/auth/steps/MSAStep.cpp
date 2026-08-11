// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
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

#include "MSAStep.h"

#include <QAbstractOAuth2>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QOAuthHttpServerReplyHandler>
#include <QOAuthOobReplyHandler>
#include <QStringList>

#include "Application.h"
#include "BuildConfig.h"
#include "FileSystem.h"

#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

bool isSchemeHandlerRegistered()
{
#ifdef Q_OS_LINUX
    QProcess process;
    process.start("xdg-mime", { "query", "default", "x-scheme-handler/" + BuildConfig.LAUNCHER_APP_BINARY_NAME });
    process.waitForFinished();
    QString output = process.readAllStandardOutput().trimmed();

    return output.contains(APPLICATION->desktopFileName());

#elif defined(Q_OS_WIN)
    QString regPath = QString("HKEY_CURRENT_USER\\Software\\Classes\\%1").arg(BuildConfig.LAUNCHER_APP_BINARY_NAME);
    QSettings settings(regPath, QSettings::NativeFormat);

    const QString registeredRunCommand = settings.value("shell/open/command/.").toString().replace("\\", "/");
    return registeredRunCommand.contains(QCoreApplication::applicationFilePath());
#endif
    return true;
}

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

/**
 * Builds a log-safe summary of a failed authentication reply.
 *
 * Authentication endpoints answer failures with bodies that carry
 * account-correlatable diagnostics, and the launcher log is user-visible and
 * uploadable to a paste service, so the body itself is never logged. Only the
 * transport status and the OAuth error identifiers are reported: the `error`
 * field is a fixed protocol identifier, and `error_codes` are numeric AADSTS
 * classifications. The free-text `error_description`, `trace_id`, and
 * `correlation_id` are deliberately left out.
 */
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
}  // namespace

class CustomOAuthOobReplyHandler : public QOAuthOobReplyHandler {
    Q_OBJECT

   public:
    explicit CustomOAuthOobReplyHandler(QObject* parent = nullptr) : QOAuthOobReplyHandler(parent)
    {
        connect(APPLICATION, &Application::oauthReplyRecieved, this, &QOAuthOobReplyHandler::callbackReceived);
    }
    ~CustomOAuthOobReplyHandler() override
    {
        disconnect(APPLICATION, &Application::oauthReplyRecieved, this, &QOAuthOobReplyHandler::callbackReceived);
    }
    QString callback() const override { return BuildConfig.LAUNCHER_APP_BINARY_NAME + "://oauth/microsoft"; }

   protected:
    void networkReplyFinished(QNetworkReply* reply) override
    {
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "OAuth2 request failed:" << describeAuthFailure(reply);
        }

        QOAuthOobReplyHandler::networkReplyFinished(reply);
    }
};

class LoggingOAuthHttpServerReplyHandler final : public QOAuthHttpServerReplyHandler {
    Q_OBJECT

   public:
    explicit LoggingOAuthHttpServerReplyHandler(QObject* parent = nullptr) : QOAuthHttpServerReplyHandler(parent) {}

   protected:
    void networkReplyFinished(QNetworkReply* reply) override
    {
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "OAuth2 request failed:" << describeAuthFailure(reply);
        }

        QOAuthHttpServerReplyHandler::networkReplyFinished(reply);
    }
};

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

MSAStep::MSAStep(AccountData* data, bool silent) : AuthStep(data), m_silent(silent)
{
    m_clientId = APPLICATION->getMSAClientID();
    if (QCoreApplication::applicationFilePath().startsWith("/tmp/.mount_") || APPLICATION->isPortable() || !isSchemeHandlerRegistered())

    {
        auto replyHandler = new LoggingOAuthHttpServerReplyHandler(this);
        configureLoginCallbackHandler(replyHandler);
        m_oauth2.setReplyHandler(replyHandler);
    } else {
        m_oauth2.setReplyHandler(new CustomOAuthOobReplyHandler(this));
    }
    m_oauth2.setAuthorizationUrl(QUrl("https://login.microsoftonline.com/consumers/oauth2/v2.0/authorize"));
    m_oauth2.setAccessTokenUrl(QUrl("https://login.microsoftonline.com/consumers/oauth2/v2.0/token"));
    m_oauth2.setScope("XboxLive.SignIn XboxLive.offline_access");
    m_oauth2.setClientIdentifier(m_clientId);
    m_oauth2.setNetworkAccessManager(APPLICATION->network());

    connect(&m_oauth2, &QOAuth2AuthorizationCodeFlow::granted, this, [this] {
        m_data->msaClientID = m_oauth2.clientIdentifier();
        m_data->msaToken.issueInstant = QDateTime::currentDateTimeUtc();
        m_data->msaToken.notAfter = m_oauth2.expirationAt();
        m_data->msaToken.extra = m_oauth2.extraTokens();
        m_data->msaToken.refresh_token = m_oauth2.refreshToken();
        m_data->msaToken.token = m_oauth2.token();
        emit finished(AccountTaskState::STATE_WORKING, tr("Got MSA token"));
    });
    connect(&m_oauth2, &QOAuth2AuthorizationCodeFlow::authorizeWithBrowser, this, &MSAStep::authorizeWithBrowser);
    connect(&m_oauth2, &QOAuth2AuthorizationCodeFlow::requestFailed, this, [this, silent](const QAbstractOAuth2::Error err) {
        auto state = AccountTaskState::STATE_FAILED_HARD;
        if (m_oauth2.status() == QAbstractOAuth::Status::Granted || silent) {
            if (err == QAbstractOAuth2::Error::NetworkError) {
                state = AccountTaskState::STATE_OFFLINE;
            } else {
                state = AccountTaskState::STATE_FAILED_SOFT;
            }
        }
        auto message = tr("Microsoft user authentication failed.");
        if (silent) {
            message = tr("Failed to refresh token.");
        }
        qWarning() << message;
        emit finished(state, message);
    });
    connect(&m_oauth2, &QOAuth2AuthorizationCodeFlow::error, this,
            [this](const QString& error, const QString& errorDescription,
                   const QUrl& uri) {
                Q_UNUSED(errorDescription)
                Q_UNUSED(uri)

                // errorDescription may contain account-correlatable provider
                // diagnostics. Keep both logs and the uploadable UI error to
                // the stable OAuth identifier only.
                const QString safeError = safeOAuthErrorCode(error);
                qWarning() << "Microsoft sign-in failed with OAuth error"
                           << safeError;
                const QString message = tr("Microsoft sign-in failed (%1).")
                                            .arg(safeError);
                emit finished(AccountTaskState::STATE_FAILED_HARD, message);
            });

    connect(&m_oauth2, &QOAuth2AuthorizationCodeFlow::extraTokensChanged, this,
            [this](const QVariantMap& tokens) { m_data->msaToken.extra = tokens; });

    connect(&m_oauth2, &QOAuth2AuthorizationCodeFlow::clientIdentifierChanged, this,
            [this](const QString& clientIdentifier) { m_data->msaClientID = clientIdentifier; });
}

QString MSAStep::describe()
{
    return tr("Logging in with Microsoft account.");
}

void MSAStep::perform()
{
    if (m_silent) {
        if (m_data->msaClientID != m_clientId) {
            emit finished(AccountTaskState::STATE_DISABLED,
                          tr("Microsoft user authentication failed - client identification has changed."));
            return;
        }
        if (m_data->msaToken.refresh_token.isEmpty()) {
            emit finished(AccountTaskState::STATE_DISABLED, tr("Microsoft user authentication failed - refresh token is empty."));
            return;
        }
        m_oauth2.setRefreshToken(m_data->msaToken.refresh_token);
        m_oauth2.refreshAccessToken();
    } else {
        m_oauth2.setModifyParametersFunction(
            [](QAbstractOAuth::Stage stage, QMultiMap<QString, QVariant>* map) { map->insert("prompt", "select_account"); });

        *m_data = AccountData();
        m_data->msaClientID = m_clientId;
        m_oauth2.grant();
    }
}

#include "MSAStep.moc"
