// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QUrl>

namespace ModPlatform {

enum class ServerSupport { Unknown, Official, Website, ClientDerived };

inline bool curseForgeFileHasServerPack(const QJsonObject& file)
{
    const auto id = file.value(QStringLiteral("serverPackFileId"));
    return (id.isDouble() && id.toInteger() > 0)
        || file.value(QStringLiteral("isServerPack")).toBool();
}

inline ServerSupport curseForgeServerSupport(const QJsonObject& mod)
{
    const auto files = mod.value(QStringLiteral("latestFiles")).toArray();
    for (const auto& value : files) {
        const auto file = value.toObject();
        if (curseForgeFileHasServerPack(file)) {
            return ServerSupport::Official;
        }
    }
    return ServerSupport::ClientDerived;
}

inline ServerSupport technicServerSupport(const QUrl& url)
{
    if (url.isEmpty() || !url.isValid()) {
        return ServerSupport::ClientDerived;
    }
    const auto scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        return ServerSupport::Website;
    }
    const auto path = url.path().toLower();
    if (path.endsWith(QStringLiteral(".zip")) || path.endsWith(QStringLiteral(".jar"))) {
        return ServerSupport::Official;
    }
    return ServerSupport::Website;
}

}  // namespace ModPlatform
