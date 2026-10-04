// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <QUrl>

namespace ModPlatform {

enum class ServerSupport { Unknown, Official, Website, ClientDerived };

/// The badge a modpack list shows for this kind of server support; empty while it is not known.
inline QString serverSupportBadge(ServerSupport support)
{
    switch (support) {
        case ServerSupport::Official:
            return QCoreApplication::translate("ModPlatform", "Official server pack");
        case ServerSupport::Website:
            return QCoreApplication::translate("ModPlatform", "Server files on website");
        case ServerSupport::ClientDerived:
            return QCoreApplication::translate("ModPlatform", "No official server pack");
        case ServerSupport::Unknown:
            break;
    }
    return {};
}

/// What a screen reader says for a pack with that badge, such as "Name. Official server pack."
inline QString serverSupportAccessibleName(const QString& name, ServerSupport support)
{
    const QString badge = serverSupportBadge(support);
    return badge.isEmpty() ? name : QCoreApplication::translate("ModPlatform", "%1. %2.").arg(name, badge);
}

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

inline ServerSupport legacyFtbServerSupport(const QString& serverPack)
{
    return serverPack.isEmpty() ? ServerSupport::ClientDerived : ServerSupport::Official;
}

inline QString legacyFtbPackVersionFolder(QString version)
{
    return version.replace(QLatin1Char('.'), QLatin1Char('_'));
}

inline QUrl legacyFtbPackUrl(const QString& baseUrl, bool privatePack, const QString& directory,
                             const QString& version, const QString& file)
{
    const QString root = privatePack ? QStringLiteral("privatepacks") : QStringLiteral("modpacks");
    return QUrl(QStringLiteral("%1%2/%3/%4/%5")
                    .arg(baseUrl)
                    .arg(root)
                    .arg(directory)
                    .arg(legacyFtbPackVersionFolder(version))
                    .arg(file));
}

}  // namespace ModPlatform
