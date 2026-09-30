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

#pragma once

#include <QWidget>
#include <QTimer>
#include <memory>

class ServerManager;
class ServerInstance;
class ServerSettingsPage;
class ServerAutomationTab;
class ServerBackupsTab;
class ServerConsoleTab;
class ServerContentTab;
class ServerOverviewTab;
class ServerFilesTab;
class ServerPlayersTab;
class ServerUpdatesTab;
class QPushButton;
class QTabWidget;

namespace Ui {
class ServerListPage;
}

class ServerListPage : public QWidget
{
    Q_OBJECT

public:
    explicit ServerListPage(QWidget *parent = nullptr);
    ~ServerListPage();

    void setServerManager(ServerManager *manager);

signals:
    void serverSelected(const QString &id);
    void serverStarted(const QString &id);
    void serverStopped(const QString &id);

private slots:
    void onCreateServer();
    void onStartServer();
    void onStopServer();
    void onRestartServer();
    void onDeleteServer();
    void onUndoDelete();
    void onInstallModpack();
    void onSettingsClicked();
    void onServerSelectionChanged();
    void onServerStatusChanged(ServerInstance *server);

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void protectInputFromWheel(QWidget *scope);
    void updateUI();
    void updateServerList();
    void updateSelectedServerInfo();
    /// Selects a server for the page and every tab; does not refresh them.
    void setSelectedServerId(const QString &serverId);
    void refreshCurrentServerTab();
    void rebuildSettingsPage();
    void setupServerNavigation();
    void syncServerNavigation();
    void applyServerVisualHierarchy();
    void attachServerTracking(const std::shared_ptr<ServerInstance> &server);
    /// Selects and restores the backup at backupPath from the Backups list.
    void restoreBackupAt(const QString &backupPath);
    void appendConsoleOutput(const QString &text);

    Ui::ServerListPage *ui;
    ServerManager *m_serverManager = nullptr;
    QString m_selectedServerId;
    std::shared_ptr<ServerInstance> m_currentConnectedServer = nullptr;
    QObject *m_serverTrackingContext = nullptr;
    ServerSettingsPage *m_embeddedSettingsPage = nullptr;
    QTimer m_liveStatisticsTimer;
    QTabWidget *m_maintenanceTab = nullptr;
    ServerUpdatesTab *m_updatesTab = nullptr;
    ServerBackupsTab *m_backupsTab = nullptr;
    ServerFilesTab *m_filesTab = nullptr;
    ServerConsoleTab *m_consoleTab = nullptr;
    ServerContentTab *m_contentTab = nullptr;
    ServerOverviewTab *m_overviewTab = nullptr;
    ServerPlayersTab *m_playersTab = nullptr;
    ServerAutomationTab *m_automationTab = nullptr;
    QPushButton *m_undoDeleteButton = nullptr;
    QWidget *m_emptyServerListWidget = nullptr;
    QWidget *m_emptyDetailWidget = nullptr;
};
