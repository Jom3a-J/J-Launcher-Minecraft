/* SPDX-License-Identifier: GPL-3.0-only */
/*
 *  J Launcher - Minecraft Launcher
 *  Copyright (C) 2026 J Launcher Contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 */

#include "Privacy.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUrlQuery>

#include <algorithm>

namespace {

constexpr auto Redacted = "[REDACTED]";
constexpr qsizetype MinimumSanitizationInput = 64 * 1024;
constexpr qsizetype MaximumSanitizationInput = 1024 * 1024;

QString normalizedName(QString name)
{
    name = name.toLower();
    name.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    return name;
}

bool isSensitiveName(const QString& name)
{
    const QString normalized = normalizedName(name);
    const bool hasSecretSuffix = normalized.endsWith(QStringLiteral("privatekey"))
        || normalized.endsWith(QStringLiteral("signingkey"))
        || normalized.endsWith(QStringLiteral("encryptionkey"))
        || normalized.endsWith(QStringLiteral("connectionstring"))
        || normalized.endsWith(QStringLiteral("dbpass"));
    return normalized == QStringLiteral("authorization")
        || normalized == QStringLiteral("proxyauthorization")
        || normalized == QStringLiteral("cookie")
        || normalized == QStringLiteral("setcookie")
        || normalized.contains(QStringLiteral("apikey"))
        || normalized == QStringLiteral("awsaccesskeyid")
        || normalized.contains(QStringLiteral("token"))
        || normalized.contains(QStringLiteral("secret"))
        || normalized.contains(QStringLiteral("password"))
        || normalized.contains(QStringLiteral("passwd"))
        || normalized.contains(QStringLiteral("pwd"))
        || normalized.contains(QStringLiteral("credential"))
        || normalized.contains(QStringLiteral("cookie"))
        || normalized.contains(QStringLiteral("session"))
        || normalized.contains(QStringLiteral("signature"))
        || normalized == QStringLiteral("sig")
        || normalized == QStringLiteral("pass")
        || normalized == QStringLiteral("dbpass")
        || normalized == QStringLiteral("pat")
        || normalized == QStringLiteral("ghpat")
        || normalized == QStringLiteral("githubpat")
        || normalized == QStringLiteral("privatekey")
        || normalized == QStringLiteral("signingkey")
        || normalized == QStringLiteral("encryptionkey")
        || normalized == QStringLiteral("connectionstring")
        || normalized == QStringLiteral("auth")
        || normalized == QStringLiteral("xauth")
        || hasSecretSuffix;
}

bool isSensitiveStructuredName(const QString& name)
{
    const QString normalized = normalizedName(name);
    return isSensitiveName(name)
        || normalized == QStringLiteral("authorizationcode")
        || normalized == QStringLiteral("authcode")
        || normalized == QStringLiteral("oauthcode")
        || normalized == QStringLiteral("devicecode")
        || normalized == QStringLiteral("usercode")
        || normalized == QStringLiteral("deletehash")
        || normalized == QStringLiteral("nonce");
}

bool isSensitiveQueryName(const QString& name)
{
    const QString normalized = normalizedName(name);
    return isSensitiveName(name)
        || normalized == QStringLiteral("authorizationcode")
        || normalized == QStringLiteral("authcode")
        || normalized == QStringLiteral("oauthcode")
        || normalized == QStringLiteral("devicecode")
        || normalized == QStringLiteral("usercode")
        || normalized == QStringLiteral("deletehash")
        || normalized == QStringLiteral("nonce")
        || normalized == QStringLiteral("oauth")
        || normalized == QStringLiteral("oauthverifier")
        || normalized == QStringLiteral("expires")
        || normalized == QStringLiteral("code")
        || normalized == QStringLiteral("state")
        || normalized == QStringLiteral("se")
        || normalized == QStringLiteral("sp")
        || normalized.startsWith(QStringLiteral("xamz"))
        || normalized.contains(QStringLiteral("signed"));
}

bool queryNeedsConservativeRedaction(
    const QList<QPair<QString, QString>>& items)
{
    return std::any_of(items.cbegin(), items.cend(),
                       [](const auto& item) {
                           const QString normalized = normalizedName(item.first);
                           return normalized == QStringLiteral("signature")
                               || normalized == QStringLiteral("sig")
                               || normalized == QStringLiteral("se")
                               || normalized == QStringLiteral("sp")
                               || normalized.startsWith(QStringLiteral("xamz"))
                               || normalized.contains(QStringLiteral("signed"));
                       });
}

template <typename Replacement>
QString replaceMatches(const QString& input,
                       const QRegularExpression& expression,
                       Replacement replacement)
{
    QString output;
    output.reserve(input.size());
    qsizetype cursor = 0;
    auto iterator = expression.globalMatch(input);
    while (iterator.hasNext()) {
        const auto match = iterator.next();
        const qsizetype start = match.capturedStart();
        const qsizetype end = match.capturedEnd();
        if (start < cursor) {
            continue;
        }
        output += input.mid(cursor, start - cursor);
        output += replacement(match);
        cursor = end;
    }
    output += input.mid(cursor);
    return output;
}

QString replaceUserPathComponents(const QString& input)
{
    static const QRegularExpression windowsUsers(
        R"((?i)([A-Z]:[\\/]+Users[\\/]+)[^\\/]+)");
    static const QRegularExpression unixUsers(
        R"((?i)(?<![A-Za-z0-9_])((?:/home/|/Users/))[^/]+)");

    QString result = replaceMatches(
        input, windowsUsers,
        [](const QRegularExpressionMatch& match) {
            return match.captured(1) + QStringLiteral("<USER>");
        });
    result = replaceMatches(
        result, unixUsers,
        [](const QRegularExpressionMatch& match) {
            return match.captured(1) + QStringLiteral("<USER>");
        });

    static const QString home = QDir::fromNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
    static const QRegularExpression homeExpression(
        home.isEmpty()
            ? QStringLiteral("(?!x)x")
            : QRegularExpression::escape(home)
                + QStringLiteral("(?=$|[/\\\\])"),
        QRegularExpression::CaseInsensitiveOption);
    result.replace(homeExpression, QStringLiteral("<USER_HOME>"));
    return result;
}

QString truncate(const QString& text, int maxLength)
{
    if (maxLength <= 0 || text.size() <= maxLength) {
        return text;
    }

    const QString marker = QStringLiteral(" ... [truncated]");
    if (maxLength <= marker.size()) {
        return text.left(maxLength);
    }
    return text.left(maxLength - marker.size()) + marker;
}

QString redactEmbeddedUrls(const QString& input)
{
    static const QRegularExpression url(
        R"((?i)\b(?:https?|ftp)://[^\s<>"']+)");
    return replaceMatches(
        input, url,
        [](const QRegularExpressionMatch& match) {
            return Privacy::sanitizeUrl(match.captured(0), 64 * 1024);
        });
}

QString redactKeyValues(const QString& input)
{
    static const QRegularExpression keyValue(
        R"((?i)(?<![A-Za-z0-9_])(\\?["']?(?:authorization|proxy[-_]?authorization|cookie|set[-_]?cookie|x[-_]?api[-_]?key|api[-_]?key|access[-_]?token|refresh[-_]?token|id[-_]?token|client[-_]?secret|authorization[-_]?code|auth[-_]?code|oauth[-_]?code|device[-_]?code|delete[-_]?hash|token|secret|password|passwd|pwd|pass|pat|private[-_]?key|signing[-_]?key|encryption[-_]?key|connection[-_]?string|x[-_]?auth|auth|session(?:[-_]?id)?|sid|signature|sig|x[-_]?amz[-_]?signature|awsaccesskeyid|nonce)\\?["']?)(\s*[:=]\s*)(\\?"(?:\\.|[^"\\])*\\?"|\\?'(?:\\.|[^'\\])*\\?'|\[REDACTED\]|[^\s,;&}\]]+))");
    static const QRegularExpression oauthCode(
        R"((?i)(\b(?:authorization[ _-]?code|auth[ _-]?code|oauth[ _-]?code)\b\s*=\s*)(\[REDACTED\]|[^\s,;&}\]]+))");
    static const QRegularExpression longPlainCode(
        R"((?i)(\bcode\s*=\s*)([A-Za-z0-9._~+/=-]{12,}))");
    static const QRegularExpression sessionId(
        R"((?i)(\(\s*session\s+id\s+is\s+)[^)\r\n]+(\)))");
    static const QRegularExpression refreshToken(
        R"((?i)(\b(?:new\s+)?refresh\s+token\b\s*[:=]\s*)(\[REDACTED\]|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|[^\s,;&}\]]+))");

    QString result = replaceMatches(
        input, keyValue,
        [](const QRegularExpressionMatch& match) {
            return match.captured(1) + match.captured(2)
                + QString::fromLatin1(Redacted);
        });
    result = replaceMatches(
        result, oauthCode,
        [](const QRegularExpressionMatch& match) {
            return match.captured(1) + QString::fromLatin1(Redacted);
        });
    result = replaceMatches(
        result, longPlainCode,
        [](const QRegularExpressionMatch& match) {
            return match.captured(1) + QString::fromLatin1(Redacted);
        });
    result = replaceMatches(
        result, sessionId,
        [](const QRegularExpressionMatch& match) {
            return match.captured(1) + QString::fromLatin1(Redacted)
                + match.captured(2);
        });
    return replaceMatches(
        result, refreshToken,
        [](const QRegularExpressionMatch& match) {
            return match.captured(1) + QString::fromLatin1(Redacted);
        });
}

QString redactMinecraftCommands(const QString& input)
{
    static const QRegularExpression command(
        QStringLiteral("(^|[\\r\\n])([ \\t]*(?:>\\s*)?)(/(?:login|register|changepassword|changepass)\\b)([^\\r\\n]*)"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::MultilineOption);
    return replaceMatches(
        input, command,
        [](const QRegularExpressionMatch& match) {
            const QString arguments = match.captured(4).trimmed();
            return match.captured(1) + match.captured(2) + match.captured(3)
                + (arguments.isEmpty()
                       ? QString()
                       : QStringLiteral(" [arguments hidden]"));
        });
}

QString redactStructuredTokens(const QString& input)
{
    static const QRegularExpression authorization(
        R"((?i)\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]+)");
    static const QRegularExpression jwt(
        R"(\beyJ[A-Za-z0-9_-]*\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\b)");
    static const QRegularExpression awsAccessKey(
        R"(\b(?:AKIA|ASIA)[A-Z0-9]{16}\b)");

    QString result = replaceMatches(
        input, authorization,
        [](const QRegularExpressionMatch& match) {
            return match.captured(1) + QStringLiteral(" ")
                + QString::fromLatin1(Redacted);
        });
    result = replaceMatches(
        result, jwt,
        [](const QRegularExpressionMatch&) {
            return QString::fromLatin1(Redacted);
        });
    return replaceMatches(
        result, awsAccessKey,
        [](const QRegularExpressionMatch&) {
            return QString::fromLatin1(Redacted);
        });
}

QJsonValue sanitizeJsonValue(const QJsonValue& value)
{
    if (value.isObject()) {
        QJsonObject result;
        const QJsonObject object = value.toObject();
        for (auto iterator = object.constBegin(); iterator != object.constEnd();
             ++iterator) {
            if (isSensitiveStructuredName(iterator.key())) {
                result.insert(iterator.key(), QString::fromLatin1(Redacted));
            } else {
                result.insert(iterator.key(), sanitizeJsonValue(iterator.value()));
            }
        }
        return result;
    }

    if (value.isArray()) {
        QJsonArray result;
        for (const QJsonValue& item : value.toArray()) {
            result.append(sanitizeJsonValue(item));
        }
        return result;
    }

    if (value.isString()) {
        return Privacy::sanitizeText(value.toString(), 64 * 1024);
    }
    return value;
}

}  // namespace

namespace Privacy {

namespace {
QString sanitizeTextCore(const QString& text, int maxLength,
                         bool includeEmbeddedUrls)
{
    // Keep a minimum inspection window for small output limits, but honor
    // larger caller limits so late crash/console lines are not discarded.
    const qsizetype requestedInput = maxLength > 0
        ? static_cast<qsizetype>(maxLength)
        : MinimumSanitizationInput;
    const qsizetype inputLimit = std::min(
        MaximumSanitizationInput,
        std::max(MinimumSanitizationInput, requestedInput));
    const QString boundedInput = text.size() > inputLimit
        ? text.left(inputLimit)
        : text;
    QString result = redactStructuredTokens(boundedInput);
    result = redactKeyValues(result);
    result = redactMinecraftCommands(result);
    if (includeEmbeddedUrls) {
        result = redactEmbeddedUrls(result);
    }
    result = replaceUserPathComponents(result);
    return truncate(result, maxLength);
}
}  // namespace

QString sanitizePath(const QString& path, int maxLength)
{
    return truncate(replaceUserPathComponents(path), maxLength);
}

QString sanitizeText(const QString& text, int maxLength)
{
    return sanitizeTextCore(text, maxLength, true);
}

QString sanitizeCommandForDisplay(const QString& command, int maxLength)
{
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("> [command sent]");
    }

    qsizetype verbLength = 0;
    while (verbLength < trimmed.size() && !trimmed.at(verbLength).isSpace()) {
        ++verbLength;
    }
    const QString verb = sanitizeTextCore(trimmed.left(verbLength), 128, false);
    if (verb.isEmpty() || verb.contains(QStringLiteral("[REDACTED]"))) {
        return QStringLiteral("> [command sent]");
    }

    const QString suffix = verbLength < trimmed.size()
        ? QStringLiteral(" [arguments hidden]")
        : QString();
    return truncate(QStringLiteral("> ") + verb + suffix, maxLength);
}

QString sanitizeJson(const QByteArray& json, int maxLength)
{
    return sanitizeResponseBody(json, maxLength);
}

QString sanitizeJson(const QString& json, int maxLength)
{
    return sanitizeJson(json.toUtf8(), maxLength);
}

QString sanitizeResponseBody(const QByteArray& body, int maxLength)
{
    const qsizetype requestedInput = maxLength > 0
        ? static_cast<qsizetype>(maxLength)
        : MinimumSanitizationInput;
    const qsizetype parseLimit = std::min(
        MaximumSanitizationInput,
        std::max(MinimumSanitizationInput, requestedInput));
    if (body.size() > parseLimit) {
        return sanitizeText(QString::fromUtf8(body.left(parseLimit)), maxLength);
    }

    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isNull()) {
        QJsonValue value;
        if (document.isObject()) {
            value = sanitizeJsonValue(document.object());
        } else if (document.isArray()) {
            value = sanitizeJsonValue(document.array());
        }
        if (!value.isUndefined()) {
            const QByteArray compact = value.isObject()
                ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
                : QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
            return truncate(QString::fromUtf8(compact), maxLength);
        }
    }
    return sanitizeText(QString::fromUtf8(body), maxLength);
}

QString sanitizeUrl(const QUrl& url, int maxLength)
{
    if (!url.isValid() || url.isEmpty()) {
        return sanitizeTextCore(url.toString(), maxLength, false);
    }

    QUrl safe = url;
    safe.setUserName(QString());
    safe.setPassword(QString());
    safe.setFragment(QString());

    QUrlQuery query(url);
    const auto items = query.queryItems(QUrl::FullyDecoded);
    const bool redactAllValues = queryNeedsConservativeRedaction(items);
    if (!items.isEmpty()) {
        QUrlQuery safeQuery;
        for (const auto& item : items) {
            const QString value = redactAllValues || isSensitiveQueryName(item.first)
                ? QString::fromLatin1(Redacted)
                : item.second;
            safeQuery.addQueryItem(item.first, value);
        }
        safe.setQuery(safeQuery);
    } else {
        safe.setQuery(QUrlQuery());
    }

    return sanitizeTextCore(safe.toString(QUrl::FullyEncoded), maxLength, false);
}

QString sanitizeUrl(const QString& url, int maxLength)
{
    const QUrl parsed(url, QUrl::TolerantMode);
    return parsed.isValid() && !parsed.isEmpty()
        ? sanitizeUrl(parsed, maxLength)
        : sanitizeTextCore(url, maxLength, false);
}

QString sanitizeEnvironmentValue(const QString& name, const QString& value,
                                 int maxLength)
{
    if (isSensitiveName(name)) {
        return QString::fromLatin1(Redacted);
    }

    if (name.compare(QStringLiteral("PATH"), Qt::CaseInsensitive) == 0) {
        QStringList paths = value.split(QDir::listSeparator());
        for (QString& path : paths) {
            path = sanitizePath(path, 1024);
        }
        return truncate(paths.join(QDir::listSeparator()), maxLength);
    }

    return sanitizeText(value, maxLength);
}

QStringList formatEnvironmentForDiagnostics(
    const QProcessEnvironment& environment,
    const QStringList& requestedAllowlist)
{
    const QStringList allowlist = requestedAllowlist.isEmpty()
        ? QStringList{
              QStringLiteral("JAVA_HOME"),
              QStringLiteral("PATH"),
              QStringLiteral("PROCESSOR_ARCHITECTURE"),
              QStringLiteral("PROCESSOR_ARCHITEW6432"),
              QStringLiteral("OS"),
              QStringLiteral("NUMBER_OF_PROCESSORS"),
          }
        : requestedAllowlist;

    QStringList result;
    QSet<QString> included;
    for (const QString& requested : allowlist) {
        for (const QString& actual : environment.keys()) {
            if (actual.compare(requested, Qt::CaseInsensitive) != 0
                || included.contains(actual)) {
                continue;
            }
            included.insert(actual);
            result << QStringLiteral("%1=%2")
                          .arg(actual,
                               sanitizeEnvironmentValue(actual,
                                                        environment.value(actual)));
        }
    }

    const int omitted = environment.keys().size() - included.size();
    if (omitted > 0) {
        result << QStringLiteral("Additional environment variables omitted: %1")
                      .arg(omitted);
    }
    return result;
}

QStringList sanitizeHeaders(
    const QList<QPair<QByteArray, QByteArray>>& headers,
    int maxValueLength)
{
    QStringList result;
    for (const auto& header : headers) {
        const QString name = QString::fromLatin1(header.first);
        const QString value = isSensitiveName(name)
            ? QString::fromLatin1(Redacted)
            : sanitizeText(QString::fromUtf8(header.second), maxValueLength);
        result << QStringLiteral("%1: %2").arg(name, value);
    }
    return result;
}

QString formatNetworkRequest(const QNetworkRequest& request)
{
    QList<QPair<QByteArray, QByteArray>> headers;
    for (const QByteArray& name : request.rawHeaderList()) {
        headers.append({ name, request.rawHeader(name) });
    }

    const QString headerText = sanitizeHeaders(headers).join(QStringLiteral(", "));
    return QStringLiteral("URL=%1; headers={%2}")
        .arg(sanitizeUrl(request.url()), headerText);
}

}  // namespace Privacy
