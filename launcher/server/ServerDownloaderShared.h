// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QString>

/// Helpers shared by ServerDownloader's per-server-type files. Not for use elsewhere.
namespace ServerDownloaderDetail {

/// NeoForge's first 1.20.1 releases still used Forge-style artifact coordinates.
bool usesLegacyNeoForgeCoordinates(const QString &minecraftVersion, const QString &loaderVersion);
/// Whether a file's SHA-1 is expectedSha1 (hex, any case). False when expectedSha1 is empty.
bool fileMatchesSha1(const QString &path, const QByteArray &expectedSha1);
/// Whether value is a 40-digit hex SHA-1.
bool isSha1(const QByteArray &value);

}  // namespace ServerDownloaderDetail
