// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>

class BaseInstance;
class MinecraftInstance;

/**
 * Instance settings sync: one instance is the main instance, and instances that follow it
 * get some of its things each time they start. Each follower chooses what it follows:
 *
 *  - game settings: options.txt, merged key by key. A setting only the follower has stays.
 *    Never copied: the file's data version (unless the follower has none), and keybinds
 *    between a pre-1.13 and a 1.13+ instance, which store them differently;
 *  - resource packs: pack files the follower does not have, and which packs are switched on
 *    (that list only between instances on the same side of 1.13);
 *  - shader packs: pack files the follower does not have, each shader's own settings file
 *    in shaderpacks/, and the selected shader (OptiFine, Iris, Oculus);
 *  - mod settings: only the config/ entries the main instance chose to share.
 *
 * Packs are only ever added. Any settings file the sync replaces is saved once, the first
 * time, under the follower's settings-sync-backup folder. Nothing is ever deleted.
 */
namespace GameSettingsSync {

enum class Kind { GameSettings, ResourcePacks, ShaderPacks, ModSettings };

// Which instance is the main one; empty when none is.
QString mainInstanceId();
void setMainInstanceId(const QString& instanceId);

// Whether an instance follows the main instance for one kind. Game settings, resource packs
// and shader packs are followed by default; mod settings are not.
bool follows(BaseInstance* instance, Kind kind);
void setFollows(BaseInstance* instance, Kind kind, bool follow);

// The top-level config/ entries (files or folders) the main instance shares.
QStringList sharedConfigEntries(BaseInstance* mainInstance);
void setSharedConfigEntries(BaseInstance* mainInstance, const QStringList& entries);

// The instance's Minecraft version, loading its components if they are not loaded yet.
QString minecraftVersion(MinecraftInstance* instance);

// Keybinds and the switched-on resource pack list are written the same way only on the same
// side of 1.13 (both before it or both from it).
bool keybindsCompatible(const QString& mainVersion, const QString& followerVersion);

// What to take from the main options.txt.
struct OptionsScope {
    bool gameSettings = true;      // every setting except keybinds and the resource pack list
    bool keybinds = true;          // key_* settings, only together with gameSettings
    bool resourcePackList = false;  // resourcePacks and incompatibleResourcePacks
};

// The follower's options.txt text with the main instance's settings merged in.
QString mergeOptions(const QString& followerText, const QString& mainText, const OptionsScope& scope);

struct Choices {
    bool gameSettings = false;
    bool resourcePacks = false;
    bool shaderPacks = false;
    bool modSettings = false;
    bool sameKeyFormat = false;    // see keybindsCompatible()
    QStringList sharedConfig;      // top-level config/ entries the main instance shares
};

struct Report {
    bool mainHasOptions = false;
    bool optionsChanged = false;
    int resourcePacksAdded = 0;
    int shaderPacksAdded = 0;
    int settingsFilesReplaced = 0;  // shader and mod settings files
    int backupsMade = 0;
    QStringList errors;

    bool changedAnything() const
    {
        return optionsChanged || resourcePacksAdded || shaderPacksAdded || settingsFilesReplaced;
    }
};

// Copies what the choices ask for from mainGameRoot into followerGameRoot. backupRoot keeps
// the follower's own copy of any file the first time it is replaced. Errors are collected,
// not raised: one failed file does not stop the rest.
Report sync(const QString& mainGameRoot, const QString& followerGameRoot, const QString& backupRoot, const Choices& choices);

// The folder an instance's replaced files are saved to.
QString backupRootFor(BaseInstance* instance);

}  // namespace GameSettingsSync
