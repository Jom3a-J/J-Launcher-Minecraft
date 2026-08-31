// SPDX-License-Identifier: GPL-3.0-only
/*
 *  J Launcher - Minecraft Launcher
 *  Copyright (C) 2026 J Launcher Contributors
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

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QUrl>
#include <QtNetworkAuth/qoauthhttpserverreplyhandler.h>
#include <QtTest>

#include "BuildConfig.h"
#include "minecraft/auth/steps/MSAStep.h"
#include "ui/dialogs/MSALoginDialog.h"

/**
 * The sign-in completion page is presentation only. These tests hold that line:
 * the launcher must send the browser to one fixed URL and tell it nothing about
 * the authentication that just happened.
 */
class LoginCallbackTest : public QObject {
    Q_OBJECT

   private slots:
    void deviceCodeQrUsesMicrosoftVerificationUrlVerbatim()
    {
        const QUrl verificationUrl(QStringLiteral("https://www.microsoft.com/link"));
        const QUrl qrUrl = deviceLoginQrUrl(verificationUrl);

        QCOMPARE(qrUrl, verificationUrl);
        QVERIFY2(!qrUrl.hasQuery(), "The QR must not construct an unsupported otc query.");
        QVERIFY2(!qrUrl.toString().contains(QStringLiteral("otc="), Qt::CaseInsensitive),
                 "The QR leaked the separately displayed one-time code into its URL.");
    }

    void usesJLauncherOwnPageAndNotAnotherProjects()
    {
        const QString configured = BuildConfig.LOGIN_CALLBACK_URL;

        QVERIFY2(!configured.isEmpty(), "No login callback URL is configured.");

        const QUrl url(configured);
        QVERIFY2(url.isValid(), qPrintable("Configured callback URL is not a valid URL: " + configured));
        QCOMPARE(url.scheme(), QStringLiteral("https"));

        // The inherited Prism destination must not survive into a production
        // build. This is the check that would have caught 0.1.0-beta shipping
        // with it.
        QVERIFY2(!configured.contains(QStringLiteral("prismlauncher"), Qt::CaseInsensitive),
                 qPrintable("The callback URL still points at Prism Launcher: " + configured));

        QCOMPARE(url.host(), QStringLiteral("jom3a-j.github.io"));
    }

    void configuredUrlCarriesNoQueryOrFragment()
    {
        const QUrl url(BuildConfig.LOGIN_CALLBACK_URL);

        // A fixed destination. Anything here would be a place for callback data
        // to end up.
        QVERIFY2(!url.hasQuery(), "The callback URL must not carry a query string.");
        QVERIFY2(!url.hasFragment(), "The callback URL must not carry a fragment.");
        QVERIFY2(url.userInfo().isEmpty(), "The callback URL must not carry user info.");
    }

    void callbackPageRedirectsOnlyToTheUrlItWasGiven()
    {
        const QString landing = QStringLiteral("https://example.invalid/done/");
        const QString page = buildLoginCallbackPage(landing);

        QVERIFY(page.contains(landing));

        // Every URL the page names must be exactly the landing URL. If a code or
        // token were ever appended, this catches it.
        QRegularExpression urlPattern(QStringLiteral("https?://[^\"'\\s>]+"));
        auto it = urlPattern.globalMatch(page);
        int found = 0;
        while (it.hasNext()) {
            const QString matched = it.next().captured(0);
            QCOMPARE(matched, landing);
            ++found;
        }
        QVERIFY2(found > 0, "The callback page names no URL at all.");
    }

    void callbackPageLeaksNoAuthenticationData()
    {
        // Values of the shape the OAuth callback actually carries.
        const QString landing = QStringLiteral("https://example.invalid/done/");
        const QString page = buildLoginCallbackPage(landing);

        for (const QString& forbidden : { QStringLiteral("code="), QStringLiteral("access_token"), QStringLiteral("refresh_token"),
                                          QStringLiteral("id_token"), QStringLiteral("client_secret"), QStringLiteral("state=") }) {
            QVERIFY2(!page.contains(forbidden, Qt::CaseInsensitive), qPrintable("The callback page mentions " + forbidden));
        }
    }

    void callbackPageDoesNotWaitOnTheLandingSite()
    {
        // Sign-in has already succeeded locally by the time this page is served.
        // It may only navigate; it must not fetch the landing site and act on a
        // response, or an unreachable site would break a completed login.
        const QString page = buildLoginCallbackPage(QStringLiteral("https://example.invalid/done/"));

        for (const QString& forbidden :
             { QStringLiteral("fetch("), QStringLiteral("XMLHttpRequest"), QStringLiteral("navigator.sendBeacon"), QStringLiteral("<img") }) {
            QVERIFY2(!page.contains(forbidden, Qt::CaseInsensitive), qPrintable("The callback page performs a request: " + forbidden));
        }
    }
    /**
     * A denied consent must not look like a success in the browser.
     *
     * Qt serves the configured callback text for any hit on the loopback path,
     * so if Microsoft redirects back with error=access_denied the browser can
     * still be told "Login Successful" and sent to the completion page, while
     * the launcher itself correctly reports a failure. The user then sees a
     * success page for a sign-in that did not happen.
     */
    void deniedConsentIsNotServedTheSuccessPage()
    {
        QString body;
        QVERIFY2(fetchCallback(QStringLiteral("error=access_denied&error_description=user+cancelled"), &body),
                 "The loopback fixture did not work; the assertions below would be meaningless.");

        QVERIFY2(!body.contains(QStringLiteral("Login Successful"), Qt::CaseInsensitive),
                 "A denied sign-in was served the success text.");
        QVERIFY2(!body.contains(BuildConfig.LOGIN_CALLBACK_URL), "A denied sign-in was redirected to the completion page.");
        QVERIFY2(body.contains(QStringLiteral("not completed"), Qt::CaseInsensitive), "A denied sign-in was not told that it failed.");
    }

    /** A callback carrying no code at all is not a success either. */
    void emptyCallbackIsNotServedTheSuccessPage()
    {
        QString body;
        QVERIFY2(fetchCallback(QString(), &body), "The loopback fixture did not work.");

        QVERIFY2(!body.contains(QStringLiteral("Login Successful"), Qt::CaseInsensitive), "An empty callback was served the success text.");
        QVERIFY2(!body.contains(BuildConfig.LOGIN_CALLBACK_URL), "An empty callback was redirected to the completion page.");
    }

    /** The path that should still work: a real authorization code. */
    void successfulCallbackStillReachesTheCompletionPage()
    {
        QString body;
        QVERIFY2(fetchCallback(QStringLiteral("code=fake-authorization-code&state=abc"), &body), "The loopback fixture did not work.");

        QVERIFY2(body.contains(BuildConfig.LOGIN_CALLBACK_URL), "A successful sign-in was not sent to the completion page.");
        QVERIFY2(!body.contains(QStringLiteral("not completed"), Qt::CaseInsensitive), "A successful sign-in was told it failed.");
    }

    /**
     * The failure page is styled to match the completion page, but it must not
     * have picked up a dependency on the network while doing so — the network
     * may be why the sign-in just failed.
     */
    void failurePageLoadsNothingExternal()
    {
        const QString page = buildLoginFailedPage();

        QRegularExpression urlPattern(QStringLiteral("https?://[^\"'\\s>]+"));
        auto it = urlPattern.globalMatch(page);
        while (it.hasNext()) {
            // The SVG namespace is an identifier, not something the browser fetches.
            QCOMPARE(it.next().captured(0), QStringLiteral("http://www.w3.org/2000/svg"));
        }

        for (const QString& forbidden :
             { QStringLiteral("<script"), QStringLiteral("<img"), QStringLiteral("@import"), QStringLiteral("<link") }) {
            QVERIFY2(!page.contains(forbidden, Qt::CaseInsensitive), qPrintable("The failure page can load something: " + forbidden));
        }

        // url(#id) points at an element in this same document - the SVG's own
        // gradient - and fetches nothing. url(anything-else) would.
        QRegularExpression cssUrl(QStringLiteral("url\\(\\s*['\"]?(?!#)"));
        QVERIFY2(!cssUrl.match(page).hasMatch(), "The failure page references an external resource through url().");
    }

   private:
    /**
     * Drive a real loopback handler, wired exactly as the launcher wires it.
     * Returns false if the handler could not be reached at all, so a broken
     * fixture reports itself rather than surfacing as a confusing assertion
     * about page content.
     */
    bool fetchCallback(const QString& query, QString* body)
    {
        // QHostAddress(...) rather than the bare QHostAddress::LocalHost enum.
        // Qt also declares QOAuthHttpServerReplyHandler(quint16, QObject*), and
        // enum-to-quint16 is a standard conversion where enum-to-QHostAddress is
        // user-defined, so the bare enum selects that overload and binds port 2.
        // Unprivileged on Windows, privileged on Linux and macOS - which is
        // exactly how this first failed.
        QOAuthHttpServerReplyHandler handler(QHostAddress(QHostAddress::LocalHost), quint16(0));
        if (!handler.isListening()) {
            qWarning() << "Could not start a loopback handler for the test.";
            return false;
        }
        configureLoginCallbackHandler(&handler);

        QNetworkAccessManager nam;
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/%2%3").arg(handler.port()).arg(query.isEmpty() ? "" : "?").arg(query));

        QNetworkReply* reply = nam.get(QNetworkRequest(url));
        QSignalSpy done(reply, &QNetworkReply::finished);
        if (!done.wait(5000)) {
            qWarning() << "The loopback handler never answered.";
            reply->deleteLater();
            return false;
        }
        *body = QString::fromUtf8(reply->readAll());
        reply->deleteLater();
        return true;
    }
};

QTEST_GUILESS_MAIN(LoginCallbackTest)

#include "LoginCallback_test.moc"
