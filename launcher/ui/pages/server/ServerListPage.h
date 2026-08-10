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
    void onUpdateServerSoftware();
    void onChangeMinecraftVersion();
    void onRestoreLatestUpdateBackup();
    void onCheckContentUpdates();
    void onInstallContentUpdate();
    void onRefreshPlayers();
    void onWhitelistPlayer();
    void onOpPlayer();
    void onBanPlayer();
    void onKickPlayer();
    void onRemovePlayerAccess();
    void onViewPlayerHistory();
    void onExportPlayerHistory();
    void onExportProfile();
    void onImportProfile();
    void onSaveAutomation();
    void onRunAutomationNow();
    void onViewCrashReport();
    void onFindConsole();
    void onCopyConsoleErrors();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void protectInputFromWheel(QWidget *scope);
    void updateUI();
    void updateServerList();
    void updateSelectedServerInfo();
    void refreshCurrentServerTab();
    void refreshInstalledContent();
    void refreshOverview();
    void refreshLiveStatistics();
    void refreshServerBackups();
    void refreshServerFiles();
    void rebuildSettingsPage();
    void refreshPlayerList();
    void refreshAutomation();
    void runAutomation(const std::shared_ptr<ServerInstance> &server, const QString &action, int retentionLimit = 0);
    void refreshDiagnostics();
    void refreshAutomationHistory();
    void setupServerNavigation();
    void applyServerVisualHierarchy();
    void recordAutomation(const std::shared_ptr<ServerInstance> &server, const QString &action, const QString &result);
    void attachServerTracking(const std::shared_ptr<ServerInstance> &server);
    bool startServerSoftwareUpdate(const std::shared_ptr<ServerInstance> &server,
                                   const QString &targetVersion, bool changeVersion,
                                   const QString &targetBuild = QString());
    void populateServerFileItem(class QTreeWidgetItem *item);
    void appendConsoleOutput(const QString &text);
    QString getStatusString(int status) const;
    static bool copyDirectory(const QString &source, const QString &destination, QString *error,
                              const QString &excludedTopLevel = QString());
    static qint64 directorySize(const QString &directory);

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
    QWidget *m_updatesTab = nullptr;
    QLabel *m_updatesInfoLabel = nullptr;
    QTreeWidget *m_contentUpdatesTree = nullptr;
    QPushButton *m_updateServerSoftwareButton = nullptr;
    QPushButton *m_changeMinecraftVersionButton = nullptr;
    QPushButton *m_restoreLatestUpdateBackupButton = nullptr;
    QPushButton *m_checkContentUpdatesButton = nullptr;
    QPushButton *m_installContentUpdateButton = nullptr;
    QNetworkAccessManager *m_updatesNetwork = nullptr;
    ServerContentUpdater *m_activeContentUpdater = nullptr;
    QWidget *m_playersTab = nullptr;
    QLabel *m_playersInfoLabel = nullptr;
    QTreeWidget *m_playersTree = nullptr;
    QPushButton *m_refreshPlayersButton = nullptr;
    QPushButton *m_whitelistPlayerButton = nullptr;
    QPushButton *m_opPlayerButton = nullptr;
    QPushButton *m_banPlayerButton = nullptr;
    QPushButton *m_kickPlayerButton = nullptr;
    QPushButton *m_removePlayerAccessButton = nullptr;
    QPushButton *m_viewPlayerHistoryButton = nullptr;
    QPushButton *m_exportPlayerHistoryButton = nullptr;
    QPushButton *m_exportProfileButton = nullptr;
    QPushButton *m_importProfileButton = nullptr;
    QWidget *m_automationTab = nullptr;
    QLabel *m_automationInfoLabel = nullptr;
    QLabel *m_diagnosticsLabel = nullptr;
    QComboBox *m_scheduleActionCombo = nullptr;
    QTimeEdit *m_scheduleTimeEdit = nullptr;
    QSpinBox *m_backupRetentionSpin = nullptr;
    QSpinBox *m_gracefulStopTimeoutSpin = nullptr;
    QSpinBox *m_cpuWarningSpin = nullptr;
    QSpinBox *m_ramWarningSpin = nullptr;
    QSpinBox *m_diskWarningSpin = nullptr;
    QCheckBox *m_scheduleEnabledCheck = nullptr;
    QCheckBox *m_autoRestartCheck = nullptr;
    QPushButton *m_saveAutomationButton = nullptr;
    QPushButton *m_runAutomationButton = nullptr;
    QPushButton *m_viewCrashReportButton = nullptr;
    QListWidget *m_automationHistoryList = nullptr;
    QTimer m_automationTimer;
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
