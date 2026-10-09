// SPDX-License-Identifier: GPL-3.0-only

#include "GameSettingsSync.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

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
const QString OptionsFileName = QStringLiteral("options.txt");
const QString DataVersionKey = QStringLiteral("version");
const QString ResourcePacksFolder = QStringLiteral("resourcepacks");
const QString ShaderPacksFolder = QStringLiteral("shaderpacks");
const QString ConfigFolder = QStringLiteral("config");
// Shader loaders keep the selected shader in these files.
const QStringList ShaderSettingsFiles{ QStringLiteral("optionsshaders.txt"), QStringLiteral("config/iris.properties"),
                                       QStringLiteral("config/oculus.properties") };
// A copy in progress; renamed into place only once complete.
const QString PartialSuffix = QStringLiteral(".jlsync-part");
const QChar SharedConfigSeparator = QLatin1Char('|');  // cannot appear in a Windows file name

QString settingKey(Kind kind)
{
    switch (kind) {
        case Kind::GameSettings:
            return QStringLiteral("SettingsSyncGameSettings");
        case Kind::ResourcePacks:
            return QStringLiteral("SettingsSyncResourcePacks");
        case Kind::ShaderPacks:
            return QStringLiteral("SettingsSyncShaderPacks");
        case Kind::ModSettings:
            return QStringLiteral("SettingsSyncModSettings");
    }
    return {};
}

bool followedByDefault(Kind kind)
{
    // Mod settings can change how a modpack plays, so instances opt in.
    return kind != Kind::ModSettings;
}

bool isResourcePackKey(const QString& key)
{
    return key == QStringLiteral("resourcePacks") || key == QStringLiteral("incompatibleResourcePacks");
}

bool isKeybind(const QString& key)
{
    return key.startsWith(QStringLiteral("key_"));
}

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
    // A trailing newline leaves one empty entry, which is not a line of its own.
    if (!lines.isEmpty() && lines.constLast().isEmpty())
        lines.removeLast();
    return lines;
}

// A single path component naming something directly inside a folder.
bool isPlainEntryName(const QString& name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..") && !name.contains(QLatin1Char('/')) &&
           !name.contains(QLatin1Char('\\')) && !name.contains(QLatin1Char(':'));
}

// Writes data to followerRoot/relativePath, saving the follower's own copy under backupRoot
// the first time. Does nothing when the contents already match.
bool writeWithBackup(const QString& relativePath, const QByteArray& data, const QString& followerRoot, const QString& backupRoot,
                     Report& report)
{
    const QString followerPath = QDir(followerRoot).filePath(relativePath);
    if (QFileInfo(followerPath).isFile()) {
        auto current = FS::read(followerPath);
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
    if (auto written = FS::write(followerPath, data); !written) {
        report.errors << written.error();
        return false;
    }
    return true;
}

// Replaces one settings file in the follower with the main instance's copy, if the main has it.
void replaceSettingsFile(const QString& relativePath, const QString& mainRoot, const QString& followerRoot, const QString& backupRoot,
                         Report& report)
{
    const QString mainPath = QDir(mainRoot).filePath(relativePath);
    if (!QFileInfo(mainPath).isFile())
        return;
    auto data = FS::read(mainPath);
    if (!data) {
        report.errors << data.error();
        return;
    }
    if (writeWithBackup(relativePath, *data, followerRoot, backupRoot, report))
        ++report.settingsFilesReplaced;
}

// Copies the files and folders in mainRoot/folder that followerRoot/folder lacks. Never
// replaces or removes anything the follower has. skip() leaves entries to other rules.
template <typename Skip>
int addMissingEntries(const QString& folder, const QString& mainRoot, const QString& followerRoot, Report& report, Skip skip)
{
    const QDir mainDir(QDir(mainRoot).filePath(folder));
    if (!mainDir.exists())
        return 0;
    const QDir followerDir(QDir(followerRoot).filePath(folder));
    int added = 0;
    const auto entries = mainDir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& entry : entries) {
        const QString name = entry.fileName();
        if (name.endsWith(PartialSuffix) || skip(entry))
            continue;
        const QString target = followerDir.filePath(name);
        if (QFileInfo::exists(target))
            continue;
        if (!FS::ensureFolderPathExists(followerDir.path())) {
            report.errors << QStringLiteral("Could not create %1").arg(followerDir.path());
            return added;
        }
        const QString partial = target + PartialSuffix;
        if (QFileInfo::exists(partial))
            FS::deletePath(partial);
        const bool copied = entry.isDir() ? FS::copy(entry.absoluteFilePath(), partial)() : QFile::copy(entry.absoluteFilePath(), partial);
        if (!copied || !QDir().rename(partial, target)) {
            FS::deletePath(partial);
            report.errors << QStringLiteral("Could not copy %1").arg(name);
            continue;
        }
        ++added;
    }
    return added;
}

bool isShaderSettingsFile(const QFileInfo& entry)
{
    return entry.isFile() && entry.suffix().compare(QStringLiteral("txt"), Qt::CaseInsensitive) == 0;
}
}  // namespace

QString mainInstanceId()
{
    return APPLICATION->settings()->getOrRegisterSetting(QStringLiteral("SettingsSyncMainInstance"), QString())->get().toString();
}

void setMainInstanceId(const QString& instanceId)
{
    APPLICATION->settings()->getOrRegisterSetting(QStringLiteral("SettingsSyncMainInstance"), QString())->set(instanceId);
}

bool follows(BaseInstance* instance, Kind kind)
{
    return instance->settings()->getOrRegisterSetting(settingKey(kind), followedByDefault(kind))->get().toBool();
}

void setFollows(BaseInstance* instance, Kind kind, bool follow)
{
    instance->settings()->getOrRegisterSetting(settingKey(kind), followedByDefault(kind))->set(follow);
}

QStringList sharedConfigEntries(BaseInstance* mainInstance)
{
    const QString stored =
        mainInstance->settings()->getOrRegisterSetting(QStringLiteral("SettingsSyncSharedConfig"), QString())->get().toString();
    return stored.split(SharedConfigSeparator, Qt::SkipEmptyParts);
}

void setSharedConfigEntries(BaseInstance* mainInstance, const QStringList& entries)
{
    QStringList kept;
    for (const QString& entry : entries) {
        if (isPlainEntryName(entry) && !kept.contains(entry))
            kept << entry;
    }
    mainInstance->settings()
        ->getOrRegisterSetting(QStringLiteral("SettingsSyncSharedConfig"), QString())
        ->set(kept.join(SharedConfigSeparator));
}

QString minecraftVersion(MinecraftInstance* instance)
{
    auto* profile = instance->getPackProfile();
    QString version = profile->getComponentVersion(QStringLiteral("net.minecraft"));
    if (version.isEmpty()) {
        // Same as the status bar: load the component list from disk if nothing has yet.
        if (auto result = profile->reload(Net::Mode::Offline); !result) {
            qWarning() << "Settings sync: could not load the components of" << instance->name() << ":" << result.error();
        }
        version = profile->getComponentVersion(QStringLiteral("net.minecraft"));
    }
    return version;
}

bool keybindsCompatible(const QString& mainVersion, const QString& followerVersion)
{
    if (mainVersion.isEmpty() || followerVersion.isEmpty())
        return false;
    // 1.13 replaced numeric key codes with names such as "key.keyboard.w", and resource pack
    // names gained their "file/" prefix.
    const Version namedKeys(QStringLiteral("1.13"));
    return (Version(mainVersion) < namedKeys) == (Version(followerVersion) < namedKeys);
}

QString mergeOptions(const QString& followerText, const QString& mainText, const OptionsScope& scope)
{
    const QStringList followerLines = splitLines(followerText);
    bool followerHasDataVersion = false;
    for (const QString& line : followerLines) {
        if (keyOf(line) == DataVersionKey) {
            followerHasDataVersion = true;
            break;
        }
    }

    // The settings to copy, in the main file's order.
    QStringList mainKeys;
    QHash<QString, QString> mainLines;
    QString mainDataVersionLine;
    for (const QString& line : splitLines(mainText)) {
        const QString key = keyOf(line);
        if (key.isEmpty() || mainLines.contains(key))
            continue;
        if (key == DataVersionKey) {
            mainDataVersionLine = line;
            continue;
        }
        const bool wanted = isResourcePackKey(key) ? scope.resourcePackList
                            : isKeybind(key)        ? scope.gameSettings && scope.keybinds
                                                    : scope.gameSettings;
        if (!wanted)
            continue;
        mainKeys.append(key);
        mainLines.insert(key, line);
    }
    // The data version tells Minecraft how to read the rest. The follower keeps its own; a
    // follower without one takes the version that matches the copied lines.
    if (!followerHasDataVersion && !mainDataVersionLine.isEmpty() && !mainKeys.isEmpty()) {
        mainKeys.prepend(DataVersionKey);
        mainLines.insert(DataVersionKey, mainDataVersionLine);
    }

    QStringList merged;
    QSet<QString> replaced;
    for (const QString& line : followerLines) {
        const QString key = keyOf(line);
        const auto mainLine = mainLines.constFind(key);
        if (!key.isEmpty() && mainLine != mainLines.constEnd() && !replaced.contains(key)) {
            merged.append(*mainLine);
            replaced.insert(key);
        } else {
            merged.append(line);
        }
    }
    for (const QString& key : mainKeys) {
        if (!replaced.contains(key))
            merged.append(mainLines.value(key));
    }
    return merged.isEmpty() ? QString() : merged.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

Report sync(const QString& mainGameRoot, const QString& followerGameRoot, const QString& backupRoot, const Choices& choices)
{
    Report report;

    // Pack files first, so the switched-on list copied below names packs that exist.
    if (choices.resourcePacks) {
        report.resourcePacksAdded =
            addMissingEntries(ResourcePacksFolder, mainGameRoot, followerGameRoot, report, [](const QFileInfo&) { return false; });
    }
    if (choices.shaderPacks) {
        // A shader's own settings file sits next to it as "<pack>.txt"; it is replaced below.
        report.shaderPacksAdded =
            addMissingEntries(ShaderPacksFolder, mainGameRoot, followerGameRoot, report, isShaderSettingsFile);
    }

    OptionsScope scope;
    scope.gameSettings = choices.gameSettings;
    scope.keybinds = choices.sameKeyFormat;
    scope.resourcePackList = choices.resourcePacks && choices.sameKeyFormat;
    if (scope.gameSettings || scope.resourcePackList) {
        const QString mainPath = QDir(mainGameRoot).filePath(OptionsFileName);
        report.mainHasOptions = QFileInfo(mainPath).isFile();
        if (report.mainHasOptions) {
            auto mainData = FS::read(mainPath);
            if (!mainData) {
                report.errors << mainData.error();
            } else {
                QByteArray followerData;
                const QString followerPath = QDir(followerGameRoot).filePath(OptionsFileName);
                bool readable = true;
                if (QFileInfo(followerPath).isFile()) {
                    auto data = FS::read(followerPath);
                    readable = data.has_value();
                    if (data)
                        followerData = *data;
                    else
                        report.errors << data.error();
                }
                if (readable) {
                    const QString merged = mergeOptions(QString::fromUtf8(followerData), QString::fromUtf8(*mainData), scope);
                    report.optionsChanged =
                        merged.toUtf8() != followerData && writeWithBackup(OptionsFileName, merged.toUtf8(), followerGameRoot, backupRoot, report);
                }
            }
        }
    }

    if (choices.shaderPacks) {
        const QDir mainShaders(QDir(mainGameRoot).filePath(ShaderPacksFolder));
        for (const QFileInfo& entry : mainShaders.entryInfoList(QDir::Files, QDir::Name)) {
            if (isShaderSettingsFile(entry))
                replaceSettingsFile(ShaderPacksFolder + QLatin1Char('/') + entry.fileName(), mainGameRoot, followerGameRoot, backupRoot, report);
        }
        for (const QString& file : ShaderSettingsFiles)
            replaceSettingsFile(file, mainGameRoot, followerGameRoot, backupRoot, report);
    }

    if (choices.modSettings) {
        const QDir mainConfig(QDir(mainGameRoot).filePath(ConfigFolder));
        for (const QString& entry : choices.sharedConfig) {
            if (!isPlainEntryName(entry))
                continue;
            const QFileInfo info(mainConfig.filePath(entry));
            if (info.isFile()) {
                replaceSettingsFile(ConfigFolder + QLatin1Char('/') + entry, mainGameRoot, followerGameRoot, backupRoot, report);
            } else if (info.isDir()) {
                QDirIterator files(info.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
                while (files.hasNext()) {
                    const QString relative = mainConfig.relativeFilePath(files.next());
                    replaceSettingsFile(ConfigFolder + QLatin1Char('/') + relative, mainGameRoot, followerGameRoot, backupRoot, report);
                }
            }
        }
    }

    return report;
}

QString backupRootFor(BaseInstance* instance)
{
    return QDir(instance->instanceRoot()).filePath(QStringLiteral("settings-sync-backup"));
}

}  // namespace GameSettingsSync
