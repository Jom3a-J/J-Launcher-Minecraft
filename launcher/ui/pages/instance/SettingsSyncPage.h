// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QIcon>
#include <QWidget>

#include "minecraft/GameSettingsSync.h"
#include "ui/pages/BasePage.h"

class QCheckBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class MinecraftInstance;

// Instance settings sync: make this instance the main one, choose what it follows from the
// main instance, and (on the main instance) which mod settings it shares. Changes apply at once.
class SettingsSyncPage : public QWidget, public BasePage {
    Q_OBJECT

   public:
    explicit SettingsSyncPage(MinecraftInstance* instance, QWidget* parent = nullptr);

    QString displayName() const override { return tr("Settings Sync"); }
    QIcon icon() const override { return QIcon::fromTheme("copy"); }
    QString id() const override { return "settings-sync"; }
    void openedImpl() override;
    void retranslate() override;

   private:
    void updateTexts();
    void refresh();
    void refreshSharedConfig();
    void onMainToggled(bool checked);
    void onSharedConfigChanged(QListWidgetItem* item);

    MinecraftInstance* m_instance;
    QLabel* m_intro;
    QCheckBox* m_mainBox;
    QLabel* m_status;
    QGroupBox* m_followGroup;
    QCheckBox* m_followBoxes[4];
    QGroupBox* m_shareGroup;
    QLabel* m_shareIntro;
    QListWidget* m_sharedConfig;
    QPushButton* m_refreshConfig;
    QLabel* m_note;
    bool m_fillingList = false;
};
