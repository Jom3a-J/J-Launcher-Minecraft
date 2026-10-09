// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QIcon>
#include <QWidget>

#include "ui/pages/BasePage.h"

class QCheckBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class MinecraftInstance;

// An instance's Sync page: whether this instance takes part in sync between instances, which
// kinds it follows, and which game settings it keeps as its own. Changes apply at once.
class SettingsSyncPage : public QWidget, public BasePage {
    Q_OBJECT

   public:
    explicit SettingsSyncPage(MinecraftInstance* instance, QWidget* parent = nullptr);

    QString displayName() const override { return tr("Sync"); }
    QIcon icon() const override { return QIcon::fromTheme("copy"); }
    QString id() const override { return "sync"; }
    void openedImpl() override;
    void retranslate() override;

   private:
    void updateTexts();
    void refresh();
    void fillOwnOptions();
    void onOwnOptionChanged(QListWidgetItem* item);

    MinecraftInstance* m_instance;
    QLabel* m_status;
    QCheckBox* m_use;
    QGroupBox* m_kindsGroup;
    QCheckBox* m_kindBoxes[5];
    QGroupBox* m_ownGroup;
    QLabel* m_ownIntro;
    QListWidget* m_ownOptions;
    bool m_filling = false;
};
