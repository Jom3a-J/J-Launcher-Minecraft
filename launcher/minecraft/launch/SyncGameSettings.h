// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <launch/LaunchStep.h>

#include "minecraft/GameSettingsSync.h"

// Sync between instances (see GameSettingsSync): before the game starts, puts the shared copy
// into this instance; after the game has closed, saves what changed into the shared copy.
// It never stops the launch.
class SyncGameSettings : public LaunchStep {
    Q_OBJECT
   public:
    explicit SyncGameSettings(LaunchTask* parent) : LaunchStep(parent) {}

    void executeTask() override;
    void finalize() override;
    bool canAbort() const override { return false; }

   private:
    void logReport(const QString& doneText, const GameSettingsSync::Report& report);

    bool m_applied = false;
    QString m_session;  // this launch's own sync record
    GameSettingsSync::Plan m_plan;
};
