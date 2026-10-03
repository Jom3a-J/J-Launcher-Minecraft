/* Copyright 2013-2024 MultiMC Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ServerListPage.h"
#include "ui_ServerListPage.h"
#include "ui/pages/server/ServerAutomationTab.h"
#include "ui/pages/server/ServerBackupsTab.h"
#include "ui/pages/server/ServerConsoleTab.h"
#include "ui/pages/server/ServerContentTab.h"
#include "ui/pages/server/ServerFilesTab.h"
#include "ui/pages/server/ServerOverviewTab.h"
#include "ui/pages/server/ServerPlayersTab.h"
#include "ui/pages/server/ServerUpdatesTab.h"
#include "ui/pages/server/ServerPageStyle.h"
#include <QAbstractButton>
#include <QVBoxLayout>
#include <QColor>
#include <QStyle>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QTreeWidget>
#include <QProgressBar>
#include <QGroupBox>
#include <QHeaderView>
#include <QTabBar>
#include <QTabWidget>
#include <QSignalBlocker>

using ServerPageStyle::applyMutedLabelPalette;
using ServerPageStyle::launcherIcon;

void ServerListPage::setupServerNavigation()
{
    // Keep the selected server visible while its operational tools switch in
    // the workspace. The dedicated rail avoids an overflowing tab strip and
    // gives every destination a stable icon and keyboard-accessible row.
    const QList<QPair<QWidget *, QString>> pages = {
        {m_overviewTab, tr("Home")},
        {m_consoleTab, tr("Console")},
        {m_contentTab, tr("Mods")},
        {m_playersTab, tr("Players")},
        {m_filesTab, tr("Files")},
        {m_backupsTab, tr("Backups")},
        {m_maintenanceTab, tr("Tools")},
        {ui->settingsTab, tr("Settings")}
    };

    for(const auto &page : pages)
    {
        const int index = ui->serverTabs->indexOf(page.first);
        if(index >= 0)
        {
            ui->serverTabs->removeTab(index);
        }
    }
    for(const auto &page : pages)
    {
        ui->serverTabs->addTab(page.first, page.second);
    }

    const QList<QIcon> icons = {
        launcherIcon("server", QStyle::SP_ComputerIcon),
        launcherIcon("log", QStyle::SP_FileDialogDetailedView),
        launcherIcon("loadermods", QStyle::SP_FileIcon),
        launcherIcon("accounts", QStyle::SP_DirHomeIcon),
        launcherIcon("viewfolder", QStyle::SP_DirOpenIcon),
        launcherIcon("copy", QStyle::SP_DialogSaveButton),
        launcherIcon("custom-commands", QStyle::SP_ComputerIcon),
        launcherIcon("settings", QStyle::SP_FileDialogContentsView)
    };
    for (int index = 0; index < icons.size(); ++index) {
        ui->serverTabs->setTabIcon(index, icons.at(index));
    }

    ui->serverTabs->tabBar()->hide();
    ui->serverNavigationList->clear();
    for (int index = 0; index < ui->serverTabs->count(); ++index) {
        auto *item = new QListWidgetItem(ui->serverTabs->tabIcon(index),
                                         ui->serverTabs->tabText(index),
                                         ui->serverNavigationList);
        item->setData(Qt::UserRole, index);
        item->setSizeHint(QSize(126, 44));
    }

    connect(ui->serverNavigationList, &QListWidget::currentRowChanged, this,
            [this](int row) {
        if (row < 0 || row >= ui->serverTabs->count()) return;
        if (!ui->serverTabs->isTabEnabled(row)) {
            syncServerNavigation();
            return;
        }
        ui->serverTabs->setCurrentIndex(row);
    });
    connect(ui->serverTabs, &QTabWidget::currentChanged, this,
            [this](int) { syncServerNavigation(); });

    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(m_overviewTab),
                                   tr("Server status, live resource use, and quick statistics."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(m_consoleTab),
                                   tr("Live server output and commands."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(m_contentTab),
                                   tr("Installed mods or plugins and downloads."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(m_playersTab),
                                   tr("Player access, operator levels, bans, and history."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(m_filesTab),
                                   tr("Browse server files and import or export server profiles."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(m_backupsTab),
                                   tr("Create, restore, and manage complete server backups."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(m_maintenanceTab),
                                   tr("Software updates, automation, monitoring thresholds, and crash diagnostics."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->settingsTab),
                                   tr("Server properties, Java, memory, and launch settings."));
    ui->serverTabs->setCurrentWidget(m_overviewTab);
    syncServerNavigation();

    m_maintenanceTab->setDocumentMode(false);
    m_maintenanceTab->setUsesScrollButtons(true);
    m_maintenanceTab->tabBar()->setExpanding(false);
}

void ServerListPage::syncServerNavigation()
{
    if (!ui->serverNavigationList) return;

    const QSignalBlocker blocker(ui->serverNavigationList);
    for (int index = 0; index < ui->serverTabs->count(); ++index) {
        QListWidgetItem *item = ui->serverNavigationList->item(index);
        if (!item) continue;
        item->setText(ui->serverTabs->tabText(index));
        item->setIcon(ui->serverTabs->tabIcon(index));
        Qt::ItemFlags flags = item->flags();
        if (ui->serverTabs->isTabEnabled(index)) {
            flags |= Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        } else {
            flags &= ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
        item->setFlags(flags);
        item->setToolTip(ui->serverTabs->tabToolTip(index));
    }
    ui->serverNavigationList->setCurrentRow(ui->serverTabs->currentIndex());
}

void ServerListPage::applyServerVisualHierarchy()
{
    // Palette roles keep the workspace native to every launcher theme while
    // object-scoped styling supplies the quieter geometry and action hierarchy.
    QFont titleFont = ui->titleLabel->font();
    titleFont.setBold(true);
    ui->titleLabel->setFont(titleFont);

    QFont sectionFont = ui->serverListTitle->font();
    sectionFont.setBold(true);
    ui->serverListTitle->setFont(sectionFont);
    ui->detailTitleLabel->setFont(sectionFont);

    applyMutedLabelPalette(ui->serverStatsLabel);
    applyMutedLabelPalette(ui->selectedServerInfoLabel);

    ui->createFromModpackButton->setIcon(launcherIcon("centralmods", QStyle::SP_FileDialogNewFolder));
    ui->createServerButton->setIcon(launcherIcon("new", QStyle::SP_FileDialogNewFolder));
    ui->startServerButton->setIcon(launcherIcon("launch", QStyle::SP_MediaPlay));
    ui->stopServerButton->setIcon(launcherIcon("status-bad", QStyle::SP_MediaStop));
    ui->restartServerButton->setIcon(launcherIcon("refresh", QStyle::SP_BrowserReload));
    ui->deleteServerButton->setIcon(launcherIcon("delete", QStyle::SP_TrashIcon));

    ui->createServerButton->setProperty("role", "primary");
    ui->startServerButton->setProperty("role", "primary");
    ui->deleteServerButton->setProperty("role", "danger");

    setStyleSheet(QStringLiteral(R"QSS(
        QWidget#ServerListPage {
            background: palette(window);
            color: palette(text);
        }
        QFrame#serverHeaderPanel {
            background: palette(base);
            border: none;
            border-bottom: 1px solid palette(midlight);
        }
        QFrame#serverListPanel {
            background: palette(alternate-base);
            border: none;
            border-right: 1px solid palette(midlight);
        }
        QFrame#serverDetailPanel {
            background: palette(window);
            border: none;
        }
        QFrame#serverCommandBar,
        QFrame#serverWorkspacePanel,
        QFrame#overviewMetricsPanel {
            background: palette(base);
            border: 1px solid palette(midlight);
            border-radius: 12px;
        }
        QLabel#titleLabel,
        QLabel#detailTitleLabel,
        QLabel#serverListTitle {
            color: palette(text);
        }
        QLineEdit,
        QComboBox,
        QSpinBox,
        QTimeEdit {
            min-height: 32px;
            padding: 2px 9px;
            background: palette(base);
            color: palette(text);
            border: 1px solid palette(midlight);
            border-radius: 8px;
            selection-background-color: palette(highlight);
            selection-color: palette(highlighted-text);
        }
        QLineEdit:focus,
        QComboBox:focus,
        QSpinBox:focus,
        QTimeEdit:focus {
            border: 2px solid palette(highlight);
            padding: 1px 8px;
        }
        QPushButton {
            min-height: 30px;
            padding: 3px 12px;
            background: palette(button);
            color: palette(button-text);
            border: 1px solid palette(midlight);
            border-radius: 8px;
        }
        QPushButton:hover {
            border-color: palette(highlight);
        }
        QPushButton:focus {
            border: 2px solid palette(highlight);
            padding: 2px 11px;
        }
        QPushButton[role="primary"] {
            background: palette(highlight);
            color: palette(highlighted-text);
            border-color: palette(highlight);
            font-weight: 600;
        }
        QPushButton:disabled {
            background: palette(alternate-base);
            color: #7d7d7d;
            border-color: palette(midlight);
        }
        QListWidget#serverList {
            background: transparent;
            border: none;
            outline: none;
        }
        QListWidget#serverList::item {
            background: transparent;
            border: none;
        }
        QWidget#serverCard {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 10px;
        }
        QWidget#serverCard:hover {
            background: palette(base);
            border-color: palette(midlight);
        }
        QWidget#serverCard[selected="true"] {
            background: palette(base);
            border-color: palette(highlight);
        }
        QListWidget#serverNavigationList {
            background: palette(alternate-base);
            border: none;
            border-right: 1px solid palette(midlight);
            padding: 9px 7px;
            outline: none;
        }
        QListWidget#serverNavigationList::item {
            min-height: 40px;
            padding: 0 10px;
            border: 1px solid transparent;
            border-radius: 8px;
        }
        QListWidget#serverNavigationList::item:hover {
            background: palette(base);
        }
        QListWidget#serverNavigationList::item:selected {
            background: palette(highlight);
            color: palette(highlighted-text);
            border-color: palette(highlight);
        }
        QListWidget#serverNavigationList::item:disabled {
            color: palette(mid);
        }
        QTabWidget#serverTabs::pane {
            background: palette(base);
            border: none;
        }
        QGroupBox {
            margin-top: 10px;
            padding: 12px;
            background: palette(base);
            border: 1px solid palette(midlight);
            border-radius: 10px;
            font-weight: 600;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 5px;
        }
        QFrame#overviewMetricsPanel QGroupBox {
            margin-top: 0;
            padding: 14px 16px;
            border: none;
            border-right: 1px solid palette(midlight);
            border-radius: 0;
            background: transparent;
        }
        QPlainTextEdit#consoleOutput,
        QTreeWidget {
            background: palette(base);
            color: palette(text);
            border: 1px solid palette(midlight);
            border-radius: 8px;
            selection-background-color: palette(highlight);
            selection-color: palette(highlighted-text);
        }
        QHeaderView::section {
            background: palette(alternate-base);
            color: palette(text);
            border: none;
            border-bottom: 1px solid palette(midlight);
            padding: 7px 8px;
            font-weight: 600;
        }
        QProgressBar {
            min-height: 12px;
            background: palette(alternate-base);
            border: 1px solid palette(midlight);
            border-radius: 6px;
            text-align: center;
        }
        QProgressBar::chunk {
            background: palette(highlight);
            border-radius: 5px;
        }
    )QSS"));

    // Keep destructive actions unmistakable without sacrificing contrast in
    // either launcher appearance. A local rule wins over the page's generic
    // button text rule while leaving the native button surface untouched.
    const bool darkAppearance = palette().color(QPalette::Window).lightness() < 128;
    ui->deleteServerButton->setStyleSheet(QStringLiteral("color: %1;").arg(
        darkAppearance ? QStringLiteral("#ff8a80") : QStringLiteral("#b3261e")));
}
