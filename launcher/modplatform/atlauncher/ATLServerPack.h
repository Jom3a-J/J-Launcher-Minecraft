// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QList>
#include <QString>

#include "modplatform/ServerPackStaging.h"
#include "modplatform/atlauncher/ATLPackManifest.h"

/*! J Launcher's server-pack support for ATLauncher modpack installs. */
namespace ATLauncher {

/*! Prepares the staging folder for a paired server: checks the pack's separate server metadata,
 *  records the provider, opens the client-only list in clientOnlyList and writes the pack's
 *  server.properties overrides. Returns an error message, or an empty string on success. */
QString prepareServerPack(const QList<VersionMod>& mods, const QString& stagingPath, const QString& packSafeName,
                          ModPlatform::ServerPackStaging::FileLists& clientOnlyList);

}  // namespace ATLauncher
