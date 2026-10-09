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
class QTreeWidget;
class QTreeWidgetItem;
class MinecraftInstance;

// Settings → Sync: sync between instances, Modrinth style (see GameSettingsSync).
// Changes apply at once.
class SyncPage : public QWidget, public BasePage {
    Q_OBJECT

   public:
    explicit SyncPage(QWidget* parent = nullptr);

    QString displayName() const override { return tr("Sync"); }
    QIcon icon() const override { return QIcon::fromTheme("copy"); }
    QString id() const override { return "sync"; }
    void openedImpl() override;
    void retranslate() override;

   private:
    void updateTexts();
    void refresh();
    void fillOptions();
    void fillConfig();
    void onEnableClicked(bool checked);
    void onStartAgain();
    bool startFromInstance(const QString& title);
    void onOptionChanged(QTreeWidgetItem* item);
    void onConfigChanged(QListWidgetItem* item);

    QLabel* m_intro;
    QCheckBox* m_enable;
    QLabel* m_status;
    QPushButton* m_startAgain;
    QGroupBox* m_kindsGroup;
    QCheckBox* m_kindBoxes[GameSettingsSync::KindCount];
    QGroupBox* m_optionsGroup;
    QTreeWidget* m_options;
    QCheckBox* m_modOptions;
    QGroupBox* m_configGroup;
    QLabel* m_configIntro;
    QListWidget* m_config;
    QPushButton* m_refreshConfig;
    QLabel* m_note;
    bool m_filling = false;
};
