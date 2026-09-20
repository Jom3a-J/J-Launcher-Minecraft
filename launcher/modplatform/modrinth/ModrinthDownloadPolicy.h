// SPDX-License-Identifier: GPL-3.0-only
/*
 *  J Launcher - Minecraft Launcher
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
 */

#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>
#include <QUrl>

#include "net/NetJob.h"
#include "tasks/Task.h"

class QJsonValue;

namespace Net {
struct ModrinthDownloadMeta;
}

/*! How one file of a Modrinth pack is fetched.
 *
 *  Kept out of ModrinthCreationTask so the decision can be tested on its own: the task needs a
 *  parent widget, a staging directory and a parsed manifest before it will do anything, and none
 *  of that has a bearing on whether a given file is worth splitting.
 */
namespace Modrinth {

/// What parseFileSize() returns when the index did not declare a size we can act on.
constexpr qint64 UnknownFileSize = -1;

/*! Reads the optional mrpack "fileSize" field.
 *
 *  The field is a hint and nothing more, so anything that is not an exact positive byte count -
 *  missing, the wrong JSON type, zero, negative, fractional, or large enough that a JSON double
 *  no longer counts exactly - yields UnknownFileSize and the file is fetched the ordinary way.
 *  An otherwise valid pack is never rejected over it.
 */
qint64 parseFileSize(const QJsonValue& value);

/*! Whether one pack file is worth splitting into concurrent range requests.
 *
 *  All three conditions have to hold: the pack has to declare a size at or above
 *  Net::SegmentedDownload::MinSegmentedSize, the URL has to be the official Modrinth download
 *  host over HTTPS, and the user's segment setting has to allow more than one request. A mirror
 *  may be any file server at all, and the discovery request would be pure overhead there.
 */
bool shouldSegment(qint64 declaredSize, const QUrl& url, int segmentsSetting);

/*! Everything enqueuePackFileDownload() needs to know about one pack file. */
struct PackFileDownload {
    QUrl url;
    QString path;
    qint64 declaredSize = UnknownFileSize;
    QCryptographicHash::Algorithm hashAlgorithm = QCryptographicHash::Sha512;
    QByteArray hash;
};

/*! Adds the download for one pack file to \a job and returns it.
 *
 *  The first URL and every fallback URL go through here, so a replacement is chosen by the same
 *  policy and carries the same checksum, metadata and destination as the attempt it replaces.
 *  Segmented coordinators are added with NetJob::addTask() (they take no permit of their own and
 *  count as one unit of the job); ordinary requests keep going through addNetAction(), which is
 *  what configures their network manager.
 *
 *  Returns a null pointer only when \a job is null.
 */
Task::Ptr enqueuePackFileDownload(const NetJob::Ptr& job,
                                  const PackFileDownload& file,
                                  const Net::ModrinthDownloadMeta& meta,
                                  int segmentsSetting);

}  // namespace Modrinth
