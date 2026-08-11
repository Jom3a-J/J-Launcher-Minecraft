// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2023 Rachel Powers <508861+Ryex@users.noreply.github.com>
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
 */

#pragma once

#include "Application.h"
#include "BuildConfig.h"
#include "net/HeaderProxy.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <utility>

namespace Net {

inline bool isCurseForgeApiRequest(const QUrl& url)
{
    const QUrl apiBase(BuildConfig.FLAME_BASE_URL);
    return url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && url.host().compare(apiBase.host(), Qt::CaseInsensitive) == 0
        && url.port(443) == apiBase.port(443);
}

inline bool isModrinthApiRequest(const QUrl& url)
{
    const auto isConfiguredEndpoint = [&url](const QUrl& configuredBase) {
        return url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
            && url.host().compare(configuredBase.host(), Qt::CaseInsensitive) == 0
            && url.port(443) == configuredBase.port(443);
    };
    return isConfiguredEndpoint(QUrl(BuildConfig.MODRINTH_PROD_URL))
        || isConfiguredEndpoint(QUrl(BuildConfig.MODRINTH_STAGING_URL));
}

inline bool isModrinthDownloadRequest(const QUrl& url)
{
    return url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && url.host().compare(BuildConfig.MODRINTH_DOWNLOAD_HOST, Qt::CaseInsensitive) == 0
        && url.port(443) == 443;
}

class CurseForgeApiKeyHeaderProxy final : public HeaderProxy {
   public:
    explicit CurseForgeApiKeyHeaderProxy(QByteArray apiKey)
        : m_apiKey(std::move(apiKey))
    {}

    QList<HeaderPair> headers(const QNetworkRequest& request) const override
    {
        if (m_apiKey.isEmpty() || !isCurseForgeApiRequest(request.url())) {
            return {};
        }
        return { { .headerName = "x-api-key", .headerValue = m_apiKey } };
    }

   private:
    QByteArray m_apiKey;
};

struct ModrinthDownloadMeta {
    QString reason;
    QString gameVersion;
    QString loader;
    QString dependentOn;

    bool isEmpty() const { return reason.isEmpty(); }

    QByteArray toJson() const
    {
        QJsonObject obj;
        if (!reason.isEmpty()) {
            obj["reason"] = reason;
        }
        if (!gameVersion.isEmpty()) {
            obj["game_version"] = gameVersion;
        }
        if (!loader.isEmpty()) {
            obj["loader"] = loader;
        }
        if (!dependentOn.isEmpty()) {
            obj["dependent_on"] = dependentOn;
        }
        return QJsonDocument(obj).toJson(QJsonDocument::Compact);
    }
};

class ApiHeaderProxy : public HeaderProxy {
   public:
    ApiHeaderProxy() = default;
    explicit ApiHeaderProxy(ModrinthDownloadMeta meta) : m_meta(std::move(meta)) {}
    ~ApiHeaderProxy() override = default;

   public:
    QList<HeaderPair> headers(const QNetworkRequest& request) const override
    {
        QList<HeaderPair> hdrs;

        if (APPLICATION->capabilities() & Application::SupportsFlame && isCurseForgeApiRequest(request.url())) {
            hdrs.append({ .headerName = "x-api-key", .headerValue = APPLICATION->getFlameAPIKey().toUtf8() });
        } else if (isModrinthApiRequest(request.url())) {
            QString token = APPLICATION->getModrinthAPIToken();
            if (!token.isEmpty()) {
                hdrs.append({ .headerName = "Authorization", .headerValue = token.toUtf8() });
            }
        }

        if (isModrinthDownloadRequest(request.url()) && !m_meta.isEmpty()) {
            hdrs.append({ .headerName = "modrinth-download-meta", .headerValue = m_meta.toJson() });
        }
        return hdrs;
    };

   private:
    ModrinthDownloadMeta m_meta;
};

}  // namespace Net
