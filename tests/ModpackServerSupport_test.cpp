// SPDX-License-Identifier: GPL-3.0-only
#include <QtTest>

#include "modplatform/ServerSupport.h"
#include "modplatform/ftb/FTBPackInstallTask.h"

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
};

QTEST_GUILESS_MAIN(ModpackServerSupportTest)
#include "ModpackServerSupport_test.moc"
