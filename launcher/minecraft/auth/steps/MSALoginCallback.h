// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QtNetworkAuth/qoauthhttpserverreplyhandler.h>

class QNetworkReply;

/**
 * The page served on the loopback callback once Microsoft redirects back to us.
 * It sends the browser on to a fixed landing URL and nothing else.
 *
 * Split out of MSAStep's constructor so the isolation rules can be tested: the
 * landing URL must be used exactly as given, with no part of the OAuth callback
 * - no query, no fragment, no authorization code - appended to it.
 */
QString buildLoginCallbackPage(const QString& landingUrl);

/**
 * The page served when Microsoft sends us back an error instead of a code,
 * which is what a cancelled or denied sign-in looks like. It says so and
 * redirects nowhere.
 */
QString buildLoginFailedPage();

/**
 * Wire a loopback handler so the browser is told the truth.
 *
 * Qt serves one fixed callback text for every hit on the loopback path, so
 * without this a denied sign-in is answered with "Login Successful" and sent to
 * the completion page, while the launcher reports the failure. Choose the page
 * from the callback's own query instead.
 */
void configureLoginCallbackHandler(QOAuthHttpServerReplyHandler* handler);

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
QString describeAuthFailure(QNetworkReply* reply);

/**
 * Logs a failed sign-in and returns the message to show for it.
 *
 * The provider's error description may contain account-correlatable
 * diagnostics, so both the log and the uploadable UI error carry only the
 * stable OAuth identifier.
 */
QString signInErrorMessage(const QString& oauthError);
