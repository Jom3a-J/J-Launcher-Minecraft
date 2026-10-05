// SPDX-License-Identifier: GPL-3.0-only

#include "ATLServerPack.h"

#include "server/ServerProperties.h"

#include <QCoreApplication>

namespace ATLauncher {

namespace {
/// These messages are shown by PackInstallTask, so they keep its translation context.
QString tr(const char* text)
{
    return QCoreApplication::translate("ATLauncher::PackInstallTask", text);
}
}  // namespace

QString prepareServerPack(const QList<VersionMod>& mods, const QString& stagingPath, const QString& packSafeName,
                          ModPlatform::ServerPackStaging::FileLists& clientOnlyList)
{
    for (const auto& mod : mods) {
        if (!mod.serverSeparate || !mod.server) {
            continue;
        }
        if (mod.serverUrl.isEmpty() || mod.serverFile.isEmpty()
            || mod.serverDownload == DownloadType::Unknown
            || mod.serverType == ModType::Unknown) {
            return tr("The ATLauncher pack has incomplete separate server metadata for %1.").arg(mod.name);
        }
    }

    using ServerLists = ModPlatform::ServerPackStaging::FileLists;
    if (!clientOnlyList.open(stagingPath, { ServerLists::ClientOnly })
        || !ModPlatform::ServerPackStaging::recordProvider(stagingPath, "atlauncher")) {
        return tr("Could not prepare the ATLauncher server compatibility manifest.");
    }
    const auto propertyOverrides = serverPropertyOverridesForPack(packSafeName);
    if (!propertyOverrides.isEmpty()) {
        QString propertyError;
        if (!ServerProperties::save(ModPlatform::ServerPackStaging::path(stagingPath, "server-properties.txt"),
                                    propertyOverrides, &propertyError)) {
            return tr("Could not prepare the ATLauncher server properties: %1").arg(propertyError);
        }
    }
    return {};
}

}  // namespace ATLauncher
