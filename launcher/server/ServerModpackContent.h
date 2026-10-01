// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>
#include <memory>

class ServerInstance;

/*! Turning a modpack instance into server files: published server-pack folders, copying,
 *  client-only filtering, and the markers left in a prepared server. Path checks are in
 *  ServerPaths.
 *
 *  ServerModpackInstaller::prepareContent is the entry point; these are the pieces the rest
 *  of the installer also needs.
 */
namespace ServerModpackContent {

/// Folders in an instance that look like a published server pack, best first.
QStringList publishedServerPackRootChoicesInternal(const QString &instanceRoot);
bool copyDirectoryContents(const QString &sourceRoot, const QString &destinationRoot,
                           QString *error);
/// Applies the pack's server.properties overrides to a newly created server.
bool applyServerPropertyOverrides(const QString &instanceRoot,
                                  const std::shared_ptr<ServerInstance> &server,
                                  QString *error);
/// Marks a server as derived from a client pack, so its dependencies are checked on start.
bool writeDerivedServerMarker(const QString &destination, QString *error);
/// Whether a prepared folder holds anything a server can run with.
bool hasUsablePreparedContent(const QString &destination);

}  // namespace ServerModpackContent
