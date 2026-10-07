// SPDX-License-Identifier: GPL-3.0-only

#include "FlameServerPack.h"

#include "Application.h"
#include "Json.h"
#include "modplatform/flame/CurseForgeHash.h"
#include "modplatform/flame/FlameAPI.h"
#include "net/ChecksumValidator.h"
#include "net/NetJob.h"
#include "net/SegmentedDownload.h"
#include "settings/SettingsObject.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <utility>

namespace Flame {

namespace {
/// These messages are shown by FlameCreationTask, so they keep its translation context.
QString tr(const char* text)
{
    return QCoreApplication::translate("FlameCreationTask", text);
}

QString archivePath(const QString& stagingPath)
{
    return ModPlatform::ServerPackStaging::path(stagingPath, QStringLiteral("published-server-pack.zip"));
}
}  // namespace

void addToServerLists(ModPlatform::ServerPackStaging::FileLists& lists, ModPlatform::SideType side, bool installed,
                      const QString& relativePath)
{
    using List = ModPlatform::ServerPackStaging::FileLists;
    if (side == ModPlatform::SideType::ClientSide) {
        lists.add(List::ClientOnly, relativePath);
        return;
    }
    if (!installed)
        return;
    if (side == ModPlatform::SideType::ServerSide)
        lists.add(List::ServerOnly, relativePath);
    else if (side == ModPlatform::SideType::NoSide)
        lists.add(List::Unknown, relativePath);
    else
        lists.add(List::Include, relativePath);
}

QString readServerPackFile(const QByteArray& response, const ServerPackRequest& request, ServerPackSource* source,
                           QString* warning)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.object().value("data").isObject()) {
        return tr("Could not understand the CurseForge server-pack response.");
    }
    const QJsonObject fileObject = document.object().value("data").toObject();
    const auto addonIdResult = Json::requireInteger(fileObject, "modId");
    const auto fileIdResult = Json::requireInteger(fileObject, "id");
    if (!addonIdResult || !fileIdResult) {
        return tr("Could not understand the CurseForge server-pack metadata:\n")
            + (addonIdResult ? fileIdResult.error() : addonIdResult.error());
    }
    const qint64 addonId = *addonIdResult;
    const qint64 fileId = *fileIdResult;
    bool validAddonId = false;
    bool validFileId = false;
    const qint64 requestedAddonId = request.modpackId.toLongLong(&validAddonId);
    const qint64 requestedFileId = request.serverPackFileId.toLongLong(&validFileId);
    if (!validAddonId || !validFileId || addonId != requestedAddonId || fileId != requestedFileId) {
        return tr("CurseForge returned metadata for a different server pack.");
    }

    const QJsonValue isServerPack = fileObject.value(QStringLiteral("isServerPack"));
    if (isServerPack.isBool() && !isServerPack.toBool()) {
        return tr("The file referenced by CurseForge is not marked as a server pack.");
    }

    const QJsonValue parentFileId = fileObject.value(QStringLiteral("parentProjectFileId"));
    bool validParentId = false;
    const qint64 requestedParentId = request.modpackFileId.toLongLong(&validParentId);
    if (validParentId && parentFileId.isDouble() && parentFileId.toInteger() > 0
        && parentFileId.toInteger() != requestedParentId) {
        return tr("The CurseForge server pack belongs to a different modpack version.");
    }

    const QJsonValue hashesValue = fileObject.value(QStringLiteral("hashes"));
    if (!hashesValue.isUndefined() && !hashesValue.isArray()) {
        return tr("CurseForge returned malformed server-pack hash metadata.");
    }
    if (hashesValue.isUndefined() || hashesValue.toArray().isEmpty()) {
        *warning = tr("CurseForge did not publish a supported hash for the dedicated server pack. "
                      "The archive will be structurally validated but remains unverified.");
        return {};
    }
    for (const QJsonValue& hashValue : hashesValue.toArray()) {
        if (const auto parsedHash = Flame::parseCurseForgeHash(hashValue.toObject())) {
            source->hashType = parsedHash->algorithmName;
            source->hash = parsedHash->value;
            return {};
        }
    }
    return tr("CurseForge returned no valid supported SHA-1 or MD5 hash for the dedicated server pack.");
}

QString resolveServerPack(QObject* context, QEventLoop& loop, const ServerPackRequest& request, ServerPackSource* source,
                          const std::function<void(const QString&)>& warn)
{
    *source = {};
    if (request.modpackId.isEmpty() || request.serverPackFileId.isEmpty()) {
        warn(tr("CurseForge does not publish a dedicated server pack for this version. "
                "The launcher will derive the server content from the client pack and "
                "exclude files marked as client-only."));
        return {};
    }

    QString error;
    QString warning;
    ServerPackSource found;
    auto [job, response] = FlameAPI::get().getFile(request.modpackId, request.serverPackFileId);
    QObject::connect(job.get(), &Task::succeeded, context, [&, response]() {
        error = readServerPackFile(*response, request, &found, &warning);
        loop.quit();
    });
    QObject::connect(job.get(), &Task::failed, context, [&](const QString& reason) {
        error = tr("Could not resolve the CurseForge server pack:\n%1").arg(reason);
        loop.quit();
    });
    job->start();
    loop.exec();
    if (!warning.isEmpty()) {
        warn(warning);
    }
    if (!error.isEmpty()) {
        return error;
    }

    auto [downloadUrlJob, downloadUrlResponse] =
        FlameAPI::get().getFileDownloadUrl(request.modpackId, request.serverPackFileId);
    QObject::connect(downloadUrlJob.get(), &Task::succeeded, context, [&, downloadUrlResponse]() {
        QString parseError;
        found.downloadUrl = FlameAPI::loadFileDownloadUrl(*downloadUrlResponse, &parseError);
        if (found.downloadUrl.isEmpty()) {
            error = parseError.isEmpty() ? tr("The CurseForge server pack is not available for third-party download.")
                                         : parseError;
        }
        loop.quit();
    });
    QObject::connect(downloadUrlJob.get(), &Task::failed, context, [&](const QString& reason) {
        error = tr("Could not get the CurseForge server-pack download URL:\n%1").arg(reason);
        loop.quit();
    });
    downloadUrlJob->start();
    loop.exec();
    if (!error.isEmpty()) {
        return error;
    }
    *source = found;
    return {};
}

void addServerPackDownload(NetJob* job, const ServerPackSource& source, const QString& stagingPath, QObject* context,
                           std::function<void(QString)> onFailure)
{
    // Published server packs are the one file in a CurseForge install that is big enough for a
    // single connection to dominate the whole download, so this one is allowed to use several.
    // It falls back to an ordinary single stream download whenever the CDN will not cooperate.
    auto serverPackDownload = Net::SegmentedDownload::makeApiFile(
        source.downloadUrl, archivePath(stagingPath), APPLICATION->network(), job->scheduler(),
        APPLICATION->settings()->get("SegmentedDownloadSegments").toInt());
    if (auto* validator = Flame::createCurseForgeChecksumValidator(source.hashType, source.hash)) {
        serverPackDownload->addValidator(validator);
    }
    Internal::abortDownloadJobOnTaskFailure(job, serverPackDownload.get(), context, std::move(onFailure));
    job->addTask(serverPackDownload);
}

QString extractServerPack(const QString& stagingPath)
{
    return ModPlatform::ServerPackStaging::extractPublishedServerPack(archivePath(stagingPath), stagingPath,
                                                                      QStringLiteral("curseforge"), QStringLiteral("CurseForge"));
}

namespace Internal {
void connectDownloadJobCompletion(NetJob* job,
                                  QObject* context,
                                  std::function<void()> onSucceeded,
                                  std::function<void(QString)> onFailed,
                                  std::function<void()> onAborted)
{
    // Task::emitFailed() emits failed and then finished too, so the latter must not run success work.
    QObject::connect(job, &NetJob::failed, context, std::move(onFailed));
    QObject::connect(job, &NetJob::finished, context,
                     [job, onSucceeded = std::move(onSucceeded), onAborted = std::move(onAborted)]() mutable {
                         if (job->wasSuccessful()) {
                             onSucceeded();
                         } else if (job->getState() == Task::State::AbortedByUser) {
                             onAborted();
                         }
                     });
}

void abortDownloadJobOnTaskFailure(NetJob* job,
                                   Task* watchedTask,
                                   QObject* context,
                                   std::function<void(QString)> onFailure)
{
    QPointer<NetJob> guardedJob(job);
    QPointer<Task> guardedTask(watchedTask);
    QObject::connect(watchedTask, &Task::finished, context,
                     [guardedJob, guardedTask, onFailure = std::move(onFailure)]() mutable {
                         if (!guardedJob || !guardedTask || !guardedJob->isRunning()
                             || guardedTask->getState() != Task::State::Failed) {
                             return;
                         }

                         onFailure(guardedTask->failReason());
                         if (guardedJob && guardedJob->isRunning())
                             guardedJob->abort();
                     },
                     Qt::DirectConnection);
}
}  // namespace Internal

}  // namespace Flame
