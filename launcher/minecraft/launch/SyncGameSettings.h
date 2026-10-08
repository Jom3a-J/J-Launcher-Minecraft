// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <launch/LaunchStep.h>

// Copies the main instance's in-game settings into this instance before it starts, when it
// follows the main instance (see GameSettingsSync). It never stops the launch.
class SyncGameSettings : public LaunchStep {
    Q_OBJECT
   public:
    explicit SyncGameSettings(LaunchTask* parent) : LaunchStep(parent) {}

    void executeTask() override;
    bool canAbort() const override { return false; }
};
