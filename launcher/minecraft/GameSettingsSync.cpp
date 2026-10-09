// SPDX-License-Identifier: GPL-3.0-only

#include "GameSettingsSync.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "Application.h"
#include "BaseInstance.h"
#include "FileSystem.h"
#include "Version.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "settings/Setting.h"
#include "settings/SettingsObject.h"

namespace GameSettingsSync {

namespace {
const QString OptionsFile = QStringLiteral("options.txt");
const QString ConfigFolder = QStringLiteral("config");
const QString DataVersionKey = QStringLiteral("version");
const QChar ListSeparator = QLatin1Char('|');  // cannot appear in a Windows file name or an options key

struct WholeFile {
    Kind kind;
    const char* path;
    const char* name;  // for the log
};
const WholeFile WholeFiles[] = {
    { Kind::Servers, "servers.dat", QT_TRANSLATE_NOOP("SyncGameSettings", "server list") },
    { Kind::CommandHistory, "command_history.txt", QT_TRANSLATE_NOOP("SyncGameSettings", "command history") },
    { Kind::Hotbars, "hotbar.nbt", QT_TRANSLATE_NOOP("SyncGameSettings", "creative hotbars") },
};

// Packs are shared per pack group, under <store>/packs/<group>.
const QString PacksFolder = QStringLiteral("packs");
const QString GroupStateFile = QStringLiteral("sync.json");     // packs deleted in some instance
const QString LocalPacksFile = QStringLiteral("local-packs.json");  // a modpack's own packs, next to the launch record
const QString PartialSuffix = QStringLiteral(".jlsync-part");   // a copy in progress, renamed once complete
struct PackFolder {
    Kind kind;
    const char* folder;
};
const PackFolder PackFolders[] = { { Kind::ResourcePacks, "resourcepacks" }, { Kind::ShaderPacks, "shaderpacks" } };
// Shader loaders keep the selected shader in these; each shader's own settings sit next to it
// as shaderpacks/<pack>.txt.
const char* const ShaderSettingsFiles[] = { "optionsshaders.txt", "config/iris.properties", "config/oculus.properties" };

// ---- Settings storage ----

QString globalKey(Kind kind)
{
    switch (kind) {
        case Kind::GameSettings:
            return QStringLiteral("SyncGameSettings");
        case Kind::Servers:
            return QStringLiteral("SyncServers");
        case Kind::CommandHistory:
            return QStringLiteral("SyncCommandHistory");
        case Kind::Hotbars:
            return QStringLiteral("SyncHotbars");
        case Kind::ResourcePacks:
            return QStringLiteral("SyncResourcePacks");
        case Kind::ShaderPacks:
            return QStringLiteral("SyncShaderPacks");
        case Kind::ModSettings:
            return QStringLiteral("SyncModSettings");
    }
    return {};
}

// Packs and mod settings copy whole files between instances, so they are opt-in.
bool onByDefault(Kind kind)
{
    return kind == Kind::GameSettings || kind == Kind::Servers || kind == Kind::CommandHistory || kind == Kind::Hotbars;
}

std::shared_ptr<Setting> globalSetting(const QString& key, const QVariant& defaultValue)
{
    return APPLICATION->settings()->getOrRegisterSetting(key, defaultValue);
}

std::shared_ptr<Setting> instanceSetting(BaseInstance* instance, const QString& key, const QVariant& defaultValue)
{
    return instance->settings()->getOrRegisterSetting(key, defaultValue);
}

QStringList splitList(const QString& stored)
{
    return stored.split(ListSeparator, Qt::SkipEmptyParts);
}

QString joinList(const QStringList& list)
{
    QStringList kept;
    for (const QString& item : list) {
        const QString trimmed = item.trimmed();
        if (!trimmed.isEmpty() && !trimmed.contains(ListSeparator) && !kept.contains(trimmed))
            kept << trimmed;
    }
    return kept.join(ListSeparator);
}

// ---- options.txt ----

struct Options {
    QStringList keys;              // in file order
    QHash<QString, QString> lines;  // key -> whole "key:value" line
    QStringList other;             // lines without a key, kept as they are
};

// options.txt is one "key:value" per line. Keys never contain ':', values may (server addresses).
QString keyOf(const QString& line)
{
    const qsizetype colon = line.indexOf(QLatin1Char(':'));
    return colon > 0 ? line.left(colon) : QString();
}

QStringList splitLines(const QString& text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
    }
    if (!lines.isEmpty() && lines.constLast().isEmpty())
        lines.removeLast();
    return lines;
}

Options parseOptions(const QString& text)
{
    Options options;
    for (const QString& line : splitLines(text)) {
        const QString key = keyOf(line);
        if (key.isEmpty()) {
            options.other << line;
        } else if (!options.lines.contains(key)) {
            options.keys << key;
            options.lines.insert(key, line);
        }
    }
    return options;
}

QString joinLines(const QStringList& lines)
{
    return lines.isEmpty() ? QString() : lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

bool isResourcePackKey(const QString& key)
{
    return key == QStringLiteral("resourcePacks") || key == QStringLiteral("incompatibleResourcePacks");
}

// Settings that are never synced with all instances: the data version (how to read the file) and
// the resource pack list, which is shared per pack group instead.
bool neverSynced(const QString& key)
{
    return key == DataVersionKey || isResourcePackKey(key);
}

bool isKeybind(const QString& key)
{
    return key.startsWith(QStringLiteral("key_"));
}

// Whether a setting moves between this instance and the shared copy. A setting a mod adds only
// goes into instances that already have it.
bool synced(const QString& key, const OptionRules& rules, bool instanceHasKey)
{
    if (rules.packListOnly)
        return isResourcePackKey(key);
    if (neverSynced(key) || rules.excluded.contains(key))
        return false;
    if (isKeybind(key) && !rules.namedKeys)
        return false;
    if (isVanillaOption(key))
        return true;
    return rules.modOptions && instanceHasKey;
}

// ---- Files ----

QString sha1Of(const QString& path)
{
    if (!QFileInfo(path).isFile())
        return {};
    auto data = FS::read(path);
    return data ? QString::fromLatin1(QCryptographicHash::hash(*data, QCryptographicHash::Sha1).toHex()) : QString();
}

// Writes data to gameRoot/relativePath, saving the instance's own copy under backupRoot the
// first time. Returns whether anything changed.
bool writeWithBackup(const QString& relativePath, const QByteArray& data, const QString& gameRoot, const QString& backupRoot,
                     Report& report)
{
    const QString path = QDir(gameRoot).filePath(relativePath);
    if (QFileInfo(path).isFile()) {
        auto current = FS::read(path);
        if (!current) {
            report.errors << current.error();
            return false;
        }
        if (*current == data)
            return false;
        const QString backupPath = QDir(backupRoot).filePath(relativePath);
        if (!QFileInfo::exists(backupPath)) {
            if (auto written = FS::write(backupPath, *current); !written) {
                report.errors << written.error();
                return false;
            }
            ++report.backupsMade;
        }
    }
    if (auto written = FS::write(path, data); !written) {
        report.errors << written.error();
        return false;
    }
    return true;
}

// Copies fromRoot/relativePath to toRoot/relativePath. Returns whether it copied.
bool copyFile(const QString& relativePath, const QString& fromRoot, const QString& toRoot, Report& report)
{
    auto data = FS::read(QDir(fromRoot).filePath(relativePath));
    if (!data) {
        report.errors << data.error();
        return false;
    }
    if (auto written = FS::write(QDir(toRoot).filePath(relativePath), *data); !written) {
        report.errors << written.error();
        return false;
    }
    return true;
}

bool isPlainEntryName(const QString& name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..") && !name.contains(QLatin1Char('/')) &&
           !name.contains(QLatin1Char('\\')) && !name.contains(QLatin1Char(':'));
}

// The files of one config/ entry, relative to the game folder.
QStringList configFiles(const QString& root, const QString& entry)
{
    const QString relativeEntry = ConfigFolder + QLatin1Char('/') + entry;
    const QFileInfo info(QDir(root).filePath(relativeEntry));
    if (info.isFile())
        return { relativeEntry };
    QStringList files;
    if (info.isDir()) {
        const QDir base(root);
        QDirIterator it(info.absoluteFilePath(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext())
            files << base.relativeFilePath(it.next());
    }
    files.sort();
    return files;
}

QString readText(const QString& path, Report& report)
{
    if (!QFileInfo(path).isFile())
        return {};
    auto data = FS::read(path);
    if (!data) {
        report.errors << data.error();
        return {};
    }
    return QString::fromUtf8(*data);
}

QString kindName(const WholeFile& file)
{
    return QCoreApplication::translate("SyncGameSettings", file.name);
}

bool planned(const Plan& plan, Kind kind)
{
    switch (kind) {
        case Kind::GameSettings:
            return plan.gameSettings;
        case Kind::Servers:
            return plan.servers;
        case Kind::CommandHistory:
            return plan.commandHistory;
        case Kind::Hotbars:
            // Hotbars hold items, which Minecraft before 1.13 names differently and cannot read
            // from a newer file; like keybinds, they are only synced from 1.13 on.
            return plan.hotbars && plan.options.namedKeys;
        case Kind::ResourcePacks:
            return plan.resourcePacks && !plan.packGroup.isEmpty();
        case Kind::ShaderPacks:
            return plan.shaderPacks && !plan.packGroup.isEmpty();
        case Kind::ModSettings:
            return plan.modSettings;
    }
    return false;
}

// ---- Packs ----

QJsonObject readJson(const QString& path)
{
    if (!QFileInfo(path).isFile())
        return {};
    auto data = FS::read(path);
    return data ? QJsonDocument::fromJson(*data).object() : QJsonObject();
}

QStringList toStringList(const QJsonValue& value)
{
    QStringList list;
    for (const QJsonValue& item : value.toArray())
        list << item.toString();
    return list;
}

QString groupRoot(const QString& storeRoot, const Plan& plan)
{
    return QDir(storeRoot).filePath(PacksFolder + QLatin1Char('/') + plan.packGroup);
}

bool isShaderSettingsName(const QString& folder, const QFileInfo& entry)
{
    return folder == QStringLiteral("shaderpacks") && entry.isFile() &&
           entry.suffix().compare(QStringLiteral("txt"), Qt::CaseInsensitive) == 0;
}

// The packs (files and folders) in a pack folder. A shader's own settings file is not a pack.
QStringList packNames(const QString& folderPath, const QString& folder)
{
    QStringList names;
    const auto entries = QDir(folderPath).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& entry : entries) {
        if (!entry.fileName().endsWith(PartialSuffix) && !isShaderSettingsName(folder, entry))
            names << entry.fileName();
    }
    return names;
}

// Copies the pack `name` from one pack folder to another, through a temporary name, so a pack
// is only ever there complete. Never replaces anything.
bool copyPack(const QString& fromFolder, const QString& toFolder, const QString& name, Report& report)
{
    const QFileInfo source(QDir(fromFolder).filePath(name));
    const QString target = QDir(toFolder).filePath(name);
    if (QFileInfo::exists(target))
        return false;
    if (!FS::ensureFolderPathExists(toFolder)) {
        report.errors << QStringLiteral("Could not create %1").arg(toFolder);
        return false;
    }
    const QString partial = target + PartialSuffix;
    if (QFileInfo::exists(partial))
        FS::deletePath(partial);
    const bool copied = source.isDir() ? FS::copy(source.absoluteFilePath(), partial)() : QFile::copy(source.absoluteFilePath(), partial);
    if (!copied || !QDir().rename(partial, target)) {
        FS::deletePath(partial);
        report.errors << QStringLiteral("Could not copy %1").arg(name);
        return false;
    }
    return true;
}

bool removePack(const Plan& plan, const QString& path)
{
    return plan.removeFile ? plan.removeFile(path) : FS::trash(path);
}

// The shader settings files to sync: the fixed ones, plus each shader's own settings file
// ("<pack>.txt"), except those of shaders that stay an instance's own.
QStringList shaderSettingsFiles(const QString& root, const QStringList& ownShaders = {})
{
    QStringList files;
    for (const char* file : ShaderSettingsFiles)
        files << QString::fromLatin1(file);
    const QDir shaders(QDir(root).filePath(QStringLiteral("shaderpacks")));
    for (const QFileInfo& entry : shaders.entryInfoList(QDir::Files, QDir::Name)) {
        if (isShaderSettingsName(QStringLiteral("shaderpacks"), entry) && !ownShaders.contains(entry.completeBaseName()))
            files << QStringLiteral("shaderpacks/") + entry.fileName();
    }
    return files;
}

QString countText(const char* text, int count)
{
    return QCoreApplication::translate("SyncGameSettings", text, nullptr, count);
}
}  // namespace

// ---- Launcher-wide choices ----

bool enabled()
{
    return globalSetting(QStringLiteral("SyncEnabled"), false)->get().toBool();
}

void setEnabled(bool on)
{
    globalSetting(QStringLiteral("SyncEnabled"), false)->set(on);
}

bool syncs(Kind kind)
{
    return globalSetting(globalKey(kind), onByDefault(kind))->get().toBool();
}

void setSyncs(Kind kind, bool on)
{
    globalSetting(globalKey(kind), onByDefault(kind))->set(on);
}

QStringList excludedOptionKeys()
{
    return splitList(globalSetting(QStringLiteral("SyncExcludedOptions"), joinList(defaultExcludedOptionKeys()))->get().toString());
}

void setExcludedOptionKeys(const QStringList& keys)
{
    globalSetting(QStringLiteral("SyncExcludedOptions"), joinList(defaultExcludedOptionKeys()))->set(joinList(keys));
}

bool syncsModOptions()
{
    return globalSetting(QStringLiteral("SyncModOptions"), true)->get().toBool();
}

void setSyncsModOptions(bool on)
{
    globalSetting(QStringLiteral("SyncModOptions"), true)->set(on);
}

QStringList sharedConfigEntries()
{
    return splitList(globalSetting(QStringLiteral("SyncSharedConfig"), QString())->get().toString());
}

void setSharedConfigEntries(const QStringList& entries)
{
    QStringList kept;
    for (const QString& entry : entries) {
        if (isPlainEntryName(entry))
            kept << entry;
    }
    globalSetting(QStringLiteral("SyncSharedConfig"), QString())->set(joinList(kept));
}

QString storeRoot()
{
    return FS::PathCombine(APPLICATION->dataRoot(), QStringLiteral("sync"));
}

// ---- Per instance ----

bool instanceUsesSync(BaseInstance* instance)
{
    return instanceSetting(instance, QStringLiteral("SyncUse"), true)->get().toBool();
}

void setInstanceUsesSync(BaseInstance* instance, bool on)
{
    instanceSetting(instance, QStringLiteral("SyncUse"), true)->set(on);
}

bool instanceUses(BaseInstance* instance, Kind kind)
{
    return instanceSetting(instance, QStringLiteral("SyncUse") + globalKey(kind).mid(4), true)->get().toBool();
}

void setInstanceUses(BaseInstance* instance, Kind kind, bool on)
{
    instanceSetting(instance, QStringLiteral("SyncUse") + globalKey(kind).mid(4), true)->set(on);
}

QStringList ownOptionKeys(BaseInstance* instance)
{
    return splitList(instanceSetting(instance, QStringLiteral("SyncOwnOptions"), QString())->get().toString());
}

void setOwnOptionKeys(BaseInstance* instance, const QStringList& keys)
{
    instanceSetting(instance, QStringLiteral("SyncOwnOptions"), QString())->set(joinList(keys));
}

QString backupRootFor(BaseInstance* instance)
{
    return QDir(instance->instanceRoot()).filePath(QStringLiteral("settings-sync-backup"));
}

QString launchStateFor(BaseInstance* instance)
{
    return QDir(instance->instanceRoot()).filePath(QStringLiteral(".jlsync/launch-state.json"));
}

// ---- Minecraft facts ----

QString minecraftVersion(MinecraftInstance* instance)
{
    auto* profile = instance->getPackProfile();
    QString version = profile->getComponentVersion(QStringLiteral("net.minecraft"));
    if (version.isEmpty()) {
        // Same as the status bar: load the component list from disk if nothing has yet.
        if (auto result = profile->reload(Net::Mode::Offline); !result) {
            qWarning() << "Sync: could not load the components of" << instance->name() << ":" << result.error();
        }
        version = profile->getComponentVersion(QStringLiteral("net.minecraft"));
    }
    return version;
}

bool usesNamedKeys(const QString& minecraftVersion)
{
    // 1.13 replaced numeric key codes with names such as "key.keyboard.w". Unknown counts as old,
    // so keybinds are never written in a form the game cannot read.
    return !minecraftVersion.isEmpty() && !(Version(minecraftVersion) < Version(QStringLiteral("1.13")));
}

bool isVanillaOption(const QString& key)
{
    static const QSet<QString> keys = [] {
        const QStringList list = vanillaOptionKeys();
        return QSet<QString>(list.begin(), list.end());
    }();
    if (keys.contains(key))
        return true;
    // Families whose members vary with the version.
    return key.startsWith(QStringLiteral("soundCategory_")) || key.startsWith(QStringLiteral("modelPart_")) ||
           key.startsWith(QStringLiteral("key_key.hotbar."));
}

QStringList vanillaOptionKeys()
{
    // Minecraft's own options.txt settings from 1.8 to 1.21, including some that were renamed.
    static const QStringList keys = [] {
        QStringList list{
            // General, video, chat, accessibility
            "accessibilityTextBackground", "advancedItemTooltips", "allowServerListing", "ao", "attackIndicator", "autoJump",
            "autoSuggestions", "backgroundForChatOnly", "biomeBlendRadius", "bobView", "chatColors", "chatDelay", "chatHeightFocused",
            "chatHeightUnfocused", "chatLineSpacing", "chatLinks", "chatLinksPrompt", "chatOpacity", "chatScale", "chatVisibility",
            "chatWidth", "cloudRange", "clouds", "damageTiltStrength", "darkMojangStudiosBackground", "darknessEffectScale",
            "directionalAudio", "discrete_mouse_scroll", "enableVsync", "enableWeakAttacks", "entityDistanceScaling", "entityShadows",
            "fancyGraphics", "forceUnicodeFont", "fov", "fovEffectScale", "fullscreen", "gamma", "glDebugVerbosity", "glintSpeed",
            "glintStrength", "graphicsMode", "guiScale", "heldItemTooltips", "hideBundleTutorial", "hideLightningFlashes",
            "hideMatchedNames", "hideServerAddress", "hideSplashTexts", "highContrast", "highContrastBlockOutline",
            "inactivityFpsLimit", "invertYMouse", "japaneseGlyphVariants", "joinedFirstServer", "lang", "lastServer", "mainHand",
            "maxFps", "menuBackgroundBlurriness", "mipmapLevels", "mouseSensitivity", "mouseWheelSensitivity", "musicFrequency",
            "narrator", "narratorHotkey", "notificationDisplayTime", "onboardAccessibility", "onlyShowSecureChat", "operatorItemsTab",
            "overrideHeight", "overrideWidth", "panoramaScrollSpeed", "particles", "pauseOnLostFocus", "prioritizeChunkUpdates",
            "rawMouseInput", "realmsNotifications", "reducedDebugInfo", "renderClouds", "renderDistance", "rotateWithMinecart",
            "screenEffectScale", "showAutosaveIndicator", "showNowPlayingToast", "showSubtitles", "simulationDistance",
            "skipMultiplayerWarning", "skipRealms32bitWarning", "snooperEnabled", "soundDevice", "startedCleanly", "syncChunkWrites",
            "telemetryOptInExtra", "textBackgroundOpacity", "toggleCrouch", "toggleSprint", "touchscreen", "tutorialStep",
            "useNativeTransport", "useVbo",
            // Minecraft's own keybinds
            "key_key.advancements", "key_key.attack", "key_key.back", "key_key.chat", "key_key.command", "key_key.drop",
            "key_key.forward", "key_key.fullscreen", "key_key.inventory", "key_key.jump", "key_key.left", "key_key.loadToolbarActivator",
            "key_key.pickItem", "key_key.playerlist", "key_key.right", "key_key.saveToolbarActivator", "key_key.screenshot",
            "key_key.smoothCamera", "key_key.sneak", "key_key.socialInteractions", "key_key.spectatorOutlines", "key_key.sprint",
            "key_key.swapOffhand", "key_key.togglePerspective", "key_key.use",
            // Volumes and skin parts
            "soundCategory_master", "soundCategory_music", "soundCategory_record", "soundCategory_weather", "soundCategory_block",
            "soundCategory_hostile", "soundCategory_neutral", "soundCategory_player", "soundCategory_ambient", "soundCategory_voice",
            "modelPart_cape", "modelPart_jacket", "modelPart_left_sleeve", "modelPart_right_sleeve", "modelPart_left_pants_leg",
            "modelPart_right_pants_leg", "modelPart_hat",
        };
        for (int slot = 1; slot <= 9; ++slot)
            list << QStringLiteral("key_key.hotbar.%1").arg(slot);
        list.sort();
        return list;
    }();
    return keys;
}

QStringList defaultExcludedOptionKeys()
{
    // One-off states, window sizes and technical switches: syncing them is rarely what anyone wants.
    return { "glDebugVerbosity", "hideBundleTutorial", "joinedFirstServer", "lastServer", "onboardAccessibility", "overrideHeight",
             "overrideWidth", "skipMultiplayerWarning", "skipRealms32bitWarning", "snooperEnabled", "startedCleanly",
             "syncChunkWrites", "telemetryOptInExtra", "tutorialStep", "useNativeTransport" };
}

// ---- The rules ----

QString applyOptions(const QString& instanceText, const QString& storeText, const OptionRules& rules)
{
    const Options instance = parseOptions(instanceText);
    const Options store = parseOptions(storeText);

    QStringList result;
    bool tookAny = false;
    for (const QString& line : splitLines(instanceText)) {
        const QString key = keyOf(line);
        const auto storeLine = store.lines.constFind(key);
        if (!key.isEmpty() && storeLine != store.lines.constEnd() && synced(key, rules, true) && instance.lines.value(key) == line) {
            result << *storeLine;
            tookAny = tookAny || *storeLine != line;
        } else {
            result << line;
        }
    }
    QStringList added;
    for (const QString& key : store.keys) {
        if (!instance.lines.contains(key) && synced(key, rules, false))
            added << store.lines.value(key);
    }
    // The data version tells Minecraft how to read the rest. An instance keeps its own; one
    // without any takes the shared copy's, which matches the lines it receives.
    if (!added.isEmpty() && !instance.lines.contains(DataVersionKey) && store.lines.contains(DataVersionKey))
        added.prepend(store.lines.value(DataVersionKey));
    if (!tookAny && added.isEmpty())
        return instanceText;
    result << added;
    return joinLines(result);
}

QString collectOptions(const QString& storeText, const QString& instanceNow, const QString& instanceAtLaunch, const OptionRules& rules)
{
    Options store = parseOptions(storeText);
    const Options now = parseOptions(instanceNow);
    const Options atLaunch = parseOptions(instanceAtLaunch);

    bool changed = false;
    for (const QString& key : now.keys) {
        // Only what changed during this session, so two games open at once keep each other's
        // changes; and whatever the shared copy does not have yet.
        const QString line = now.lines.value(key);
        if (!synced(key, rules, true) || store.lines.value(key) == line)
            continue;
        if (store.lines.contains(key) && atLaunch.lines.value(key) == line)
            continue;
        if (!store.lines.contains(key))
            store.keys << key;
        store.lines.insert(key, line);
        changed = true;
    }
    // The shared copy needs a data version to go with the lines it hands to instances without one.
    if (changed && !store.lines.contains(DataVersionKey) && now.lines.contains(DataVersionKey)) {
        store.keys.prepend(DataVersionKey);
        store.lines.insert(DataVersionKey, now.lines.value(DataVersionKey));
        changed = true;
    }
    if (!changed)
        return storeText;
    QStringList lines;
    for (const QString& key : store.keys)
        lines << store.lines.value(key);
    lines << store.other;
    return joinLines(lines);
}

Report applyToInstance(const QString& storeRoot, const QString& gameRoot, const QString& backupRoot, const QString& statePath,
                       const Plan& plan)
{
    Report report;
    // The record of an earlier session must never be compared with this one: if the new record
    // cannot be written, closing the game would treat values sync put in as the player's changes.
    if (QFileInfo::exists(statePath) && !QFile::remove(statePath)) {
        report.errors << QStringLiteral("Could not replace the sync record %1").arg(statePath);
        return report;
    }

    QJsonObject files;
    QJsonObject packs;
    const bool anyPacks = planned(plan, Kind::ResourcePacks) || planned(plan, Kind::ShaderPacks);
    const QString group = anyPacks ? groupRoot(storeRoot, plan) : QString();
    QStringList ownShaders;  // a modpack's own shaders: their settings stay its own too

    // Packs first, so the switched-on list put in below names packs that exist.
    if (anyPacks) {
        const QString localPath = QFileInfo(statePath).dir().filePath(LocalPacksFile);
        // A modpack's packs stay its own: the ones it has when it first joins pack sync.
        if (plan.modpack && !QFileInfo::exists(localPath)) {
            QJsonObject local;
            for (const PackFolder& folder : PackFolders) {
                const QString name = QString::fromLatin1(folder.folder);
                local.insert(name, QJsonArray::fromStringList(packNames(QDir(gameRoot).filePath(name), name)));
            }
            if (auto written = FS::write(localPath, QJsonDocument(local).toJson(QJsonDocument::Compact)); !written)
                report.errors << written.error();
        }
        const QJsonObject local = readJson(localPath);
        ownShaders = toStringList(local.value(QStringLiteral("shaderpacks")));
        const QJsonObject removed = readJson(QDir(group).filePath(GroupStateFile)).value("removed").toObject();

        for (const PackFolder& folder : PackFolders) {
            if (!planned(plan, folder.kind))
                continue;
            const QString name = QString::fromLatin1(folder.folder);
            const QString instanceFolder = QDir(gameRoot).filePath(name);
            const QString groupFolder = QDir(group).filePath(name);
            const QStringList own = toStringList(local.value(name));
            const QStringList gone = toStringList(removed.value(name));
            const QStringList have = packNames(instanceFolder, name);

            int added = 0;
            for (const QString& pack : packNames(groupFolder, name)) {
                if (!have.contains(pack) && !own.contains(pack) && !gone.contains(pack) && copyPack(groupFolder, instanceFolder, pack, report))
                    ++added;
            }
            int deleted = 0;
            for (const QString& pack : gone) {
                if (!have.contains(pack) || own.contains(pack))
                    continue;
                if (removePack(plan, QDir(instanceFolder).filePath(pack)))
                    ++deleted;
                else
                    report.errors << QStringLiteral("Could not move %1 to the Recycle Bin").arg(pack);
            }
            const bool resource = folder.kind == Kind::ResourcePacks;
            if (added)
                report.changed << countText(resource ? QT_TRANSLATE_NOOP("SyncGameSettings", "%n resource pack(s) added")
                                                     : QT_TRANSLATE_NOOP("SyncGameSettings", "%n shader pack(s) added"),
                                            added);
            if (deleted)
                report.changed << countText(resource ? QT_TRANSLATE_NOOP("SyncGameSettings", "%n resource pack(s) removed")
                                                     : QT_TRANSLATE_NOOP("SyncGameSettings", "%n shader pack(s) removed"),
                                            deleted);
            packs.insert(name, QJsonArray::fromStringList(packNames(instanceFolder, name)));
        }
    }

    // options.txt: the game settings shared with all instances, then the group's pack list.
    const QString instanceOptions = readText(QDir(gameRoot).filePath(OptionsFile), report);
    QString options = instanceOptions;
    bool tookSettings = false;
    bool tookPackList = false;
    if (plan.gameSettings) {
        const QString storeOptionsPath = QDir(storeRoot).filePath(OptionsFile);
        if (QFileInfo(storeOptionsPath).isFile()) {
            const QString merged = applyOptions(options, readText(storeOptionsPath, report), plan.options);
            tookSettings = merged != options;
            options = merged;
        }
    }
    if (planned(plan, Kind::ResourcePacks)) {
        const QString groupOptionsPath = QDir(group).filePath(OptionsFile);
        if (QFileInfo(groupOptionsPath).isFile()) {
            OptionRules packRules;
            packRules.packListOnly = true;
            const QString merged = applyOptions(options, readText(groupOptionsPath, report), packRules);
            tookPackList = merged != options;
            options = merged;
        }
    }
    QString optionsAfter = instanceOptions;
    if (options != instanceOptions && writeWithBackup(OptionsFile, options.toUtf8(), gameRoot, backupRoot, report)) {
        optionsAfter = options;
        if (tookSettings)
            report.changed << QCoreApplication::translate("SyncGameSettings", "game settings");
        if (tookPackList)
            report.changed << QCoreApplication::translate("SyncGameSettings", "switched-on resource packs");
    }

    for (const WholeFile& file : WholeFiles) {
        if (!planned(plan, file.kind))
            continue;
        const QString path = QString::fromLatin1(file.path);
        const QString storePath = QDir(storeRoot).filePath(path);
        if (QFileInfo(storePath).isFile()) {
            auto data = FS::read(storePath);
            if (!data)
                report.errors << data.error();
            else if (writeWithBackup(path, *data, gameRoot, backupRoot, report))
                report.changed << kindName(file);
        }
        files.insert(path, sha1Of(QDir(gameRoot).filePath(path)));
    }

    QJsonObject groupFiles;
    if (planned(plan, Kind::ShaderPacks)) {
        int replaced = 0;
        for (const QString& path : shaderSettingsFiles(group, ownShaders)) {
            if (!QFileInfo(QDir(group).filePath(path)).isFile())
                continue;
            auto data = FS::read(QDir(group).filePath(path));
            if (!data)
                report.errors << data.error();
            else if (writeWithBackup(path, *data, gameRoot, backupRoot, report))
                ++replaced;
        }
        for (const QString& path : shaderSettingsFiles(gameRoot, ownShaders))
            groupFiles.insert(path, sha1Of(QDir(gameRoot).filePath(path)));
        if (replaced)
            report.changed << countText(QT_TRANSLATE_NOOP("SyncGameSettings", "%n shader settings file(s)"), replaced);
    }

    if (plan.modSettings) {
        int replaced = 0;
        for (const QString& entry : plan.sharedConfig) {
            // Only into instances that have the mod, which shows as its own config entry.
            if (!isPlainEntryName(entry) || !QFileInfo::exists(QDir(gameRoot).filePath(ConfigFolder + QLatin1Char('/') + entry)))
                continue;
            for (const QString& path : configFiles(storeRoot, entry)) {
                auto data = FS::read(QDir(storeRoot).filePath(path));
                if (!data)
                    report.errors << data.error();
                else if (writeWithBackup(path, *data, gameRoot, backupRoot, report))
                    ++replaced;
            }
            for (const QString& path : configFiles(gameRoot, entry))
                files.insert(path, sha1Of(QDir(gameRoot).filePath(path)));
        }
        if (replaced)
            report.changed << countText(QT_TRANSLATE_NOOP("SyncGameSettings", "%n mod settings file(s)"), replaced);
    }

    QJsonObject state;
    state.insert("options", optionsAfter);
    state.insert("files", files);
    state.insert("packs", packs);
    state.insert("groupFiles", groupFiles);
    if (auto written = FS::write(statePath, QJsonDocument(state).toJson(QJsonDocument::Compact)); !written)
        report.errors << written.error();
    return report;
}

Report collectFromInstance(const QString& storeRoot, const QString& gameRoot, const QString& statePath, const Plan& plan)
{
    Report report;
    if (!QFileInfo(statePath).isFile())
        return report;  // nothing was applied, so there is nothing to compare with
    auto stateData = FS::read(statePath);
    if (!stateData) {
        report.errors << stateData.error();
        return report;
    }
    const QJsonObject state = QJsonDocument::fromJson(*stateData).object();
    const QJsonObject files = state.value("files").toObject();
    const QString instanceOptions = readText(QDir(gameRoot).filePath(OptionsFile), report);
    const QString optionsAtLaunch = state.value("options").toString();

    if (plan.gameSettings) {
        const QString storeOptionsPath = QDir(storeRoot).filePath(OptionsFile);
        const QString storeText = readText(storeOptionsPath, report);
        const QString collected = collectOptions(storeText, instanceOptions, optionsAtLaunch, plan.options);
        if (collected != storeText) {
            if (auto written = FS::write(storeOptionsPath, collected.toUtf8()); !written)
                report.errors << written.error();
            else
                report.changed << QCoreApplication::translate("SyncGameSettings", "game settings");
        }
    }

    for (const WholeFile& file : WholeFiles) {
        const QString path = QString::fromLatin1(file.path);
        if (!planned(plan, file.kind) || !files.contains(path))
            continue;
        const QString now = sha1Of(QDir(gameRoot).filePath(path));
        const bool storeHasIt = QFileInfo(QDir(storeRoot).filePath(path)).isFile();
        if (!now.isEmpty() && (now != files.value(path).toString() || !storeHasIt) && copyFile(path, gameRoot, storeRoot, report))
            report.changed << kindName(file);
    }

    const bool anyPacks = planned(plan, Kind::ResourcePacks) || planned(plan, Kind::ShaderPacks);
    if (anyPacks) {
        const QString group = groupRoot(storeRoot, plan);
        const QJsonObject local = readJson(QFileInfo(statePath).dir().filePath(LocalPacksFile));
        const QJsonObject packsAtLaunch = state.value("packs").toObject();
        QJsonObject groupState = readJson(QDir(group).filePath(GroupStateFile));
        QJsonObject removed = groupState.value("removed").toObject();
        bool groupStateChanged = false;

        for (const PackFolder& folder : PackFolders) {
            const QString name = QString::fromLatin1(folder.folder);
            if (!planned(plan, folder.kind) || !packsAtLaunch.contains(name))
                continue;
            const QString instanceFolder = QDir(gameRoot).filePath(name);
            const QString groupFolder = QDir(group).filePath(name);
            const QStringList own = toStringList(local.value(name));
            const QStringList atLaunch = toStringList(packsAtLaunch.value(name));
            const QStringList now = packNames(instanceFolder, name);
            const QStringList inGroup = packNames(groupFolder, name);
            QStringList gone = toStringList(removed.value(name));

            int added = 0;
            for (const QString& pack : now) {
                // Added during this session, or not shared yet (the kind was switched on later).
                if (own.contains(pack) || inGroup.contains(pack))
                    continue;
                // Deleted elsewhere while this game ran (or it could not be moved out at launch):
                // having it since launch is no reason to bring it back. Added again, it was not.
                if (gone.contains(pack) && atLaunch.contains(pack))
                    continue;
                if (copyPack(instanceFolder, groupFolder, pack, report)) {
                    ++added;
                    gone.removeAll(pack);
                }
            }
            int deleted = 0;
            for (const QString& pack : atLaunch) {
                if (now.contains(pack) || own.contains(pack))
                    continue;
                // Deleted during this session: it leaves the other instances when they start.
                if (!gone.contains(pack))
                    gone << pack;
                if (inGroup.contains(pack) && !removePack(plan, QDir(groupFolder).filePath(pack)))
                    report.errors << QStringLiteral("Could not move %1 to the Recycle Bin").arg(pack);
                ++deleted;
            }
            if (gone != toStringList(removed.value(name))) {
                removed.insert(name, QJsonArray::fromStringList(gone));
                groupStateChanged = true;
            }
            const bool resource = folder.kind == Kind::ResourcePacks;
            if (added)
                report.changed << countText(resource ? QT_TRANSLATE_NOOP("SyncGameSettings", "%n resource pack(s) added")
                                                     : QT_TRANSLATE_NOOP("SyncGameSettings", "%n shader pack(s) added"),
                                            added);
            if (deleted)
                report.changed << countText(resource ? QT_TRANSLATE_NOOP("SyncGameSettings", "%n resource pack(s) removed")
                                                     : QT_TRANSLATE_NOOP("SyncGameSettings", "%n shader pack(s) removed"),
                                            deleted);
        }
        if (groupStateChanged) {
            groupState.insert("removed", removed);
            if (auto written = FS::write(QDir(group).filePath(GroupStateFile), QJsonDocument(groupState).toJson()); !written)
                report.errors << written.error();
        }

        if (planned(plan, Kind::ResourcePacks)) {
            OptionRules packRules;
            packRules.packListOnly = true;
            const QString groupOptionsPath = QDir(group).filePath(OptionsFile);
            const QString groupText = readText(groupOptionsPath, report);
            const QString collected = collectOptions(groupText, instanceOptions, optionsAtLaunch, packRules);
            if (collected != groupText) {
                if (auto written = FS::write(groupOptionsPath, collected.toUtf8()); !written)
                    report.errors << written.error();
                else
                    report.changed << QCoreApplication::translate("SyncGameSettings", "switched-on resource packs");
            }
        }

        if (planned(plan, Kind::ShaderPacks) && state.contains("groupFiles")) {
            const QJsonObject groupFiles = state.value("groupFiles").toObject();
            int saved = 0;
            for (const QString& path : shaderSettingsFiles(gameRoot, toStringList(local.value(QStringLiteral("shaderpacks"))))) {
                const QString now = sha1Of(QDir(gameRoot).filePath(path));
                const bool groupHasIt = QFileInfo(QDir(group).filePath(path)).isFile();
                if (!now.isEmpty() && (now != groupFiles.value(path).toString() || !groupHasIt) && copyFile(path, gameRoot, group, report))
                    ++saved;
            }
            if (saved)
                report.changed << countText(QT_TRANSLATE_NOOP("SyncGameSettings", "%n shader settings file(s)"), saved);
        }
    }

    if (plan.modSettings) {
        int saved = 0;
        for (const QString& entry : plan.sharedConfig) {
            if (!isPlainEntryName(entry))
                continue;
            for (const QString& path : configFiles(gameRoot, entry)) {
                const QString now = sha1Of(QDir(gameRoot).filePath(path));
                const bool storeHasIt = QFileInfo(QDir(storeRoot).filePath(path)).isFile();
                // A file with no record was not there at launch: the mod made it during this session.
                if ((now != files.value(path).toString() || !storeHasIt) && copyFile(path, gameRoot, storeRoot, report))
                    ++saved;
            }
        }
        if (saved)
            report.changed << countText(QT_TRANSLATE_NOOP("SyncGameSettings", "%n mod settings file(s)"), saved);
    }
    return report;
}

Report initializeStore(const QString& storeRoot, const QString& gameRoot, const Plan& plan)
{
    Report report;
    const Options options = parseOptions(readText(QDir(gameRoot).filePath(OptionsFile), report));
    if (plan.gameSettings) {
        // Everything except what is never synced: which settings move is decided at each launch,
        // so changing the choices later needs no new starting point.
        QStringList lines;
        for (const QString& key : options.keys) {
            if (key == DataVersionKey || (!neverSynced(key) && (!isKeybind(key) || plan.options.namedKeys)))
                lines << options.lines.value(key);
        }
        if (auto written = FS::write(QDir(storeRoot).filePath(OptionsFile), joinLines(lines).toUtf8()); !written)
            report.errors << written.error();
        else
            report.changed << QCoreApplication::translate("SyncGameSettings", "game settings");
    }
    for (const WholeFile& file : WholeFiles) {
        const QString path = QString::fromLatin1(file.path);
        if (planned(plan, file.kind) && QFileInfo(QDir(gameRoot).filePath(path)).isFile() && copyFile(path, gameRoot, storeRoot, report))
            report.changed << kindName(file);
    }

    // A modpack's packs are its own, so a modpack instance starts the pack groups empty.
    if (!plan.modpack) {
        const QString group = groupRoot(storeRoot, plan);
        QJsonObject groupState = readJson(QDir(group).filePath(GroupStateFile));
        QJsonObject removed = groupState.value("removed").toObject();
        bool groupStateChanged = false;
        for (const PackFolder& folder : PackFolders) {
            if (!planned(plan, folder.kind))
                continue;
            const QString name = QString::fromLatin1(folder.folder);
            const QStringList starting = packNames(QDir(gameRoot).filePath(name), name);
            int copied = 0;
            for (const QString& pack : starting) {
                if (copyPack(QDir(gameRoot).filePath(name), QDir(group).filePath(name), pack, report))
                    ++copied;
            }
            // The starting point's packs are wanted, whatever was deleted before.
            QStringList gone = toStringList(removed.value(name));
            const qsizetype before = gone.size();
            for (const QString& pack : starting)
                gone.removeAll(pack);
            if (gone.size() != before) {
                removed.insert(name, QJsonArray::fromStringList(gone));
                groupStateChanged = true;
            }
            if (copied)
                report.changed << countText(folder.kind == Kind::ResourcePacks
                                                ? QT_TRANSLATE_NOOP("SyncGameSettings", "%n resource pack(s) added")
                                                : QT_TRANSLATE_NOOP("SyncGameSettings", "%n shader pack(s) added"),
                                            copied);
        }
        if (groupStateChanged) {
            groupState.insert("removed", removed);
            if (auto written = FS::write(QDir(group).filePath(GroupStateFile), QJsonDocument(groupState).toJson()); !written)
                report.errors << written.error();
        }
        if (planned(plan, Kind::ResourcePacks)) {
            QStringList lines;
            for (const QString& key : options.keys) {
                if (isResourcePackKey(key))
                    lines << options.lines.value(key);
            }
            if (!lines.isEmpty()) {
                if (auto written = FS::write(QDir(group).filePath(OptionsFile), joinLines(lines).toUtf8()); !written)
                    report.errors << written.error();
            }
        }
        if (planned(plan, Kind::ShaderPacks)) {
            for (const QString& path : shaderSettingsFiles(gameRoot)) {
                if (QFileInfo(QDir(gameRoot).filePath(path)).isFile())
                    copyFile(path, gameRoot, group, report);
            }
        }
    }

    if (plan.modSettings) {
        int copied = 0;
        for (const QString& entry : plan.sharedConfig) {
            if (!isPlainEntryName(entry))
                continue;
            for (const QString& path : configFiles(gameRoot, entry)) {
                if (copyFile(path, gameRoot, storeRoot, report))
                    ++copied;
            }
        }
        if (copied)
            report.changed << countText(QT_TRANSLATE_NOOP("SyncGameSettings", "%n mod settings file(s)"), copied);
    }
    return report;
}

QString packGroupName(const QString& minecraftVersion, const QString& loader)
{
    if (minecraftVersion.isEmpty())
        return {};
    QString name = minecraftVersion + QLatin1Char('-') + loader;
    // A folder name: keep it to plain characters.
    for (QChar& c : name) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('.') && c != QLatin1Char('-') && c != QLatin1Char('_'))
            c = QLatin1Char('_');
    }
    return name;
}

QString packGroupFor(MinecraftInstance* instance)
{
    const QString version = minecraftVersion(instance);
    auto* profile = instance->getPackProfile();
    // The mod loader decides which packs work: Iris needs Fabric or Quilt, Oculus Forge, and so on.
    static const QList<QPair<QString, QString>> loaders{
        { QStringLiteral("net.neoforged"), QStringLiteral("neoforge") },
        { QStringLiteral("net.minecraftforge"), QStringLiteral("forge") },
        { QStringLiteral("net.fabricmc.fabric-loader"), QStringLiteral("fabric") },
        { QStringLiteral("org.quiltmc.quilt-loader"), QStringLiteral("quilt") },
    };
    QString loader = QStringLiteral("vanilla");
    for (const auto& [uid, name] : loaders) {
        if (!profile->getComponentVersion(uid).isEmpty()) {
            loader = name;
            break;
        }
    }
    return packGroupName(version, loader);
}

Plan planFor(MinecraftInstance* instance)
{
    Plan plan;
    const bool useSync = instanceUsesSync(instance);
    auto uses = [&](Kind kind) { return useSync && syncs(kind) && instanceUses(instance, kind); };
    plan.gameSettings = uses(Kind::GameSettings);
    plan.servers = uses(Kind::Servers);
    plan.commandHistory = uses(Kind::CommandHistory);
    plan.hotbars = uses(Kind::Hotbars);
    plan.resourcePacks = uses(Kind::ResourcePacks);
    plan.shaderPacks = uses(Kind::ShaderPacks);
    plan.modSettings = uses(Kind::ModSettings);

    const QStringList excluded = excludedOptionKeys() + ownOptionKeys(instance);
    plan.options.excluded = QSet<QString>(excluded.begin(), excluded.end());
    plan.options.modOptions = syncsModOptions();
    plan.options.namedKeys = usesNamedKeys(minecraftVersion(instance));
    plan.sharedConfig = sharedConfigEntries();
    if (plan.resourcePacks || plan.shaderPacks) {
        plan.packGroup = packGroupFor(instance);
        plan.modpack = instance->isManagedPack();
    }
    return plan;
}

Plan startingPlanFor(MinecraftInstance* instance)
{
    Plan plan;
    plan.gameSettings = syncs(Kind::GameSettings);
    plan.servers = syncs(Kind::Servers);
    plan.commandHistory = syncs(Kind::CommandHistory);
    plan.hotbars = syncs(Kind::Hotbars);
    plan.resourcePacks = syncs(Kind::ResourcePacks);
    plan.shaderPacks = syncs(Kind::ShaderPacks);
    plan.modSettings = syncs(Kind::ModSettings);
    plan.options.modOptions = syncsModOptions();
    plan.options.namedKeys = usesNamedKeys(minecraftVersion(instance));
    plan.sharedConfig = sharedConfigEntries();
    plan.packGroup = packGroupFor(instance);
    plan.modpack = instance->isManagedPack();
    return plan;
}

}  // namespace GameSettingsSync
