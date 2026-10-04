// SPDX-License-Identifier: GPL-3.0-only

#include "TechnicPackDetails.h"

#include "BuildConfig.h"
#include "modplatform/ServerSupportRequestQueue.h"
#include "net/ApiDownload.h"
#include "net/NetJob.h"

#include <QJsonDocument>
#include <QUrl>

#include <memory>
#include <utility>

namespace Technic {
namespace {
ModPlatform::ServerSupportRequestQueue<QJsonObject>& packDetailsQueue()
{
    static ModPlatform::ServerSupportRequestQueue<QJsonObject> queue(PackDetailsConcurrency);
    return queue;
}
}

void requestPackDetails(QNetworkAccessManager* network, const QString& slug, QObject* owner, PackDetailsCallback callback)
{
    using Queue = ModPlatform::ServerSupportRequestQueue<QJsonObject>;
    const QString key = slug;
    auto starter = [network, slug](Queue::Completion complete) -> Queue::Cancel {
        auto job = makeShared<NetJob>(QString("Technic::PackMeta(%1)").arg(slug), network);
        const QUrl url(QString("%1modpack/%2?build=%3")
                           .arg(BuildConfig.TECHNIC_API_BASE_URL, slug, BuildConfig.TECHNIC_API_BUILD));
        auto [action, response] = Net::ApiDownload::makeByteArray(url);
        job->addNetAction(action);
        auto completion = std::make_shared<Queue::Completion>(std::move(complete));
        QObject::connect(job.get(), &NetJob::succeeded, job.get(), [response, completion] {
            const QByteArray body = std::move(*response);
            QJsonParseError parseError{};
            const auto document = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()
                || document.object().contains(QStringLiteral("error"))) {
                (*completion)(Queue::Result{});
                return;
            }
            (*completion)(document.object());
        });
        QObject::connect(job.get(), &NetJob::failed, job.get(), [completion](const QString&) {
            (*completion)(Queue::Result{});
        });
        job->start();
        return [job] { job->abort(); };
    };
    packDetailsQueue().request(key, owner, std::move(starter), std::move(callback));
}

void cancelPackDetailsRequests(QObject* owner)
{
    packDetailsQueue().cancelOwner(owner);
}

void cachePackDetails(const QString& slug, const QJsonObject& details)
{
    packDetailsQueue().cacheValue(slug, details);
}
}  // namespace Technic
