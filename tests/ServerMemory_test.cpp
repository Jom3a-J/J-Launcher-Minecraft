// SPDX-License-Identifier: GPL-3.0-only

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>

#include <server/ServerInstance.h>
#include <server/ServerManager.h>
#include <server/ServerMemory.h>
#include <server/ServerModpackInstaller.h>
#include <ui/dialogs/CreateServerDialog.h>

namespace {
bool writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

// A 32 GiB workstation, the reference host used by the tier expectations.
constexpr quint64 kWorkstationRamMiB = 32768;

void checkInvariants(const ServerMemoryRecommendation& recommendation, quint64 totalRamMiB)
{
    QVERIFY(recommendation.maxMemoryMiB >= 256);
    QVERIFY(recommendation.maxMemoryMiB <= 32768);
    QVERIFY(recommendation.minMemoryMiB >= 256);
    QVERIFY(recommendation.minMemoryMiB <= recommendation.maxMemoryMiB);
    QCOMPARE(recommendation.maxMemoryMiB % 256, 0);
    QCOMPARE(recommendation.minMemoryMiB % 256, 0);
    if (totalRamMiB >= 1024) {
        QVERIFY(recommendation.maxMemoryMiB <= static_cast<int>((totalRamMiB * 3) / 4));
    }
}
}  // namespace

class ServerMemoryTest : public QObject {
    Q_OBJECT

   private slots:
    void sizesVanillaAtTwoGibibytes()
    {
        const auto recommendation = ServerMemory::recommend("vanilla", 0, kWorkstationRamMiB);
        QCOMPARE(recommendation.maxMemoryMiB, 2048);
        QCOMPARE(recommendation.minMemoryMiB, 1024);
        QVERIFY(!recommendation.fromProviderRecommendation);
        QVERIFY(!recommendation.clampedToHost);
        checkInvariants(recommendation, kWorkstationRamMiB);
    }

    void sizesPluginServersAtThreeGibibytes()
    {
        for (const QString& loader : { QStringLiteral("paper"), QStringLiteral("purpur") }) {
            const auto recommendation = ServerMemory::recommend(loader, 25, kWorkstationRamMiB);
            QCOMPARE(recommendation.maxMemoryMiB, 3072);
            QCOMPARE(recommendation.minMemoryMiB, 1536);
            checkInvariants(recommendation, kWorkstationRamMiB);
        }
    }

    void scalesModdedMemoryWithServerSideJarCount()
    {
        QCOMPARE(ServerMemory::recommend("forge", 5, kWorkstationRamMiB).maxMemoryMiB, 4096);
        QCOMPARE(ServerMemory::recommend("fabric", 40, kWorkstationRamMiB).maxMemoryMiB, 5120);
        QCOMPARE(ServerMemory::recommend("neoforge", 100, kWorkstationRamMiB).maxMemoryMiB, 6144);
        QCOMPARE(ServerMemory::recommend("forge", 250, kWorkstationRamMiB).maxMemoryMiB, 8192);
        QCOMPARE(ServerMemory::recommend("forge", 400, kWorkstationRamMiB).maxMemoryMiB, 10240);
    }

    void sizesRlcraftScalePacksWellAboveTheOldDefault()
    {
        // ~178 deployed mods on a 32 GiB machine should land around 6 GiB,
        // materially above the old 2048 MiB default.
        const auto recommendation = ServerMemory::recommend("forge", 178, kWorkstationRamMiB);
        QCOMPARE(recommendation.maxMemoryMiB, 6144);
        QCOMPARE(recommendation.minMemoryMiB, 3072);
        QVERIFY(recommendation.maxMemoryMiB > 2048);
        checkInvariants(recommendation, kWorkstationRamMiB);
    }

    void honorsExplicitProviderRecommendations()
    {
        const auto recommendation = ServerMemory::recommend("forge", 178, kWorkstationRamMiB, 8192);
        QCOMPARE(recommendation.maxMemoryMiB, 8192);
        QCOMPARE(recommendation.minMemoryMiB, 4096);
        QVERIFY(recommendation.fromProviderRecommendation);
    }

    void resolvesProviderTrustWithoutInstanceSettings()
    {
        // A true per-instance override wins.
        QCOMPARE(ServerMemory::resolveProviderRecommendation(true, 6144, 4096), 6144);
        // Otherwise the exported pack recommendation is used.
        QCOMPARE(ServerMemory::resolveProviderRecommendation(false, 6144, 4096), 4096);
        // The global default alone must not count.
        QCOMPARE(ServerMemory::resolveProviderRecommendation(false, 2048, 0), 0);
        QCOMPARE(ServerMemory::resolveProviderRecommendation(false, 0, 0), 0);
    }

    void clampsToLowMemoryHostsWithoutInvalidValues()
    {
        // 4 GiB laptop: reserve 2 GiB for the OS, so a large pack clamps.
        const auto laptop = ServerMemory::recommend("forge", 178, 4096);
        QCOMPARE(laptop.maxMemoryMiB, 2048);
        QCOMPARE(laptop.minMemoryMiB, 1024);
        QVERIFY(laptop.clampedToHost);
        checkInvariants(laptop, 4096);

        // 2 GiB machine: the fractional cap still yields a usable pair.
        const auto tiny = ServerMemory::recommend("vanilla", 0, 2048);
        QCOMPARE(tiny.maxMemoryMiB, 1536);
        QCOMPARE(tiny.minMemoryMiB, 768);
        QVERIFY(tiny.clampedToHost);
        checkInvariants(tiny, 2048);
    }

    void capsAtSeventyFivePercentOfTotalRam()
    {
        // Even an inflated provider value cannot exceed 75% of 32 GiB.
        const auto capped = ServerMemory::recommend("forge", 0, kWorkstationRamMiB, 30000);
        QCOMPARE(capped.maxMemoryMiB, 24576);
        QCOMPARE(capped.minMemoryMiB, 12288);
        QVERIFY(capped.clampedToHost);
        checkInvariants(capped, kWorkstationRamMiB);
    }

    void keepsInvariantsAcrossLoadersAndHostSizes()
    {
        const QStringList loaders{ "vanilla", "paper", "purpur", "fabric",
                                   "forge", "neoforge", "unknown" };
        const QList<quint64> hosts{ 2048, 4096, 8192, 16384, 32768, 65536 };
        const QList<int> counts{ 0, 1, 20, 60, 178, 400 };
        for (const QString& loader : loaders) {
            for (quint64 host : hosts) {
                for (int count : counts) {
                    checkInvariants(ServerMemory::recommend(loader, count, host), host);
                }
            }
        }
    }

    void countsOnlyEnabledServerSideJars()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(writeFile(QDir(root.path()).filePath("server/mods/a.jar"), "a"));
        QVERIFY(writeFile(QDir(root.path()).filePath("server/mods/b.jar"), "b"));
        QVERIFY(writeFile(QDir(root.path()).filePath("server/mods/c.jar.disabled"), "c"));
        QVERIFY(writeFile(QDir(root.path()).filePath("server/plugins/p.jar"), "p"));

        const QString serverDir = QDir(root.path()).filePath("server");
        QCOMPARE(ServerMemory::countDeployedServerJars(serverDir, "forge"), 2);
        QCOMPARE(ServerMemory::countDeployedServerJars(serverDir, "paper"), 1);
        QCOMPARE(ServerMemory::countDeployedServerJars(serverDir, "vanilla"), 0);
        QCOMPARE(ServerMemory::countDeployedServerJars(
                     QDir(root.path()).filePath("missing"), "forge"),
                 0);
    }

    void modpackCreationSizesMemoryAndPersistsAfterReload()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());
        const QString instanceRoot = root.filePath("instance");
        const QString gameRoot = QDir(instanceRoot).filePath("minecraft");
        for (int index = 0; index < 178; ++index) {
            QVERIFY(writeFile(QDir(gameRoot).filePath(
                                  QString("mods/mod-%1.jar").arg(index)),
                              QByteArray("mod")));
        }

        const QString dataDir = root.filePath("server-data");
        QString serverId;
        {
            ServerManager manager(dataDir);
            const auto profile = ServerModpackInstaller::profileForVersions(
                "1.20.1", {}, "47.1.0", {}, {});
            QVERIFY(profile.isValid());
            const auto result = ServerModpackInstaller::createMatchingServer(
                &manager, profile, instanceRoot, gameRoot, "RLCraft Scale Server",
                0, kWorkstationRamMiB);
            QVERIFY2(result.isValid(), qPrintable(result.error));
            serverId = result.serverId;
            const auto server = manager.getServer(serverId);
            QVERIFY(server);
            QCOMPARE(server->maxMemory(), 6144);
            QCOMPARE(server->minMemory(), 3072);
        }

        ServerManager reloaded(dataDir);
        QVERIFY(reloaded.load());
        const auto restored = reloaded.getServer(serverId);
        QVERIFY(restored);
        QCOMPARE(restored->maxMemory(), 6144);
        QCOMPARE(restored->minMemory(), 3072);
    }

    void dialogDefaultsToAutomaticAndKeepsManualOverride()
    {
        CreateServerDialog dialog;
        QVERIFY(dialog.isMemoryAutomatic());
        auto* autoCheck = dialog.findChild<QCheckBox*>("serverAutoMemoryCheck");
        auto* minSpin = dialog.findChild<QSpinBox*>("serverMinMemoryInput");
        auto* maxSpin = dialog.findChild<QSpinBox*>("serverMaxMemoryInput");
        auto* info = dialog.findChild<QLabel*>("serverAutoMemoryLabel");
        auto* typeCombo = dialog.findChild<QComboBox*>("serverTypeCombo");
        QVERIFY(autoCheck && minSpin && maxSpin && info && typeCombo);
        QVERIFY(autoCheck->isChecked());
        QVERIFY(!minSpin->isEnabled());
        QVERIFY(!maxSpin->isEnabled());
        QVERIFY(info->text().contains("Automatic", Qt::CaseInsensitive));

        const int vanillaMax = maxSpin->value();
        QVERIFY(vanillaMax > 0);
        typeCombo->setCurrentText("Forge");
        QVERIFY(dialog.isMemoryAutomatic());
        QVERIFY(maxSpin->value() >= vanillaMax);

        // An intentional edit opts out of automation...
        const int manualMin = minSpin->value() == 256 ? 512 : 256;
        minSpin->setValue(manualMin);
        QVERIFY(!dialog.isMemoryAutomatic());
        QVERIFY(!autoCheck->isChecked());
        QVERIFY(minSpin->isEnabled());

        // ...and later type changes must not overwrite the manual choice.
        typeCombo->setCurrentText("Paper");
        QVERIFY(!dialog.isMemoryAutomatic());
        QCOMPARE(minSpin->value(), manualMin);
    }
};

QTEST_MAIN(ServerMemoryTest)

#include "ServerMemory_test.moc"
