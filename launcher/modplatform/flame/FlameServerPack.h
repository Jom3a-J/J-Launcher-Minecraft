// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <functional>

#include "modplatform/ModIndex.h"
#include "modplatform/ServerPackStaging.h"

class NetJob;
class QEventLoop;
class QObject;
class Task;

/*! J Launcher's server-pack support for CurseForge modpack installs: finding the server pack
 *  CurseForge publishes for a modpack version, downloading it beside the pack's files, and
 *  unpacking it into the server staging folder. */
namespace Flame {

/// Where a modpack version's published server pack downloads from, and how to check it.
struct ServerPackSource {
    QUrl downloadUrl;
    QString hashType;  //!< Empty when CurseForge published no supported hash.
    QString hash;
};

/// What a modpack install asks CurseForge for.
struct ServerPackRequest {
    QString modpackId;      //!< The CurseForge project.
    QString modpackFileId;  //!< The modpack version the server pack must belong to.
    QString serverPackFileId;
};

/*! Sorts one of the pack's files into the server lists: client-only files are listed as such, and
 *  an installed file that is not client-only is listed by the side CurseForge says it runs on. */
void addToServerLists(ModPlatform::ServerPackStaging::FileLists& lists, ModPlatform::SideType side, bool installed,
                      const QString& relativePath);

/*! Checks CurseForge's metadata for a server-pack file and reads its hash into *source.
 *  Returns an error message, or an empty string when the file is the right server pack. Sets
 *  *warning when the pack has no supported hash and will only be checked structurally. */
QString readServerPackFile(const QByteArray& response, const ServerPackRequest& request, ServerPackSource* source,
                           QString* warning);

/*! Looks up the server pack's metadata and download address, waiting on loop for each answer.
 *  Returns an error message, or an empty string with *source filled in; *source stays empty when the
 *  modpack version has no server pack. Warnings go to warn. */
QString resolveServerPack(QObject* context, QEventLoop& loop, const ServerPackRequest& request, ServerPackSource* source,
                          const std::function<void(const QString&)>& warn);

/*! Adds the server pack's download to job, saving it in the server staging folder. If it fails,
 *  onFailure gets the reason at once and the rest of the job stops. */
void addServerPackDownload(NetJob* job, const ServerPackSource& source, const QString& stagingPath, QObject* context,
                           std::function<void(QString)> onFailure);

/// Checks and unpacks the downloaded server pack into the staging folder. Returns an error message, or empty.
QString extractServerPack(const QString& stagingPath);

namespace Internal {
/*! Connects terminal outcomes separately; never treats Task::finished as success. */
void connectDownloadJobCompletion(NetJob* job,
                                  QObject* context,
                                  std::function<void()> onSucceeded,
                                  std::function<void(QString)> onFailed,
                                  std::function<void()> onAborted);
/*! Fails fast on a watchedTask failure, then aborts the rest of a job before queued work refills. */
void abortDownloadJobOnTaskFailure(NetJob* job,
                                   Task* watchedTask,
                                   QObject* context,
                                   std::function<void(QString)> onFailure);
}  // namespace Internal

}  // namespace Flame
