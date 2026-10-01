// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QScrollArea>

class ServerManager;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTimeEdit;

/*! The Server Manager's "Automation & Health" tab: the daily schedule, backup retention,
 *  crash restart, monitoring warning levels, automation history and the latest crash.
 */
class ServerAutomationTab : public QScrollArea
{
    Q_OBJECT

public:
    explicit ServerAutomationTab(QWidget *parent = nullptr);

    void setServerManager(ServerManager *manager);
    /// The server the tab shows; empty for none. Call refresh() to load it.
    void setServerId(const QString &serverId);

    /// Reloads everything the tab shows for the current server.
    void refresh();
    void refreshHistory();
    void refreshDiagnostics();
    /// Enables the controls that apply to the current server's state.
    void updateActions();

signals:
    /// An action ran from this tab; the server may be starting, stopping or backing up.
    void automationRun();

private:
    void save();
    void runNow();
    void showCrashReport();

    ServerManager *m_serverManager = nullptr;
    QString m_serverId;
    QLabel *m_infoLabel = nullptr;
    QCheckBox *m_scheduleEnabledCheck = nullptr;
    QComboBox *m_scheduleActionCombo = nullptr;
    QTimeEdit *m_scheduleTimeEdit = nullptr;
    QSpinBox *m_backupRetentionSpin = nullptr;
    QSpinBox *m_gracefulStopTimeoutSpin = nullptr;
    QCheckBox *m_autoRestartCheck = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_runButton = nullptr;
    QSpinBox *m_cpuWarningSpin = nullptr;
    QSpinBox *m_ramWarningSpin = nullptr;
    QSpinBox *m_diskWarningSpin = nullptr;
    QListWidget *m_historyList = nullptr;
    QLabel *m_diagnosticsLabel = nullptr;
    QPushButton *m_viewCrashReportButton = nullptr;
};
