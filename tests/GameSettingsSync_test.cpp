// SPDX-License-Identifier: GPL-3.0-only

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <share.h>
#endif

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

// A plan for pack sync in one pack group. Deleted packs are removed straight away (and listed)
// instead of going to the Recycle Bin, so the test leaves nothing there.
Plan packPlan(QStringList* removedPaths, const QString& group = "1.21.1-fabric", bool resource = true, bool shader = false)
{
    Plan plan;
    plan.resourcePacks = resource;
    plan.shaderPacks = shader;
    plan.packGroup = group;
    plan.removeFile = [removedPaths](const QString& path) {
        if (removedPaths)
            *removedPaths << path;
        return QFileInfo(path).isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
    };
    return plan;
}

QStringList entries(const QString& folder)
{
    return QDir(folder).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
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

    // Like the launch step: closing the game uses the session its start returned.
    mutable QHash<QString, QString> sessions;

    GameSettingsSync::Report apply(const QString& name, const Plan& plan = everything()) const
    {
        auto report = GameSettingsSync::applyToInstance(store(), game(name), backup(name), state(name), plan);
        sessions.insert(name, report.session);
        return report;
    }
    GameSettingsSync::Report collect(const QString& name, const Plan& plan = everything()) const
    {
        return GameSettingsSync::collectFromInstance(store(), game(name), state(name), plan, sessions.value(name));
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
        // Snapshots: 17w43a was the first with named keys; 1.12-era ones still use numbers.
        QVERIFY(!GameSettingsSync::usesNamedKeys("17w06a"));
        QVERIFY(!GameSettingsSync::usesNamedKeys("17w31a"));
        QVERIFY(GameSettingsSync::usesNamedKeys("17w43a"));
        QVERIFY(GameSettingsSync::usesNamedKeys("18w01a"));
        QVERIFY(GameSettingsSync::usesNamedKeys("24w14a"));
        // Pre-releases and release candidates belong to their release.
        QVERIFY(GameSettingsSync::usesNamedKeys("1.13-pre1"));
        QVERIFY(GameSettingsSync::usesNamedKeys("1.13 Pre-Release 1"));
        QVERIFY(GameSettingsSync::usesNamedKeys("1.21-rc1"));
        QVERIFY(!GameSettingsSync::usesNamedKeys("1.12.2-pre2"));
        // Anything unrecognised counts as old.
        QVERIFY(!GameSettingsSync::usesNamedKeys("3D Shareware v1.34"));
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
        QVERIFY(!report.errors.isEmpty());
        QVERIFY(report.session.isEmpty());
        // Nothing was put in, so closing the game cannot mistake sync's values for the player's.
        QCOMPARE(readText(s.game("a") + "/options.txt"), QByteArray("fov:0.5\n"));
        // And closing the game saves nothing: the earlier record (fov:0.1) is never compared with.
        s.collect("a");
        QFile::setPermissions(s.state("a"), QFile::ReadOwner | QFile::WriteOwner);
        QCOMPARE(readText(s.store() + "/options.txt"), QByteArray("fov:0.8\n"));
    }

    void onlyComparesWithItsOwnSessionsRecord()
    {
        Setup s;
        QVERIFY(writeText(s.store() + "/options.txt", "fov:0.8\n"));
        QVERIFY(writeText(s.game("a") + "/options.txt", "fov:0.5\n"));
        QVERIFY(writeText(s.state("a"), "{\"session\":\"earlier\",\"options\":\"fov:0.1\\n\",\"files\":{}}"));
        const auto report = GameSettingsSync::collectFromInstance(s.store(), s.game("a"), s.state("a"), everything(), "this-one");
        QVERIFY(report.changed.isEmpty());
        QCOMPARE(readText(s.store() + "/options.txt"), QByteArray("fov:0.8\n"));
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

    // ---- Resource and shader packs ----

    void packsTravelBothWaysWithinAGroup()
    {
        Setup s;
        const Plan plan = packPlan(nullptr);
        const QString group = s.store() + "/packs/1.21.1-fabric/resourcepacks";
        QVERIFY(writeText(group + "/Faithful.zip", "faithful"));
        QVERIFY(writeText(s.game("a") + "/resourcepacks/Mine.zip", "mine"));

        auto report = s.apply("a", plan);
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join('\n')));
        QCOMPARE(entries(s.game("a") + "/resourcepacks"), QStringList({ "Faithful.zip", "Mine.zip" }));

        // A pack added during the session goes to the group; the one it already had too.
        QVERIFY(writeText(s.game("a") + "/resourcepacks/New.zip", "new"));
        report = s.collect("a", plan);
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join('\n')));
        QCOMPARE(entries(group), QStringList({ "Faithful.zip", "Mine.zip", "New.zip" }));

        s.apply("b", plan);
        QCOMPARE(entries(s.game("b") + "/resourcepacks"), QStringList({ "Faithful.zip", "Mine.zip", "New.zip" }));
        QCOMPARE(readText(s.game("b") + "/resourcepacks/New.zip"), QByteArray("new"));
    }

    void packsNeverCrossGroups()
    {
        Setup s;
        QVERIFY(writeText(s.store() + "/packs/1.21.1-fabric/resourcepacks/Faithful.zip", "faithful"));
        s.apply("forge", packPlan(nullptr, "1.20.1-forge"));
        QVERIFY(entries(s.game("forge") + "/resourcepacks").isEmpty());
        QCOMPARE(GameSettingsSync::packGroupName("1.20.1", "forge"), QString("1.20.1-forge"));
        QCOMPARE(GameSettingsSync::packGroupName(QString(), "forge"), QString());

        // No group (unknown version): packs are not synced at all.
        s.apply("unknown", packPlan(nullptr, QString()));
        QVERIFY(entries(s.game("unknown") + "/resourcepacks").isEmpty());
    }

    void aDeletedPackLeavesTheOtherInstances()
    {
        Setup s;
        QStringList removed;
        const Plan plan = packPlan(&removed);
        const QString group = s.store() + "/packs/1.21.1-fabric/resourcepacks";
        QVERIFY(writeText(group + "/Old.zip", "old"));
        s.apply("a", plan);
        s.apply("b", plan);

        QVERIFY(QFile::remove(s.game("a") + "/resourcepacks/Old.zip"));
        s.collect("a", plan);
        QVERIFY(!QFileInfo::exists(group + "/Old.zip"));

        removed.clear();
        const auto report = s.apply("b", plan);
        QVERIFY(report.errors.isEmpty());
        QVERIFY(!QFileInfo::exists(s.game("b") + "/resourcepacks/Old.zip"));
        QCOMPARE(removed, QStringList({ QDir(s.game("b") + "/resourcepacks").filePath("Old.zip") }));

        // Added again later, it syncs again.
        QVERIFY(writeText(s.game("b") + "/resourcepacks/Old.zip", "old again"));
        s.collect("b", plan);
        s.apply("a", plan);
        QCOMPARE(readText(s.game("a") + "/resourcepacks/Old.zip"), QByteArray("old again"));
    }

    void aPackDeletedWhileAnotherGameRanStaysDeleted()
    {
        Setup s;
        const Plan plan = packPlan(nullptr);
        const QString group = s.store() + "/packs/1.21.1-fabric/resourcepacks";
        QVERIFY(writeText(group + "/Old.zip", "old"));
        s.apply("a", plan);
        s.apply("b", plan);  // b is open at the same time

        QVERIFY(QFile::remove(s.game("a") + "/resourcepacks/Old.zip"));
        s.collect("a", plan);
        s.collect("b", plan);  // b still has it and changed nothing
        QVERIFY(!QFileInfo::exists(group + "/Old.zip"));

        s.apply("c", plan);
        QVERIFY(!QFileInfo::exists(s.game("c") + "/resourcepacks/Old.zip"));
    }

    void folderPacksAreCopiedWhole()
    {
        Setup s;
        const Plan plan = packPlan(nullptr);
        QVERIFY(writeText(s.game("a") + "/resourcepacks/FolderPack/pack.mcmeta", "{}"));
        QVERIFY(writeText(s.game("a") + "/resourcepacks/FolderPack/assets/minecraft/a.png", "png"));
        s.apply("a", plan);
        s.collect("a", plan);
        s.apply("b", plan);
        QCOMPARE(readText(s.game("b") + "/resourcepacks/FolderPack/pack.mcmeta"), QByteArray("{}"));
        QCOMPARE(readText(s.game("b") + "/resourcepacks/FolderPack/assets/minecraft/a.png"), QByteArray("png"));
    }

    void startingAgainBringsBackTheStartingPacks()
    {
        Setup s;
        const Plan plan = packPlan(nullptr);
        QVERIFY(writeText(s.store() + "/packs/1.21.1-fabric/sync.json", "{\"removed\":{\"resourcepacks\":[\"Faithful.zip\",\"Other.zip\"]}}"));
        QVERIFY(writeText(s.game("start") + "/resourcepacks/Faithful.zip", "faithful"));
        QVERIFY(GameSettingsSync::initializeStore(s.store(), s.game("start"), plan).errors.isEmpty());
        QCOMPARE(readText(s.store() + "/packs/1.21.1-fabric/sync.json").contains("Faithful.zip"), false);
        QVERIFY(readText(s.store() + "/packs/1.21.1-fabric/sync.json").contains("Other.zip"));
        s.apply("b", plan);
        QCOMPARE(readText(s.game("b") + "/resourcepacks/Faithful.zip"), QByteArray("faithful"));
    }

    void aModpacksOwnShaderSettingsStayItsOwn()
    {
        Setup s;
        Plan plan = packPlan(nullptr, "1.21.1-fabric", false, true);
        plan.modpack = true;
        QVERIFY(writeText(s.game("pack") + "/shaderpacks/Own.zip", "own shader"));
        QVERIFY(writeText(s.game("pack") + "/shaderpacks/Own.zip.txt", "QUALITY=1\n"));
        s.apply("pack", plan);
        QVERIFY(writeText(s.game("pack") + "/shaderpacks/Own.zip.txt", "QUALITY=2\n"));
        QVERIFY(writeText(s.game("pack") + "/shaderpacks/Added.zip.txt", "QUALITY=3\n"));
        s.collect("pack", plan);
        const QString group = s.store() + "/packs/1.21.1-fabric/shaderpacks";
        QVERIFY(!QFileInfo::exists(group + "/Own.zip.txt"));
        QCOMPARE(readText(group + "/Added.zip.txt"), QByteArray("QUALITY=3\n"));
    }

    void aModpacksOwnPacksStayItsOwn()
    {
        Setup s;
        QStringList removed;
        Plan plan = packPlan(&removed);
        plan.modpack = true;
        const QString group = s.store() + "/packs/1.21.1-fabric/resourcepacks";
        QVERIFY(writeText(s.game("pack") + "/resourcepacks/Bundled.zip", "from the modpack"));
        QVERIFY(writeText(group + "/Bundled.zip", "someone else's"));
        // Deleted somewhere else: that must not take the modpack's own copy.
        QVERIFY(writeText(s.store() + "/packs/1.21.1-fabric/sync.json", "{\"removed\":{\"resourcepacks\":[\"Bundled.zip\"]}}"));

        s.apply("pack", plan);
        QCOMPARE(readText(s.game("pack") + "/resourcepacks/Bundled.zip"), QByteArray("from the modpack"));
        QVERIFY(removed.isEmpty());

        // Packs added later are shared; deleting its own pack shares nothing.
        QVERIFY(writeText(s.game("pack") + "/resourcepacks/Added.zip", "added later"));
        QVERIFY(QFile::remove(s.game("pack") + "/resourcepacks/Bundled.zip"));
        s.collect("pack", plan);
        QCOMPARE(readText(group + "/Added.zip"), QByteArray("added later"));
        QCOMPARE(readText(group + "/Bundled.zip"), QByteArray("someone else's"));
    }

    void theSwitchedOnListStaysWithinTheGroup()
    {
        Setup s;
        Plan plan = packPlan(nullptr);
        plan.gameSettings = true;
        QVERIFY(writeText(s.store() + "/options.txt", "fov:0.5\n"));
        QVERIFY(writeText(s.store() + "/packs/1.21.1-fabric/options.txt", "resourcePacks:[\"vanilla\",\"file/A.zip\"]\n"));
        QVERIFY(writeText(s.game("a") + "/options.txt", "fov:0.0\nresourcePacks:[\"vanilla\"]\n"));

        s.apply("a", plan);
        QCOMPARE(readText(s.game("a") + "/options.txt"), QByteArray("fov:0.5\nresourcePacks:[\"vanilla\",\"file/A.zip\"]\n"));

        // A new order goes to the group's list, never to the settings shared with every instance.
        QVERIFY(writeText(s.game("a") + "/options.txt", "fov:0.5\nresourcePacks:[\"file/A.zip\",\"vanilla\"]\n"));
        s.collect("a", plan);
        QCOMPARE(readText(s.store() + "/packs/1.21.1-fabric/options.txt"), QByteArray("resourcePacks:[\"file/A.zip\",\"vanilla\"]\n"));
        QCOMPARE(readText(s.store() + "/options.txt"), QByteArray("fov:0.5\n"));
    }

    void syncsShaderPacksAndTheirSettings()
    {
        Setup s;
        const Plan plan = packPlan(nullptr, "1.21.1-fabric", false, true);
        const QString group = s.store() + "/packs/1.21.1-fabric";
        QVERIFY(writeText(group + "/shaderpacks/BSL.zip", "bsl"));
        QVERIFY(writeText(group + "/shaderpacks/BSL.zip.txt", "SHADOW=true\n"));
        QVERIFY(writeText(group + "/config/iris.properties", "shaderPack=BSL.zip\n"));
        QVERIFY(writeText(s.game("a") + "/config/iris.properties", "shaderPack=\n"));

        auto report = s.apply("a", plan);
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join('\n')));
        QCOMPARE(readText(s.game("a") + "/shaderpacks/BSL.zip"), QByteArray("bsl"));
        QCOMPARE(readText(s.game("a") + "/shaderpacks/BSL.zip.txt"), QByteArray("SHADOW=true\n"));
        QCOMPARE(readText(s.game("a") + "/config/iris.properties"), QByteArray("shaderPack=BSL.zip\n"));
        QCOMPARE(readText(s.backup("a") + "/config/iris.properties"), QByteArray("shaderPack=\n"));

        QVERIFY(writeText(s.game("a") + "/shaderpacks/BSL.zip.txt", "SHADOW=false\n"));
        report = s.collect("a", plan);
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(readText(group + "/shaderpacks/BSL.zip.txt"), QByteArray("SHADOW=false\n"));
        // Resource packs were not chosen: the shader settings file is not a pack, and nothing else moved.
        QVERIFY(!QFileInfo::exists(group + "/resourcepacks"));
    }

    void packsStayPutWhenNotChosen()
    {
        Setup s;
        QVERIFY(writeText(s.store() + "/packs/1.21.1-fabric/resourcepacks/Faithful.zip", "faithful"));
        QVERIFY(writeText(s.store() + "/packs/1.21.1-fabric/shaderpacks/BSL.zip", "bsl"));
        Plan plan = packPlan(nullptr, "1.21.1-fabric", false, false);
        plan.gameSettings = true;
        s.apply("a", plan);
        QVERIFY(!QFileInfo::exists(s.game("a") + "/resourcepacks"));
        QVERIFY(!QFileInfo::exists(s.game("a") + "/shaderpacks"));
    }

    void startsThePackGroupFromTheChosenInstance()
    {
        Setup s;
        QVERIFY(writeText(s.game("start") + "/resourcepacks/Faithful.zip", "faithful"));
        QVERIFY(writeText(s.game("start") + "/options.txt", "fov:0.7\nresourcePacks:[\"file/Faithful.zip\"]\n"));
        Plan plan = packPlan(nullptr);
        auto report = GameSettingsSync::initializeStore(s.store(), s.game("start"), plan);
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(entries(s.store() + "/packs/1.21.1-fabric/resourcepacks"), QStringList({ "Faithful.zip" }));
        QCOMPARE(readText(s.store() + "/packs/1.21.1-fabric/options.txt"), QByteArray("resourcePacks:[\"file/Faithful.zip\"]\n"));

        // A modpack instance's packs are its own, so it starts the group empty.
        Setup m;
        QVERIFY(writeText(m.game("start") + "/resourcepacks/Bundled.zip", "bundled"));
        plan.modpack = true;
        report = GameSettingsSync::initializeStore(m.store(), m.game("start"), plan);
        QVERIFY(!QFileInfo::exists(m.store() + "/packs"));
    }

    void startingAgainReplacesWhatTheNewInstanceLacks()
    {
        Setup s;
        Plan plan = everything();
        plan.sharedConfig = { "xaero" };
        QVERIFY(writeText(s.store() + "/options.txt", "fov:0.8\n"));
        QVERIFY(writeText(s.store() + "/servers.dat", "the old instance's servers"));
        QVERIFY(writeText(s.store() + "/config/xaero/old.txt", "old"));
        QVERIFY(writeText(s.game("new") + "/options.txt", "fov:0.3\n"));
        QVERIFY(writeText(s.game("new") + "/config/xaero/minimap.txt", "zoom:2"));

        const auto report = GameSettingsSync::initializeStore(s.store(), s.game("new"), plan);
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join('\n')));
        QCOMPARE(readText(s.store() + "/options.txt"), QByteArray("fov:0.3\n"));
        QVERIFY(!QFileInfo::exists(s.store() + "/servers.dat"));
        QVERIFY(!QFileInfo::exists(s.store() + "/config/xaero/old.txt"));
        QCOMPARE(readText(s.store() + "/config/xaero/minimap.txt"), QByteArray("zoom:2"));

        // Nothing was deleted: the old copies were set aside.
        const QStringList replaced = QDir(s.store() + "/replaced").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(replaced.size(), 1);
        const QString aside = s.store() + "/replaced/" + replaced.first();
        QCOMPARE(readText(aside + "/servers.dat"), QByteArray("the old instance's servers"));
        QCOMPARE(readText(aside + "/options.txt"), QByteArray("fov:0.8\n"));
        QCOMPARE(readText(aside + "/config/xaero/old.txt"), QByteArray("old"));

        // An instance starting afterwards does not get the old server list.
        s.apply("other");
        QVERIFY(!QFileInfo::exists(s.game("other") + "/servers.dat"));
    }

    void startingAgainFromAnUnreadableInstanceChangesNothing()
    {
#ifdef Q_OS_WIN
        Setup s;
        QVERIFY(writeText(s.store() + "/options.txt", "fov:0.8\n"));
        QVERIFY(writeText(s.store() + "/servers.dat", "servers"));
        QVERIFY(writeText(s.game("locked") + "/options.txt", "fov:0.3\n"));
        // Another program holds the file and lets nobody read it.
        int handle = -1;
        QCOMPARE(_wsopen_s(&handle, reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(s.game("locked") + "/options.txt").utf16()),
                           _O_RDONLY, _SH_DENYRW, 0),
                 0);
        const auto report = GameSettingsSync::initializeStore(s.store(), s.game("locked"), everything());
        _close(handle);
        QVERIFY(!report.errors.isEmpty());
        QCOMPARE(readText(s.store() + "/options.txt"), QByteArray("fov:0.8\n"));
        QCOMPARE(readText(s.store() + "/servers.dat"), QByteArray("servers"));
        QVERIFY(!QFileInfo::exists(s.store() + "/replaced"));
#else
        QSKIP("Holding a file so nobody can read it is a Windows feature.");
#endif
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
