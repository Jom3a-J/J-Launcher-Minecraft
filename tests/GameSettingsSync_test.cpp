// SPDX-License-Identifier: GPL-3.0-only

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "minecraft/GameSettingsSync.h"

using GameSettingsSync::Choices;
using GameSettingsSync::OptionsScope;

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

OptionsScope settingsOnly(bool keybinds = true)
{
    OptionsScope scope;
    scope.keybinds = keybinds;
    return scope;
}

// A main and a follower instance's game folders, and the follower's backup folder.
struct Instances {
    QTemporaryDir root;
    QString main() const { return root.filePath("main/minecraft"); }
    QString follower() const { return root.filePath("follower/minecraft"); }
    QString backup() const { return root.filePath("follower/settings-sync-backup"); }
};

Choices everything()
{
    Choices choices;
    choices.gameSettings = choices.resourcePacks = choices.shaderPacks = choices.modSettings = true;
    choices.sameKeyFormat = true;
    return choices;
}
}  // namespace

class GameSettingsSyncTest : public QObject {
    Q_OBJECT

   private slots:
    // ---- Game settings (options.txt) ----

    void copiesMainSettingsAndKeepsFollowerOnlyOnes()
    {
        const QString follower = "version:3465\nfov:0.0\nsoundCategory_master:1.0\nfollowerOnly:kept\n";
        const QString main = "version:3955\nfov:0.5\nsoundCategory_master:0.3\nmainOnly:added\n";
        QCOMPARE(GameSettingsSync::mergeOptions(follower, main, settingsOnly()),
                 QString("version:3465\nfov:0.5\nsoundCategory_master:0.3\nfollowerOnly:kept\nmainOnly:added\n"));
    }

    void copiesTheResourcePackListOnlyWhenAsked()
    {
        const QString follower = "resourcePacks:[\"vanilla\"]\nfov:0.0\n";
        const QString main = "resourcePacks:[\"vanilla\",\"file/pack.zip\"]\nincompatibleResourcePacks:[]\nfov:0.5\n";
        QCOMPARE(GameSettingsSync::mergeOptions(follower, main, settingsOnly()), QString("resourcePacks:[\"vanilla\"]\nfov:0.5\n"));

        OptionsScope packsOnly;
        packsOnly.gameSettings = false;
        packsOnly.resourcePackList = true;
        QCOMPARE(GameSettingsSync::mergeOptions(follower, main, packsOnly),
                 QString("resourcePacks:[\"vanilla\",\"file/pack.zip\"]\nfov:0.0\nincompatibleResourcePacks:[]\n"));
    }

    void takesTheDataVersionOnlyWhenTheFollowerHasNone()
    {
        const QString main = "version:3955\nfov:0.5\n";
        QCOMPARE(GameSettingsSync::mergeOptions(QString(), main, settingsOnly()), QString("version:3955\nfov:0.5\n"));
        QCOMPARE(GameSettingsSync::mergeOptions("fov:0.0\n", main, settingsOnly()), QString("fov:0.5\nversion:3955\n"));
        QCOMPARE(GameSettingsSync::mergeOptions("version:1343\n", main, settingsOnly()), QString("version:1343\nfov:0.5\n"));
    }

    void copiesKeybindsOnlyWhenAllowed()
    {
        const QString follower = "key_key.jump:57\nfov:0.0\n";
        const QString main = "key_key.jump:key.keyboard.space\nkey_key.sprint:key.keyboard.left.control\nfov:0.5\n";
        QCOMPARE(GameSettingsSync::mergeOptions(follower, main, settingsOnly(false)), QString("key_key.jump:57\nfov:0.5\n"));
        QCOMPARE(GameSettingsSync::mergeOptions(follower, main, settingsOnly()),
                 QString("key_key.jump:key.keyboard.space\nfov:0.5\nkey_key.sprint:key.keyboard.left.control\n"));
    }

    void keepsValuesThatContainColonsWhole()
    {
        QCOMPARE(GameSettingsSync::mergeOptions("lastServer:old.example:25565\n", "lastServer:mc.example.net:25566\n", settingsOnly()),
                 QString("lastServer:mc.example.net:25566\n"));
    }

    void readsWindowsLineEndings()
    {
        QCOMPARE(GameSettingsSync::mergeOptions("fov:0.0\r\nfollowerOnly:kept\r\n", "fov:0.5\r\n", settingsOnly()),
                 QString("fov:0.5\nfollowerOnly:kept\n"));
    }

    void keybindsAreCompatibleOnTheSameSideOf113()
    {
        QVERIFY(GameSettingsSync::keybindsCompatible("1.20.1", "1.13"));
        QVERIFY(GameSettingsSync::keybindsCompatible("1.8.9", "1.12.2"));
        QVERIFY(!GameSettingsSync::keybindsCompatible("1.12.2", "1.20.1"));
        QVERIFY(!GameSettingsSync::keybindsCompatible("1.21", "1.7.10"));
        QVERIFY(!GameSettingsSync::keybindsCompatible(QString(), "1.20.1"));
    }

    void backsUpTheFollowersOwnSettingsOnce()
    {
        Instances dirs;
        QVERIFY(writeText(dirs.main() + "/options.txt", "fov:0.5\n"));
        QVERIFY(writeText(dirs.follower() + "/options.txt", "fov:0.0\nfollowerOnly:kept\n"));
        Choices choices;
        choices.gameSettings = true;
        choices.sameKeyFormat = true;

        auto report = GameSettingsSync::sync(dirs.main(), dirs.follower(), dirs.backup(), choices);
        QVERIFY(report.optionsChanged);
        QCOMPARE(report.backupsMade, 1);
        QCOMPARE(readText(dirs.follower() + "/options.txt"), QByteArray("fov:0.5\nfollowerOnly:kept\n"));
        QCOMPARE(readText(dirs.backup() + "/options.txt"), QByteArray("fov:0.0\nfollowerOnly:kept\n"));

        report = GameSettingsSync::sync(dirs.main(), dirs.follower(), dirs.backup(), choices);
        QVERIFY(!report.changedAnything());

        // A later change copies again but keeps the first backup.
        QVERIFY(writeText(dirs.main() + "/options.txt", "fov:1.0\n"));
        report = GameSettingsSync::sync(dirs.main(), dirs.follower(), dirs.backup(), choices);
        QVERIFY(report.optionsChanged);
        QCOMPARE(report.backupsMade, 0);
        QCOMPARE(readText(dirs.backup() + "/options.txt"), QByteArray("fov:0.0\nfollowerOnly:kept\n"));
    }

    void createsTheFollowersSettingsWithoutABackup()
    {
        Instances dirs;
        QVERIFY(writeText(dirs.main() + "/options.txt", "version:3955\nfov:0.5\n"));
        QVERIFY(QDir().mkpath(dirs.follower()));
        Choices choices;
        choices.gameSettings = true;

        const auto report = GameSettingsSync::sync(dirs.main(), dirs.follower(), dirs.backup(), choices);
        QVERIFY(report.optionsChanged);
        QCOMPARE(report.backupsMade, 0);
        QVERIFY(!QFileInfo::exists(dirs.backup()));
        QCOMPARE(readText(dirs.follower() + "/options.txt"), QByteArray("version:3955\nfov:0.5\n"));
    }

    void doesNothingWithoutMainSettings()
    {
        Instances dirs;
        QVERIFY(writeText(dirs.follower() + "/options.txt", "fov:0.0\n"));
        const auto report = GameSettingsSync::sync(dirs.main(), dirs.follower(), dirs.backup(), everything());
        QVERIFY(!report.mainHasOptions);
        QVERIFY(!report.changedAnything());
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(readText(dirs.follower() + "/options.txt"), QByteArray("fov:0.0\n"));
    }

    // ---- Resource and shader packs ----

    void addsMissingPacksButNeverReplacesOrRemovesAny()
    {
        Instances dirs;
        QVERIFY(writeText(dirs.main() + "/resourcepacks/Faithful.zip", "main faithful"));
        QVERIFY(writeText(dirs.main() + "/resourcepacks/Shared.zip", "main shared"));
        QVERIFY(writeText(dirs.main() + "/resourcepacks/FolderPack/pack.mcmeta", "{}"));
        QVERIFY(writeText(dirs.follower() + "/resourcepacks/Shared.zip", "follower shared"));
        QVERIFY(writeText(dirs.follower() + "/resourcepacks/OwnPack.zip", "follower own"));
        Choices choices;
        choices.resourcePacks = true;

        const auto report = GameSettingsSync::sync(dirs.main(), dirs.follower(), dirs.backup(), choices);
        QCOMPARE(report.resourcePacksAdded, 2);
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(readText(dirs.follower() + "/resourcepacks/Faithful.zip"), QByteArray("main faithful"));
        QCOMPARE(readText(dirs.follower() + "/resourcepacks/FolderPack/pack.mcmeta"), QByteArray("{}"));
        QCOMPARE(readText(dirs.follower() + "/resourcepacks/Shared.zip"), QByteArray("follower shared"));
        QCOMPARE(readText(dirs.follower() + "/resourcepacks/OwnPack.zip"), QByteArray("follower own"));
        QCOMPARE(QDir(dirs.follower() + "/resourcepacks").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size(), 4);
    }

    void copiesTheSwitchedOnPackListWithThePacks()
    {
        Instances dirs;
        QVERIFY(writeText(dirs.main() + "/resourcepacks/Faithful.zip", "pack"));
        QVERIFY(writeText(dirs.main() + "/options.txt", "fov:0.5\nresourcePacks:[\"vanilla\",\"file/Faithful.zip\"]\n"));
        QVERIFY(writeText(dirs.follower() + "/options.txt", "fov:0.0\nresourcePacks:[\"vanilla\"]\n"));
        Choices choices;
        choices.resourcePacks = true;
        choices.sameKeyFormat = true;

        auto report = GameSettingsSync::sync(dirs.main(), dirs.follower(), dirs.backup(), choices);
        QVERIFY(report.optionsChanged);
        QCOMPARE(readText(dirs.follower() + "/options.txt"), QByteArray("fov:0.0\nresourcePacks:[\"vanilla\",\"file/Faithful.zip\"]\n"));

        // Across 1.13 the list is written differently: the files still come, the list does not.
        Instances other;
        QVERIFY(writeText(other.main() + "/resourcepacks/Faithful.zip", "pack"));
        QVERIFY(writeText(other.main() + "/options.txt", "resourcePacks:[\"vanilla\",\"file/Faithful.zip\"]\n"));
        QVERIFY(writeText(other.follower() + "/options.txt", "resourcePacks:[\"Faithful.zip\"]\n"));
        choices.sameKeyFormat = false;
        report = GameSettingsSync::sync(other.main(), other.follower(), other.backup(), choices);
        QCOMPARE(report.resourcePacksAdded, 1);
        QVERIFY(!report.optionsChanged);
        QCOMPARE(readText(other.follower() + "/options.txt"), QByteArray("resourcePacks:[\"Faithful.zip\"]\n"));
    }

    void copiesShaderPacksAndTheirSettings()
    {
        Instances dirs;
        QVERIFY(writeText(dirs.main() + "/shaderpacks/BSL.zip", "bsl"));
        QVERIFY(writeText(dirs.main() + "/shaderpacks/BSL.zip.txt", "SHADOW=true\n"));
        QVERIFY(writeText(dirs.main() + "/config/iris.properties", "shaderPack=BSL.zip\n"));
        QVERIFY(writeText(dirs.main() + "/optionsshaders.txt", "shaderPack=BSL.zip\n"));
        QVERIFY(writeText(dirs.follower() + "/shaderpacks/BSL.zip.txt", "SHADOW=false\n"));
        QVERIFY(writeText(dirs.follower() + "/config/iris.properties", "shaderPack=\n"));
        Choices choices;
        choices.shaderPacks = true;

        const auto report = GameSettingsSync::sync(dirs.main(), dirs.follower(), dirs.backup(), choices);
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(report.shaderPacksAdded, 1);
        QCOMPARE(report.settingsFilesReplaced, 3);
        QCOMPARE(report.backupsMade, 2);
        QCOMPARE(readText(dirs.follower() + "/shaderpacks/BSL.zip"), QByteArray("bsl"));
        QCOMPARE(readText(dirs.follower() + "/shaderpacks/BSL.zip.txt"), QByteArray("SHADOW=true\n"));
        QCOMPARE(readText(dirs.follower() + "/config/iris.properties"), QByteArray("shaderPack=BSL.zip\n"));
        QCOMPARE(readText(dirs.follower() + "/optionsshaders.txt"), QByteArray("shaderPack=BSL.zip\n"));
        QCOMPARE(readText(dirs.backup() + "/shaderpacks/BSL.zip.txt"), QByteArray("SHADOW=false\n"));
        QCOMPARE(readText(dirs.backup() + "/config/iris.properties"), QByteArray("shaderPack=\n"));
    }

    // ---- Mod settings ----

    void copiesOnlyTheSharedConfigEntries()
    {
        Instances dirs;
        QVERIFY(writeText(dirs.main() + "/config/sodium-options.json", "{\"quality\":1}"));
        QVERIFY(writeText(dirs.main() + "/config/xaero/minimap.txt", "zoom:2"));
        QVERIFY(writeText(dirs.main() + "/config/xaero/sub/waypoints.txt", "home"));
        QVERIFY(writeText(dirs.main() + "/config/modpack-balance.toml", "hard=true"));
        QVERIFY(writeText(dirs.follower() + "/config/sodium-options.json", "{\"quality\":0}"));
        QVERIFY(writeText(dirs.follower() + "/config/modpack-balance.toml", "hard=false"));
        QVERIFY(writeText(dirs.follower() + "/config/xaero/own.txt", "kept"));
        Choices choices;
        choices.modSettings = true;
        choices.sharedConfig = { "sodium-options.json", "xaero", "../escape", "missing.json" };

        const auto report = GameSettingsSync::sync(dirs.main(), dirs.follower(), dirs.backup(), choices);
        QVERIFY(report.errors.isEmpty());
        QCOMPARE(report.settingsFilesReplaced, 3);
        QCOMPARE(report.backupsMade, 1);
        QCOMPARE(readText(dirs.follower() + "/config/sodium-options.json"), QByteArray("{\"quality\":1}"));
        QCOMPARE(readText(dirs.follower() + "/config/xaero/minimap.txt"), QByteArray("zoom:2"));
        QCOMPARE(readText(dirs.follower() + "/config/xaero/sub/waypoints.txt"), QByteArray("home"));
        QCOMPARE(readText(dirs.follower() + "/config/xaero/own.txt"), QByteArray("kept"));
        QCOMPARE(readText(dirs.follower() + "/config/modpack-balance.toml"), QByteArray("hard=false"));
        QCOMPARE(readText(dirs.backup() + "/config/sodium-options.json"), QByteArray("{\"quality\":0}"));
    }

    void copiesNothingThatIsNotChosen()
    {
        Instances dirs;
        QVERIFY(writeText(dirs.main() + "/options.txt", "fov:0.5\n"));
        QVERIFY(writeText(dirs.main() + "/resourcepacks/Faithful.zip", "pack"));
        QVERIFY(writeText(dirs.main() + "/shaderpacks/BSL.zip", "bsl"));
        QVERIFY(writeText(dirs.main() + "/config/sodium-options.json", "{}"));
        QVERIFY(QDir().mkpath(dirs.follower()));
        Choices choices;
        choices.sharedConfig = { "sodium-options.json" };

        const auto report = GameSettingsSync::sync(dirs.main(), dirs.follower(), dirs.backup(), choices);
        QVERIFY(!report.changedAnything());
        QCOMPARE(QDir(dirs.follower()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size(), 0);
    }
};

QTEST_GUILESS_MAIN(GameSettingsSyncTest)

#include "GameSettingsSync_test.moc"
