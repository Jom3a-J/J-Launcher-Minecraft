// SPDX-License-Identifier: GPL-3.0-only

#include "SyncGameSettings.h"

#include "Application.h"
#include "InstanceList.h"
#include "launch/LaunchTask.h"
#include "logs/Privacy.h"
#include "minecraft/GameSettingsSync.h"
#include "minecraft/MinecraftInstance.h"

using GameSettingsSync::Kind;

void SyncGameSettings::executeTask()
{
    auto* instance = m_parent->instance();
    const QString mainId = GameSettingsSync::mainInstanceId();
    if (mainId.isEmpty() || mainId == instance->id()) {
        emitSucceeded();
        return;
    }

    GameSettingsSync::Choices choices;
    choices.gameSettings = GameSettingsSync::follows(instance, Kind::GameSettings);
    choices.resourcePacks = GameSettingsSync::follows(instance, Kind::ResourcePacks);
    choices.shaderPacks = GameSettingsSync::follows(instance, Kind::ShaderPacks);
    choices.modSettings = GameSettingsSync::follows(instance, Kind::ModSettings);
    if (!choices.gameSettings && !choices.resourcePacks && !choices.shaderPacks && !choices.modSettings) {
        emitSucceeded();
        return;
    }

    auto* main = APPLICATION->instances()->getInstanceById(mainId);
    if (!main) {
        emit logLine(tr("Settings sync: the main instance no longer exists, so nothing was copied."), MessageLevel::Launcher);
        emitSucceeded();
        return;
    }

    choices.sameKeyFormat =
        GameSettingsSync::keybindsCompatible(GameSettingsSync::minecraftVersion(main), GameSettingsSync::minecraftVersion(instance));
    if (choices.modSettings)
        choices.sharedConfig = GameSettingsSync::sharedConfigEntries(main);

    const auto report =
        GameSettingsSync::sync(main->gameRoot(), instance->gameRoot(), GameSettingsSync::backupRootFor(instance), choices);

    QStringList done;
    if (report.optionsChanged)
        done << tr("game settings");
    if (report.resourcePacksAdded)
        done << tr("%n resource pack(s)", nullptr, report.resourcePacksAdded);
    if (report.shaderPacksAdded)
        done << tr("%n shader pack(s)", nullptr, report.shaderPacksAdded);
    if (report.settingsFilesReplaced)
        done << tr("%n shader or mod settings file(s)", nullptr, report.settingsFilesReplaced);

    QString line = done.isEmpty() ? tr("Settings sync: everything already matches \"%1\".").arg(main->name())
                                  : tr("Settings sync: copied %1 from \"%2\".").arg(done.join(QStringLiteral(", ")), main->name());
    if ((choices.gameSettings || choices.resourcePacks) && !report.mainHasOptions)
        line += QLatin1Char(' ') + tr("\"%1\" has no game settings yet.").arg(main->name());
    if (!choices.sameKeyFormat && (choices.gameSettings || choices.resourcePacks))
        line += QLatin1Char(' ') +
                tr("Keybinds and the switched-on resource packs were left as they are, because the two instances store them differently.");
    if (report.backupsMade)
        line += QLatin1Char(' ') + tr("This instance's own copies of replaced files were saved in its settings-sync-backup folder.");
    emit logLine(line, MessageLevel::Launcher);

    for (const QString& error : report.errors)
        emit logLine(tr("Settings sync: %1").arg(Privacy::sanitizeText(error)), MessageLevel::Warning);
    emitSucceeded();
}
