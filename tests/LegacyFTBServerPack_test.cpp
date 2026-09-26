// SPDX-License-Identifier: GPL-3.0-only
#include <QtTest>

#include "modplatform/ServerSupport.h"
#include "modplatform/legacy_ftb/PackFetchTask.h"

class LegacyFTBServerPackTest final : public QObject {
    Q_OBJECT
   private slots:
    void parserReadsServerPackAttribute()
    {
        LegacyFTB::PackFetchTask parser(nullptr);
        QByteArray xml = R"(<modpacks><modpack name="FTB Academy" dir="FTBAcademy" repoVersion="1_1_0" version="1.1.0" url="FTBAcademy.zip" serverPack="FTBAcademyServer.zip"/></modpacks>)";
        for (const auto type : { LegacyFTB::PackType::Public, LegacyFTB::PackType::ThirdParty, LegacyFTB::PackType::Private }) {
            LegacyFTB::ModpackList packs;
            QVERIFY(parser.parseAndAddPacks(xml, type, packs));
            QCOMPARE(packs.size(), 1);
            QCOMPARE(packs.first().serverPack, QStringLiteral("FTBAcademyServer.zip"));
        }
    }

    void serverPackSupportMapping()
    {
        QCOMPARE(ModPlatform::legacyFtbServerSupport(QStringLiteral("server.zip")), ModPlatform::ServerSupport::Official);
        QCOMPARE(ModPlatform::legacyFtbServerSupport({}), ModPlatform::ServerSupport::ClientDerived);
    }

    void serverPackUrlUsesSelectedVersionFolder()
    {
        const QString base = QStringLiteral("https://dist.creeper.host/FTB2/");
        const auto current = ModPlatform::legacyFtbPackUrl(base, false, "FTBAcademy", "1.1.0", "FTBAcademyServer.zip");
        const auto old = ModPlatform::legacyFtbPackUrl(base, false, "FTBAcademy", "1.0.2", "FTBAcademyServer.zip");
        QCOMPARE(current.toString(), QStringLiteral("https://dist.creeper.host/FTB2/modpacks/FTBAcademy/1_1_0/FTBAcademyServer.zip"));
        QCOMPARE(old.toString(), QStringLiteral("https://dist.creeper.host/FTB2/modpacks/FTBAcademy/1_0_2/FTBAcademyServer.zip"));
        const auto privateUrl = ModPlatform::legacyFtbPackUrl(base, true, "PrivatePack", "2.0", "Server.zip");
        QCOMPARE(privateUrl.toString(), QStringLiteral("https://dist.creeper.host/FTB2/privatepacks/PrivatePack/2_0/Server.zip"));
    }
};

QTEST_GUILESS_MAIN(LegacyFTBServerPackTest)
#include "LegacyFTBServerPack_test.moc"
