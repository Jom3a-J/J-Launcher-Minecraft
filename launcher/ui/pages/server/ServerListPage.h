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
class ServerContentUpdater;
class ServerAutomationTab;
class ServerPlayersTab;
class ServerUpdatesTab;
class QProgressBar;
class QLabel;
class QListWidget;
class QPushButton;
class QTreeWidget;
class QNetworkAccessManager;
class QComboBox;
class QTimeEdit;
class QSpinBox;
class QCheckBox;
class QLineEdit;
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
    void onOpenFolder();
    void onInstallModpack();
    void onBrowseMods();
    void onAddLocalContent();
    void onToggleInstalledContent();
    void onRemoveInstalledContent();
    void onOpenContentFolder();
    void onCreateBackup();
    void onRestoreBackup();
    void onOpenBackupsFolder();
    void onRemoveBackup();
    void onImportServerPack();
    void onSendCommand();
    void onSettingsClicked();
    void onServerSelectionChanged();
    void onServerStatusChanged(ServerInstance *server);
    void onExportProfile();
    void onImportProfile();
    void onFindConsole();
    void onCopyConsoleErrors();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void protectInputFromWheel(QWidget *scope);
    void updateUI();
    void updateServerList();
    void updateSelectedServerInfo();
    /// Selects a server for the page and every tab; does not refresh them.
    void setSelectedServerId(const QString &serverId);
    void refreshCurrentServerTab();
    void refreshInstalledContent();
    void refreshOverview();
    void refreshLiveStatistics();
    void refreshServerBackups();
    void refreshServerFiles();
    void rebuildSettingsPage();
    void setupServerNavigation();
    void syncServerNavigation();
    void applyServerVisualHierarchy();
    void attachServerTracking(const std::shared_ptr<ServerInstance> &server);
    /// Selects and restores the backup at backupPath from the Backups list.
    void restoreBackupAt(const QString &backupPath);
    void populateServerFileItem(class QTreeWidgetItem *item);
    void appendConsoleOutput(const QString &text);
    void browseServerContent(const QString &initialSearch = QString());
    QStringList missingServerDependencies(ServerInstance *server) const;
    void offerDependencyRepair(const QStringList &missingIds,
                               const QString &introduction);
    QString getStatusString(int status) const;

    Ui::ServerListPage *ui;
    ServerManager *m_serverManager = nullptr;
    QString m_selectedServerId;
    std::shared_ptr<ServerInstance> m_currentConnectedServer = nullptr;
    QObject *m_serverTrackingContext = nullptr;
    ServerSettingsPage *m_embeddedSettingsPage = nullptr;
    QTimer m_liveStatisticsTimer;
    quint64 m_overviewRefreshGeneration = 0;
    qint64 m_liveStatisticsPid = 0;
    quint64 m_previousProcessCpuMs = 0;
    qint64 m_previousSampleTimeMs = 0;
    QProgressBar *m_overviewCpuBar = nullptr;
    QProgressBar *m_overviewRamBar = nullptr;
    QLabel *m_overviewSummaryLabel = nullptr;
    QLabel *m_overviewUpdatedLabel = nullptr;
    QTabWidget *m_maintenanceTab = nullptr;
    ServerUpdatesTab *m_updatesTab = nullptr;
    ServerPlayersTab *m_playersTab = nullptr;
    QPushButton *m_exportProfileButton = nullptr;
    QPushButton *m_importProfileButton = nullptr;
    ServerAutomationTab *m_automationTab = nullptr;
    QLineEdit *m_consoleSearchInput = nullptr;
    QPushButton *m_findConsoleButton = nullptr;
    QPushButton *m_copyConsoleErrorsButton = nullptr;
    QPushButton *m_undoDeleteButton = nullptr;
    QCheckBox *m_pauseConsoleScrollCheck = nullptr;
    QPushButton *m_clearConsoleButton = nullptr;
    QPushButton *m_exportConsoleButton = nullptr;
    QWidget *m_emptyServerListWidget = nullptr;
    QWidget *m_emptyDetailWidget = nullptr;
};
