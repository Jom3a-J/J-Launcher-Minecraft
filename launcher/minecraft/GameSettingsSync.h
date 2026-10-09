// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QSet>
#include <functional>
#include <iterator>
#include <QString>
#include <QStringList>

class BaseInstance;
class MinecraftInstance;

/**
 * Sync between instances, in the style of the Modrinth App.
 *
 * The launcher keeps one shared copy of the synced things in <data>/sync. When a game starts,
 * the shared copy is put into the instance; when the game closes, whatever changed during
 * that session goes back into the shared copy. Changes therefore travel in both directions,
 * and two games open at the same time each save only what they changed.
 *
 * What is synced:
 *  - game settings: chosen options.txt settings. Settings that mods add only go to instances
 *    that already have them, i.e. that have the mod. Keybinds are not synced with instances
 *    before 1.13, which store them differently;
 *  - the multiplayer server list (servers.dat), command history and creative hotbars (hotbars,
 *    like keybinds, only from 1.13 on);
 *  - resource packs and shader packs (off by default): only within a group of instances with the
 *    same Minecraft version and mod loader. Packs added in one instance are copied into the
 *    others; packs deleted in one are moved to the Recycle Bin in the others; which resource packs
 *    are switched on and their order, the selected shader and each shader's settings follow.
 *    Packs a modpack instance already had when it joined pack sync stay its own;
 *  - mod settings (off by default): chosen config/ entries, only to instances that have them.
 *
 * An instance can opt out of each kind and keep its own value for chosen settings. Any file the
 * sync replaces in an instance is saved once, the first time, under its settings-sync-backup
 * folder. Nothing is ever deleted.
 */
namespace GameSettingsSync {

enum class Kind { GameSettings, Servers, CommandHistory, Hotbars, ResourcePacks, ShaderPacks, ModSettings };
// In the order the Sync pages show them.
constexpr Kind AllKinds[] = { Kind::GameSettings, Kind::Servers,       Kind::CommandHistory, Kind::Hotbars,
                              Kind::ResourcePacks, Kind::ShaderPacks, Kind::ModSettings };
constexpr int KindCount = int(std::size(AllKinds));

// ---- Launcher-wide choices (Settings → Sync) ----

bool enabled();
void setEnabled(bool on);
bool syncs(Kind kind);  // game settings, servers, command history and hotbars default on; packs and mod settings off
void setSyncs(Kind kind, bool on);
QStringList excludedOptionKeys();  // game settings left out of the sync
void setExcludedOptionKeys(const QStringList& keys);
bool syncsModOptions();  // options.txt settings that mods add
void setSyncsModOptions(bool on);
QStringList sharedConfigEntries();  // top-level config/ entries synced as mod settings
void setSharedConfigEntries(const QStringList& entries);
QString storeRoot();

// ---- Per instance (the instance's Sync page) ----

bool instanceUsesSync(BaseInstance* instance);
void setInstanceUsesSync(BaseInstance* instance, bool on);
bool instanceUses(BaseInstance* instance, Kind kind);
void setInstanceUses(BaseInstance* instance, Kind kind, bool on);
QStringList ownOptionKeys(BaseInstance* instance);  // settings this instance keeps as its own
void setOwnOptionKeys(BaseInstance* instance, const QStringList& keys);
QString backupRootFor(BaseInstance* instance);
QString launchStateFor(BaseInstance* instance);  // what the instance looked like after the last apply

// ---- Minecraft facts ----

QString minecraftVersion(MinecraftInstance* instance);  // loads the components if needed
bool usesNamedKeys(const QString& minecraftVersion);   // 1.13 and later name keys ("key.keyboard.w")
// The pack group: instances with the same Minecraft version and mod loader, e.g. "1.21.1-fabric".
// Empty when the version is unknown, and then packs are not synced.
QString packGroupFor(MinecraftInstance* instance);
QString packGroupName(const QString& minecraftVersion, const QString& loader);
bool isVanillaOption(const QString& key);
QStringList vanillaOptionKeys();         // the settings offered in Settings → Sync, sorted
QStringList defaultExcludedOptionKeys();  // technical or one-off settings left out by default

// ---- The rules, on text and folders ----

struct OptionRules {
    QSet<QString> excluded;  // left out everywhere, plus the ones this instance keeps as its own
    bool modOptions = true;  // also sync settings mods add
    bool namedKeys = true;   // the instance is 1.13+; keybinds are only synced with those
    // Only the switched-on resource pack list (resourcePacks, incompatibleResourcePacks), which
    // is shared per pack group rather than with all instances.
    bool packListOnly = false;
};

// The instance's options.txt with the shared settings put in.
QString applyOptions(const QString& instanceText, const QString& storeText, const OptionRules& rules);
// The shared settings with what changed in the instance since launch put in.
QString collectOptions(const QString& storeText, const QString& instanceNow, const QString& instanceAtLaunch, const OptionRules& rules);

struct Plan {
    bool gameSettings = false;
    bool servers = false;
    bool commandHistory = false;
    bool hotbars = false;
    bool modSettings = false;
    bool resourcePacks = false;
    bool shaderPacks = false;
    OptionRules options;
    QStringList sharedConfig;
    QString packGroup;     // see packGroupFor(); packs are only synced when it is set
    bool modpack = false;  // the packs it has when it joins pack sync stay its own
    // How a pack deleted in another instance leaves this one. Default: the Recycle Bin.
    std::function<bool(const QString& path)> removeFile;
};

struct Report {
    QStringList changed;  // what was put into the instance, or saved to the shared copy
    int backupsMade = 0;
    QStringList errors;
    QString session;  // applyToInstance: set once this session's record is saved; collect needs it
};

// When a game starts: puts the shared copy into the instance and records the result in statePath.
Report applyToInstance(const QString& storeRoot, const QString& gameRoot, const QString& backupRoot, const QString& statePath,
                       const Plan& plan);
// When the game has closed: saves what changed since applyToInstance into the shared copy. Only with
// the session applyToInstance returned, and only against that session's own record.
Report collectFromInstance(const QString& storeRoot, const QString& gameRoot, const QString& statePath, const Plan& plan,
                           const QString& session);
// The starting point: replaces the shared copy with this instance's things.
Report initializeStore(const QString& storeRoot, const QString& gameRoot, const Plan& plan);

// The plan for an instance from the launcher-wide and per-instance choices.
Plan planFor(MinecraftInstance* instance);
// The plan for using an instance as the starting point: the launcher-wide choices only.
Plan startingPlanFor(MinecraftInstance* instance);

}  // namespace GameSettingsSync
