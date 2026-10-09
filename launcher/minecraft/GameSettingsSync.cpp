// SPDX-License-Identifier: GPL-3.0-only

#include "GameSettingsSync.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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
        case Kind::ModSettings:
            return QStringLiteral("SyncModSettings");
    }
    return {};
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

// Settings that are never synced: the data version (how to read the file) and the resource
// pack list (resource packs are their own, later step).
bool neverSynced(const QString& key)
{
    return key == DataVersionKey || key == QStringLiteral("resourcePacks") || key == QStringLiteral("incompatibleResourcePacks");
}

bool isKeybind(const QString& key)
{
    return key.startsWith(QStringLiteral("key_"));
}

// Whether a setting moves between this instance and the shared copy. A setting a mod adds only
// goes into instances that already have it.
bool synced(const QString& key, const OptionRules& rules, bool instanceHasKey)
{
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
        case Kind::ModSettings:
            return plan.modSettings;
    }
    return false;
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
    return globalSetting(globalKey(kind), kind != Kind::ModSettings)->get().toBool();
}

void setSyncs(Kind kind, bool on)
{
    globalSetting(globalKey(kind), kind != Kind::ModSettings)->set(on);
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
    QString optionsAfter;

    const QString instanceOptionsPath = QDir(gameRoot).filePath(OptionsFile);
    const QString instanceOptions = readText(instanceOptionsPath, report);
    optionsAfter = instanceOptions;
    if (plan.gameSettings) {
        const QString storeOptionsPath = QDir(storeRoot).filePath(OptionsFile);
        if (QFileInfo(storeOptionsPath).isFile()) {
            const QString merged = applyOptions(instanceOptions, readText(storeOptionsPath, report), plan.options);
            if (merged != instanceOptions && writeWithBackup(OptionsFile, merged.toUtf8(), gameRoot, backupRoot, report)) {
                report.changed << QCoreApplication::translate("SyncGameSettings", "game settings");
                optionsAfter = merged;
            }
        }
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
            report.changed << QCoreApplication::translate("SyncGameSettings", "%n mod settings file(s)", nullptr, replaced);
    }

    QJsonObject state;
    state.insert("options", optionsAfter);
    state.insert("files", files);
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

    if (plan.gameSettings) {
        const QString storeOptionsPath = QDir(storeRoot).filePath(OptionsFile);
        const QString storeText = readText(storeOptionsPath, report);
        const QString collected = collectOptions(storeText, readText(QDir(gameRoot).filePath(OptionsFile), report),
                                                 state.value("options").toString(), plan.options);
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
            report.changed << QCoreApplication::translate("SyncGameSettings", "%n mod settings file(s)", nullptr, saved);
    }
    return report;
}

Report initializeStore(const QString& storeRoot, const QString& gameRoot, const Plan& plan)
{
    Report report;
    if (plan.gameSettings) {
        // Everything except what is never synced: which settings move is decided at each launch,
        // so changing the choices later needs no new starting point.
        const Options options = parseOptions(readText(QDir(gameRoot).filePath(OptionsFile), report));
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
            report.changed << QCoreApplication::translate("SyncGameSettings", "%n mod settings file(s)", nullptr, copied);
    }
    return report;
}

Plan planFor(MinecraftInstance* instance)
{
    Plan plan;
    const bool useSync = instanceUsesSync(instance);
    plan.gameSettings = useSync && syncs(Kind::GameSettings) && instanceUses(instance, Kind::GameSettings);
    plan.servers = useSync && syncs(Kind::Servers) && instanceUses(instance, Kind::Servers);
    plan.commandHistory = useSync && syncs(Kind::CommandHistory) && instanceUses(instance, Kind::CommandHistory);
    plan.hotbars = useSync && syncs(Kind::Hotbars) && instanceUses(instance, Kind::Hotbars);
    plan.modSettings = useSync && syncs(Kind::ModSettings) && instanceUses(instance, Kind::ModSettings);

    const QStringList excluded = excludedOptionKeys() + ownOptionKeys(instance);
    plan.options.excluded = QSet<QString>(excluded.begin(), excluded.end());
    plan.options.modOptions = syncsModOptions();
    plan.options.namedKeys = usesNamedKeys(minecraftVersion(instance));
    plan.sharedConfig = sharedConfigEntries();
    return plan;
}

Plan startingPlanFor(MinecraftInstance* instance)
{
    Plan plan;
    plan.gameSettings = syncs(Kind::GameSettings);
    plan.servers = syncs(Kind::Servers);
    plan.commandHistory = syncs(Kind::CommandHistory);
    plan.hotbars = syncs(Kind::Hotbars);
    plan.modSettings = syncs(Kind::ModSettings);
    plan.options.modOptions = syncsModOptions();
    plan.options.namedKeys = usesNamedKeys(minecraftVersion(instance));
    plan.sharedConfig = sharedConfigEntries();
    return plan;
}

}  // namespace GameSettingsSync
