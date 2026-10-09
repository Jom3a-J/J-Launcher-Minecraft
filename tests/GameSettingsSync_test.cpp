// SPDX-License-Identifier: GPL-3.0-only

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "minecraft/GameSettingsSync.h"

using GameSettingsSync::OptionRules;
using GameSettingsSync::Plan;

namespace {
bool writeText(const QString& path, const QByteArray& text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
}

QByteArray readText(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

OptionRules rules(QSet<QString> excluded = {}, bool modOptions = true, bool namedKeys = true)
{
    OptionRules result;
    result.excluded = std::move(excluded);
    result.modOptions = modOptions;
    result.namedKeys = namedKeys;
    return result;
}

Plan everything()
{
    Plan plan;
    plan.gameSettings = plan.servers = plan.commandHistory = plan.hotbars = plan.modSettings = true;
    return plan;
}

// The shared copy, two instances' game folders, and their backup folders and launch states.
struct Setup {
    QTemporaryDir root;
    QString store() const { return root.filePath("data/sync"); }
    QString game(const QString& name) const { return root.filePath(name + "/minecraft"); }
    QString backup(const QString& name) const { return root.filePath(name + "/settings-sync-backup"); }
    QString state(const QString& name) const { return root.filePath(name + "/.jlsync/launch-state.json"); }

    GameSettingsSync::Report apply(const QString& name, const Plan& plan = everything()) const
    {
        return GameSettingsSync::applyToInstance(store(), game(name), backup(name), state(name), plan);
    }
    GameSettingsSync::Report collect(const QString& name, const Plan& plan = everything()) const
    {
        return GameSettingsSync::collectFromInstance(store(), game(name), state(name), plan);
    }
};
}  // namespace

class GameSettingsSyncTest : public QObject {
    Q_OBJECT

   private slots:
    // ---- Putting the shared settings into an instance ----

    void appliesOnlyTheSyncedSettings()
    {
        const QString instance = "version:3465\nfov:0.0\ngamma:0.0\nlastServer:old\nresourcePacks:[\"vanilla\"]\n";
        const QString store = "version:3955\nfov:0.5\ngamma:1.0\nlastServer:new\nresourcePacks:[\"file/x.zip\"]\n";
        // gamma is this instance's own; lastServer is left out everywhere; version and the pack list never move.
        QCOMPARE(GameSettingsSync::applyOptions(instance, store, rules({ "gamma", "lastServer" })),
                 QString("version:3465\nfov:0.5\ngamma:0.0\nlastServer:old\nresourcePacks:[\"vanilla\"]\n"));
    }

    void modSettingsOnlyGoWhereTheModIs()
    {
        const QString store = "fov:0.5\nsodium.quality:high\n";
        QCOMPARE(GameSettingsSync::applyOptions("fov:0.0\n", store, rules()), QString("fov:0.5\n"));
        QCOMPARE(GameSettingsSync::applyOptions("fov:0.0\nsodium.quality:low\n", store, rules()),
                 QString("fov:0.5\nsodium.quality:high\n"));
        QCOMPARE(GameSettingsSync::applyOptions("fov:0.0\nsodium.quality:low\n", store, rules({}, false)),
                 QString("fov:0.5\nsodium.quality:low\n"));
    }

    void keybindsOnlyBetween113AndLater()
    {
        const QString store = "key_key.jump:key.keyboard.space\nfov:0.5\n";
        QCOMPARE(GameSettingsSync::applyOptions("key_key.jump:57\nfov:0.0\n", store, rules({}, true, false)),
                 QString("key_key.jump:57\nfov:0.5\n"));
        QCOMPARE(GameSettingsSync::applyOptions("key_key.jump:key.keyboard.j\n", store, rules()),
                 QString("key_key.jump:key.keyboard.space\nfov:0.5\n"));
        QVERIFY(GameSettingsSync::usesNamedKeys("1.13"));
        QVERIFY(GameSettingsSync::usesNamedKeys("1.21.1"));
        QVERIFY(!GameSettingsSync::usesNamedKeys("1.12.2"));
        QVERIFY(!GameSettingsSync::usesNamedKeys(QString()));
    }

    void takesTheDataVersionOnlyWhenTheInstanceHasNone()
    {
        const QString store = "version:3955\nfov:0.5\n";
        QCOMPARE(GameSettingsSync::applyOptions(QString(), store, rules()), QString("version:3955\nfov:0.5\n"));
        QCOMPARE(GameSettingsSync::applyOptions("version:1343\n", store, rules()), QString("version:1343\nfov:0.5\n"));
    }

    void leavesAnUnchangedFileAsItIs()
    {
        const QString instance = "fov:0.5\r\nsodium.quality:low\r\n";
        QCOMPARE(GameSettingsSync::applyOptions(instance, "fov:0.5\n", rules()), instance);
    }

    // ---- Saving what changed back into the shared copy ----

    void savesOnlyWhatChangedDuringTheSession()
    {
        const QString store = "fov:0.5\ngamma:0.5\n";
        const QString atLaunch = "fov:0.5\ngamma:0.5\nmaxFps:120\n";
        const QString now = "fov:0.5\ngamma:1.0\nmaxFps:120\n";
        QCOMPARE(GameSettingsSync::collectOptions(store, now, atLaunch, rules()), QString("fov:0.5\ngamma:1.0\nmaxFps:120\n"));
    }

    void twoGamesKeepEachOthersChanges()
    {
        const QString store = "fov:0.5\ngamma:0.5\n";
        const QString first = GameSettingsSync::collectOptions(store, "fov:0.9\ngamma:0.5\n", store, rules());
        const QString second = GameSettingsSync::collectOptions(first, "fov:0.5\ngamma:1.0\n", store, rules());
        QCOMPARE(second, QString("fov:0.9\ngamma:1.0\n"));
    }

    void neverSavesLeftOutOrOwnSettings()
    {
        const QString store = "fov:0.5\nlastServer:a\n";
        const QString now = "fov:0.9\nlastServer:b\nversion:3955\nresourcePacks:[]\n";
        QCOMPARE(GameSettingsSync::collectOptions(store, now, store, rules({ "fov", "lastServer" })), store);
        QCOMPARE(GameSettingsSync::collectOptions(store, "key_key.jump:57\n", QString(), rules({}, true, false)), store);
    }

    void savesModSettingsOnlyWhenTheyAreSynced()
    {
        const QString now = "fov:0.5\nsodium.quality:high\n";
        QCOMPARE(GameSettingsSync::collectOptions("fov:0.5\n", now, "fov:0.5\n", rules()), QString("fov:0.5\nsodium.quality:high\n"));
        QCOMPARE(GameSettingsSync::collectOptions("fov:0.5\n", now, "fov:0.5\n", rules({}, false)), QString("fov:0.5\n"));
    }

    // ---- Whole instances ----

    void syncsBetweenInstancesBothWays()
    {
        Setup s;
        QVERIFY(writeText(s.store() + "/options.txt", "fov:0.5\n"));
        QVERIFY(writeText(s.game("a") + "/options.txt", "fov:0.0\n"));
        QVERIFY(writeText(s.game("b") + "/options.txt", "fov:0.0\n"));

        // A starts, gets the shared value, and changes it in-game.
        auto report = s.apply("a");
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(readText(s.game("a") + "/options.txt"), QByteArray("fov:0.5\n"));
        QVERIFY(writeText(s.game("a") + "/options.txt", "fov:0.8\n"));
        report = s.collect("a");
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(readText(s.store() + "/options.txt"), QByteArray("fov:0.8\n"));

        // B starts afterwards and gets A's change.
        s.apply("b");
        QCOMPARE(readText(s.game("b") + "/options.txt"), QByteArray("fov:0.8\n"));
    }

    void syncsServersCommandHistoryAndHotbarsAsWholeFiles()
    {
        Setup s;
        QVERIFY(writeText(s.store() + "/servers.dat", "shared servers"));
        QVERIFY(writeText(s.store() + "/hotbar.nbt", "shared hotbar"));
        QVERIFY(writeText(s.game("a") + "/servers.dat", "own servers"));

        auto report = s.apply("a");
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(readText(s.game("a") + "/servers.dat"), QByteArray("shared servers"));
        QCOMPARE(readText(s.game("a") + "/hotbar.nbt"), QByteArray("shared hotbar"));
        QCOMPARE(readText(s.backup("a") + "/servers.dat"), QByteArray("own servers"));
        QCOMPARE(report.backupsMade, 1);

        // A new command history, a changed server list; the hotbar is untouched.
        QVERIFY(writeText(s.game("a") + "/servers.dat", "changed servers"));
        QVERIFY(writeText(s.game("a") + "/command_history.txt", "/time set day\n"));
        report = s.collect("a");
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(readText(s.store() + "/servers.dat"), QByteArray("changed servers"));
        QCOMPARE(readText(s.store() + "/command_history.txt"), QByteArray("/time set day\n"));
        QCOMPARE(readText(s.store() + "/hotbar.nbt"), QByteArray("shared hotbar"));
    }

    void anUnchangedFileDoesNotOverwriteAnotherGamesChange()
    {
        Setup s;
        QVERIFY(writeText(s.store() + "/servers.dat", "shared"));
        s.apply("a");
        s.apply("b");
        QVERIFY(writeText(s.game("a") + "/servers.dat", "added a server in a"));
        s.collect("a");
        s.collect("b");  // b was open at the same time and changed nothing
        QCOMPARE(readText(s.store() + "/servers.dat"), QByteArray("added a server in a"));
    }

    void backsUpAnInstancesOwnFilesOnlyOnce()
    {
        Setup s;
        QVERIFY(writeText(s.store() + "/servers.dat", "shared 1"));
        QVERIFY(writeText(s.game("a") + "/servers.dat", "own"));
        s.apply("a");
        QVERIFY(writeText(s.store() + "/servers.dat", "shared 2"));
        const auto report = s.apply("a");
        QCOMPARE(report.backupsMade, 0);
        QCOMPARE(readText(s.game("a") + "/servers.dat"), QByteArray("shared 2"));
        QCOMPARE(readText(s.backup("a") + "/servers.dat"), QByteArray("own"));
    }

    void neverUsesTheRecordOfAnEarlierSession()
    {
        Setup s;
        QVERIFY(writeText(s.store() + "/options.txt", "fov:0.8\n"));
        QVERIFY(writeText(s.game("a") + "/options.txt", "fov:0.5\n"));
        // A record left from an earlier session, which cannot be replaced.
        QVERIFY(writeText(s.state("a"), "{\"options\":\"fov:0.1\\n\",\"files\":{}}"));
        QVERIFY(QFile::setPermissions(s.state("a"), QFile::ReadOwner));

        const auto report = s.apply("a");
        QFile::setPermissions(s.state("a"), QFile::ReadOwner | QFile::WriteOwner);
        QVERIFY(!report.errors.isEmpty());
        // Nothing was put in, so closing the game cannot mistake sync's values for the player's.
        QCOMPARE(readText(s.game("a") + "/options.txt"), QByteArray("fov:0.5\n"));
    }

    void hotbarsOnlyFrom113On()
    {
        Setup s;
        QVERIFY(writeText(s.store() + "/hotbar.nbt", "shared hotbar"));
        QVERIFY(writeText(s.game("old") + "/hotbar.nbt", "old hotbar"));
        Plan plan;
        plan.hotbars = true;
        plan.options.namedKeys = false;
        s.apply("old", plan);
        QCOMPARE(readText(s.game("old") + "/hotbar.nbt"), QByteArray("old hotbar"));
        QVERIFY(writeText(s.game("old") + "/hotbar.nbt", "changed old hotbar"));
        s.collect("old", plan);
        QCOMPARE(readText(s.store() + "/hotbar.nbt"), QByteArray("shared hotbar"));
    }

    void savesNothingWithoutALaunchRecord()
    {
        Setup s;
        QVERIFY(writeText(s.game("a") + "/options.txt", "fov:0.9\n"));
        QVERIFY(writeText(s.game("a") + "/servers.dat", "servers"));
        const auto report = s.collect("a");
        QVERIFY(report.changed.isEmpty());
        QVERIFY(!QFileInfo::exists(s.store()));
    }

    void modSettingsFilesOnlyGoWhereTheModIs()
    {
        Setup s;
        Plan plan;
        plan.modSettings = true;
        plan.sharedConfig = { "sodium-options.json", "xaero", "../escape" };
        QVERIFY(writeText(s.store() + "/config/sodium-options.json", "{\"quality\":1}"));
        QVERIFY(writeText(s.store() + "/config/xaero/minimap.txt", "zoom:2"));
        QVERIFY(writeText(s.game("a") + "/config/sodium-options.json", "{\"quality\":0}"));
        QVERIFY(QDir().mkpath(s.game("b") + "/config"));

        s.apply("a", plan);
        s.apply("b", plan);
        QCOMPARE(readText(s.game("a") + "/config/sodium-options.json"), QByteArray("{\"quality\":1}"));
        QVERIFY(!QFileInfo::exists(s.game("a") + "/config/xaero"));
        QVERIFY(!QFileInfo::exists(s.game("b") + "/config/sodium-options.json"));

        QVERIFY(writeText(s.game("a") + "/config/sodium-options.json", "{\"quality\":2}"));
        s.collect("a", plan);
        QCOMPARE(readText(s.store() + "/config/sodium-options.json"), QByteArray("{\"quality\":2}"));
    }

    void syncsOnlyWhatThePlanAllows()
    {
        Setup s;
        QVERIFY(writeText(s.store() + "/options.txt", "fov:0.5\n"));
        QVERIFY(writeText(s.store() + "/servers.dat", "shared servers"));
        QVERIFY(writeText(s.store() + "/hotbar.nbt", "shared hotbar"));
        QVERIFY(writeText(s.game("a") + "/options.txt", "fov:0.0\n"));
        Plan plan;
        plan.servers = true;
        s.apply("a", plan);
        QCOMPARE(readText(s.game("a") + "/options.txt"), QByteArray("fov:0.0\n"));
        QCOMPARE(readText(s.game("a") + "/servers.dat"), QByteArray("shared servers"));
        QVERIFY(!QFileInfo::exists(s.game("a") + "/hotbar.nbt"));

        QVERIFY(writeText(s.game("a") + "/options.txt", "fov:0.9\n"));
        s.collect("a", plan);
        QCOMPARE(readText(s.store() + "/options.txt"), QByteArray("fov:0.5\n"));
    }

    void startsFromTheChosenInstance()
    {
        Setup s;
        QVERIFY(writeText(s.store() + "/options.txt", "fov:0.1\n"));
        QVERIFY(writeText(s.game("start") + "/options.txt",
                          "version:1343\nkey_key.jump:57\nfov:0.7\nresourcePacks:[]\nsodium.quality:high\n"));
        QVERIFY(writeText(s.game("start") + "/servers.dat", "start servers"));
        Plan plan = everything();
        plan.options.namedKeys = false;  // a 1.12 instance: its keybinds are not the shared ones

        const auto report = GameSettingsSync::initializeStore(s.store(), s.game("start"), plan);
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(readText(s.store() + "/options.txt"), QByteArray("version:1343\nfov:0.7\nsodium.quality:high\n"));
        QCOMPARE(readText(s.store() + "/servers.dat"), QByteArray("start servers"));
    }
};

QTEST_GUILESS_MAIN(GameSettingsSyncTest)

#include "GameSettingsSync_test.moc"
