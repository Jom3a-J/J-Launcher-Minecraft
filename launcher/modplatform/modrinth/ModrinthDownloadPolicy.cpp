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

#include "modplatform/modrinth/ModrinthDownloadPolicy.h"

#include <QJsonValue>
#include <cmath>

#include "net/ApiDownload.h"
#include "net/ApiHeaderProxy.h"
#include "net/ChecksumValidator.h"
#include "net/SegmentedDownload.h"

namespace Modrinth {

namespace {
/*! 2^53: the first integer a JSON number no longer carries exactly.
 *
 *  Qt hands every JSON number back as a double, and from here up neighbouring integers collapse
 *  onto the same value - 9007199254740993 reads back as 9007199254740992 - so a number at or above
 *  this is not a size we can act on. Nothing anyone ships in a modpack is remotely near it, which
 *  is exactly why a value up there means the field is junk rather than enormous.
 */
constexpr double FirstInexactJsonInteger = 9007199254740992.0;
}  // namespace

qint64 parseFileSize(const QJsonValue& value)
{
    // Only a JSON number counts. A string, a null, a bool, an array, an object or a missing field
    // are all simply "no hint" - none of them is an error, and none of them rejects the pack.
    if (!value.isDouble()) {
        return UnknownFileSize;
    }
    const double raw = value.toDouble();
    if (!std::isfinite(raw) || raw < 1.0 || raw >= FirstInexactJsonInteger || raw != std::floor(raw)) {
        return UnknownFileSize;
    }
    return static_cast<qint64>(raw);
}

bool shouldSegment(qint64 declaredSize, const QUrl& url, int segmentsSetting)
{
    if (segmentsSetting <= 1) {
        return false;
    }
    if (declaredSize < Net::SegmentedDownload::MinSegmentedSize) {
        return false;
    }
    return Net::isModrinthDownloadRequest(url);
}

Task::Ptr enqueuePackFileDownload(const NetJob::Ptr& job,
                                  const PackFileDownload& file,
                                  const Net::ModrinthDownloadMeta& meta,
                                  int segmentsSetting)
{
    if (!job) {
        return nullptr;
    }

    if (shouldSegment(file.declaredSize, file.url, segmentsSetting)) {
        // The job's own network manager and scheduler, so the segments run under the same process
        // wide host limits - and through the same QNetworkAccessManager - as everything else in
        // the pack. addNetAction() does this for ordinary requests; a coordinator added with
        // addTask() has to be handed both up front.
        auto segmented = Net::SegmentedDownload::makeApiFile(file.url, file.path, job->network(), job->scheduler(),
                                                             segmentsSetting, meta);
        segmented->addValidator(new Net::ChecksumValidator(file.hashAlgorithm, file.hash));
        job->addTask(segmented);
        return segmented;
    }

    auto download = Net::ApiDownload::makeFile(file.url, file.path, Net::Download::Option::NoOptions, meta);
    download->addValidator(new Net::ChecksumValidator(file.hashAlgorithm, file.hash));
    job->addNetAction(download);
    return download;
}

}  // namespace Modrinth
