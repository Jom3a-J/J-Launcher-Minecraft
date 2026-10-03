// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

class NetJob;

namespace Modrinth {

/*! Adds downloads for the files in a Modrinth pack index that only the server needs (files the
 *  client does not require and the server supports) into cacheRoot. Returns an error message, or
 *  an empty string when every such file has a safe path, HTTPS addresses and a SHA-512 hash. */
QString addServerOnlyDownloads(const QString& indexPath, const QString& cacheRoot, NetJob* downloads);

}  // namespace Modrinth
