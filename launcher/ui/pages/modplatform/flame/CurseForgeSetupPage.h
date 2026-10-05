// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QIcon>
#include <QWidget>

#include "ui/pages/BasePage.h"

/*! Stands in for the CurseForge page in the New Instance dialog when the build has no CurseForge
 *  API key, and sends the user to the settings page where they can add their own. */
class CurseForgeSetupPage final : public QWidget, public BasePage {
   public:
    explicit CurseForgeSetupPage(QWidget* parent = nullptr);

    QString displayName() const override { return tr("CurseForge"); }
    QIcon icon() const override { return QIcon::fromTheme(QStringLiteral("flame")); }
    QString id() const override { return QStringLiteral("curseforge-setup"); }
};

/*! Sits above the pages of the mod download window when the build has no CurseForge API key,
 *  explaining why CurseForge is missing and sending the user to the settings page. */
class CurseForgeSetupRow final : public QWidget {
   public:
    explicit CurseForgeSetupRow(QWidget* parent = nullptr);
};
