// SPDX-License-Identifier: GPL-3.0-only

#include "SyncGameSettings.h"

#include "launch/LaunchTask.h"
#include "logs/Privacy.h"
#include "minecraft/MinecraftInstance.h"

void SyncGameSettings::executeTask()
{
    auto* instance = m_parent->instance();
    if (!GameSettingsSync::enabled() || !GameSettingsSync::instanceUsesSync(instance)) {
        emitSucceeded();
        return;
    }

    m_plan = GameSettingsSync::planFor(instance);
    const auto report = GameSettingsSync::applyToInstance(GameSettingsSync::storeRoot(), instance->gameRoot(),
                                                          GameSettingsSync::backupRootFor(instance),
                                                          GameSettingsSync::launchStateFor(instance), m_plan);
    // Without this session's own record, closing the game must not compare against anything.
    m_session = report.session;
    m_applied = !m_session.isEmpty();
    logReport(tr("Sync: put the shared %1 into this instance."), report);
    if (!m_plan.options.namedKeys && m_plan.gameSettings)
        emit logLine(tr("Sync: keybinds are not synced with this instance, because Minecraft before 1.13 stores them differently."),
                     MessageLevel::Launcher);
    if (report.backupsMade)
        emit logLine(tr("Sync: this instance's own copies of replaced files were saved in its settings-sync-backup folder."),
                     MessageLevel::Launcher);
    emitSucceeded();
}

void SyncGameSettings::finalize()
{
    if (!m_applied)
        return;
    m_applied = false;
    auto* instance = m_parent->instance();
    const auto report = GameSettingsSync::collectFromInstance(GameSettingsSync::storeRoot(), instance->gameRoot(),
                                                              GameSettingsSync::launchStateFor(instance), m_plan, m_session);
    logReport(tr("Sync: saved the changed %1 for the other instances."), report);
}

void SyncGameSettings::logReport(const QString& doneText, const GameSettingsSync::Report& report)
{
    if (!report.changed.isEmpty())
        emit logLine(doneText.arg(report.changed.join(QStringLiteral(", "))), MessageLevel::Launcher);
    for (const QString& error : report.errors)
        emit logLine(tr("Sync: %1").arg(Privacy::sanitizeText(error)), MessageLevel::Warning);
}
