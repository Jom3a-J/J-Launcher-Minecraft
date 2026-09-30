// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <memory>

class ServerDataStore;
class ServerInstance;

/// Where a modpack server's mods came from, so they can be checked for updates later.
namespace ServerModpackTracking {

/// Records the source of every jar in a new server's mods folder, from the instance's metadata.
void importContentTracking(const QString &gameRoot,
                           const std::shared_ptr<ServerInstance> &server,
                           ServerDataStore &records);

}  // namespace ServerModpackTracking
