// SPDX-License-Identifier: GPL-3.0-only
#include <QtTest>
#include <QCoreApplication>
#include <utility>

#include "modplatform/ServerSupport.h"
#include "modplatform/ServerSupportRequestQueue.h"
#include "modplatform/ftb/FTBPackInstallTask.h"
#include "ui/pages/modplatform/technic/TechnicModel.h"

class ModpackServerSupportTest final : public QObject {
    Q_OBJECT
   private slots:
    void curseForgeLatestFiles()
    {
        auto parse = [](QByteArray json) { return QJsonDocument::fromJson(json).object(); };
        QCOMPARE(ModPlatform::curseForgeServerSupport(parse(R"({"latestFiles":[{"serverPackFileId":42}]})")),
                 ModPlatform::ServerSupport::Official);
        QCOMPARE(ModPlatform::curseForgeServerSupport(parse(R"({"latestFiles":[{"isServerPack":true}]})")),
                 ModPlatform::ServerSupport::Official);
        QCOMPARE(ModPlatform::curseForgeServerSupport(parse(R"({"latestFiles":[{}]})")),
                 ModPlatform::ServerSupport::ClientDerived);
    }

    void technicServerUrls()
    {
        QCOMPARE(ModPlatform::technicServerSupport(QUrl("https://servers.technicpack.net/Technic/servers/tekkitmain/Tekkit_Server_v1.2.9g-2.zip")),
                 ModPlatform::ServerSupport::Official);
        QCOMPARE(ModPlatform::technicServerSupport(QUrl("https://www.gtnewhorizons.com/downloads/")),
                 ModPlatform::ServerSupport::Website);
        QCOMPARE(ModPlatform::technicServerSupport({}), ModPlatform::ServerSupport::ClientDerived);
    }

    void ftbProbeStatuses()
    {
        QCOMPARE(FTB::serverPackSupportFromHttpStatus(200, false), ModPlatform::ServerSupport::Official);
        QCOMPARE(FTB::serverPackSupportFromHttpStatus(404, true), ModPlatform::ServerSupport::ClientDerived);
        QCOMPARE(FTB::serverPackSupportFromHttpStatus(0, true), ModPlatform::ServerSupport::Unknown);
    }

    void providerQueueLimits()
    {
        QObject owner;
        auto checkLimit = [&owner](int limit, int count) {
            using Queue = ModPlatform::ServerSupportRequestQueue<int>;
            Queue queue(limit);
            QList<Queue::Completion> completions;
            int peak = 0;
            for (int i = 0; i < count; ++i) {
                queue.request(QString::number(i), &owner,
                    [&queue, &completions, &peak](Queue::Completion complete) {
                        peak = qMax(peak, queue.activeCount());
                        completions.append(std::move(complete));
                        return Queue::Cancel{};
                    }, [](Queue::Result) {});
            }
            while (!completions.isEmpty()) completions.takeFirst()(Queue::Result{ 0 });
            QCOMPARE(peak, limit);
            QCOMPARE(queue.activeCount(), 0);
        };
        QCOMPARE(FTB::ServerPackProbeConcurrency, 1);
        QCOMPARE(Technic::PackDetailsConcurrency, 4);
        checkLimit(FTB::ServerPackProbeConcurrency, 4);
        checkLimit(Technic::PackDetailsConcurrency, 7);
    }

    void providerQueueCachesCompletedKeys()
    {
        using Queue = ModPlatform::ServerSupportRequestQueue<ModPlatform::ServerSupport>;
        Queue queue(1);
        QObject owner;
        int starts = 0;
        ModPlatform::ServerSupport result = ModPlatform::ServerSupport::Unknown;
        Queue::Completion complete;
        auto starter = [&starts, &complete](Queue::Completion done) {
            ++starts;
            complete = std::move(done);
            return Queue::Cancel{};
        };
        queue.request("same", &owner, starter, [&result](Queue::Result value) { result = *value; });
        complete(ModPlatform::ServerSupport::Official);
        QCOMPARE(starts, 1);
        queue.request("same", &owner, starter, [&result](Queue::Result value) { result = *value; });
        QCoreApplication::processEvents();
        QCOMPARE(starts, 1);
        QCOMPARE(result, ModPlatform::ServerSupport::Official);
    }

    void providerQueueResetDropsPendingOwner()
    {
        using Queue = ModPlatform::ServerSupportRequestQueue<int>;
        Queue queue(1);
        QObject activeOwner;
        QObject pendingOwner;
        QList<Queue::Completion> completions;
        int starts = 0;
        auto starter = [&completions, &starts](Queue::Completion done) {
            ++starts;
            completions.append(std::move(done));
            return Queue::Cancel{};
        };
        queue.request("active", &activeOwner, starter, [](Queue::Result) {});
        queue.request("pending", &pendingOwner, starter, [](Queue::Result) {});
        queue.cancelOwner(&pendingOwner);
        QCOMPARE(starts, 1);
        QCOMPARE(queue.pendingCount(), 0);
        completions.takeFirst()(Queue::Result{ 1 });
        QCOMPARE(starts, 1);
        QCOMPARE(queue.activeCount(), 0);
    }
};

QTEST_GUILESS_MAIN(ModpackServerSupportTest)
#include "ModpackServerSupport_test.moc"
