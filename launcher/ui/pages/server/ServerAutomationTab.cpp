// SPDX-License-Identifier: GPL-3.0-only

#include "ServerAutomationTab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimeEdit>
#include <QVBoxLayout>

#include "server/ServerInstance.h"
#include "server/ServerManager.h"

ServerAutomationTab::ServerAutomationTab(QWidget *parent) : QScrollArea(parent)
{
    setObjectName(QStringLiteral("automationHealthScrollArea"));
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(this);
    setWidget(content);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 10, 10, 10);
    m_infoLabel = new QLabel(tr("Schedule one daily maintenance action. A scheduled backup runs only while the server is stopped."), content);
    m_infoLabel->setObjectName(QStringLiteral("automationInfoLabel"));
    m_infoLabel->setWordWrap(true);
    layout->addWidget(m_infoLabel);

    auto *scheduleGroup = new QGroupBox(tr("Daily Schedule & Backup Retention"), content);
    auto *scheduleForm = new QFormLayout(scheduleGroup);
    m_scheduleEnabledCheck = new QCheckBox(tr("Enable daily schedule"), scheduleGroup);
    m_scheduleEnabledCheck->setObjectName(QStringLiteral("scheduleEnabledCheck"));
    m_scheduleActionCombo = new QComboBox(scheduleGroup);
    m_scheduleActionCombo->setObjectName(QStringLiteral("scheduleActionCombo"));
    m_scheduleActionCombo->addItem(tr("Start"), "start");
    m_scheduleActionCombo->addItem(tr("Stop"), "stop");
    m_scheduleActionCombo->addItem(tr("Restart"), "restart");
    m_scheduleActionCombo->addItem(tr("Backup"), "backup");
    m_scheduleTimeEdit = new QTimeEdit(QTime::currentTime(), scheduleGroup);
    m_scheduleTimeEdit->setObjectName(QStringLiteral("scheduleTimeEdit"));
    m_scheduleTimeEdit->setDisplayFormat("HH:mm");
    m_backupRetentionSpin = new QSpinBox(scheduleGroup);
    m_backupRetentionSpin->setObjectName(QStringLiteral("backupRetentionSpin"));
    m_backupRetentionSpin->setRange(0, 100);
    m_backupRetentionSpin->setSpecialValueText(tr("Keep all backups"));
    m_backupRetentionSpin->setToolTip(tr("Automatic backups beyond this number are removed, oldest first. 0 keeps all."));
    m_gracefulStopTimeoutSpin = new QSpinBox(scheduleGroup);
    m_gracefulStopTimeoutSpin->setRange(5, 120);
    m_gracefulStopTimeoutSpin->setSuffix(tr(" seconds"));
    m_gracefulStopTimeoutSpin->setToolTip(tr("How long to wait after sending the stop command before terminating an unresponsive server."));
    m_autoRestartCheck = new QCheckBox(tr("Restart automatically after a crash (5-second delay)"), scheduleGroup);
    scheduleForm->addRow(m_scheduleEnabledCheck);
    scheduleForm->addRow(tr("Action:"), m_scheduleActionCombo);
    scheduleForm->addRow(tr("Time:"), m_scheduleTimeEdit);
    scheduleForm->addRow(tr("Keep automatic backups:"), m_backupRetentionSpin);
    scheduleForm->addRow(tr("Graceful stop timeout:"), m_gracefulStopTimeoutSpin);
    scheduleForm->addRow(m_autoRestartCheck);
    auto *scheduleActions = new QHBoxLayout();
    m_saveButton = new QPushButton(tr("Save Automation"), scheduleGroup);
    m_saveButton->setObjectName(QStringLiteral("saveAutomationButton"));
    m_runButton = new QPushButton(tr("Run Selected Action Now"), scheduleGroup);
    scheduleActions->addWidget(m_saveButton);
    scheduleActions->addWidget(m_runButton);
    scheduleActions->addStretch();
    scheduleForm->addRow(scheduleActions);
    layout->addWidget(scheduleGroup);

    auto *alertsGroup = new QGroupBox(tr("Monitoring Warning Thresholds"), content);
    auto *alertsForm = new QFormLayout(alertsGroup);
    m_cpuWarningSpin = new QSpinBox(alertsGroup);
    m_cpuWarningSpin->setObjectName(QStringLiteral("cpuWarningSpin"));
    m_cpuWarningSpin->setRange(50, 100);
    m_cpuWarningSpin->setSuffix("%");
    m_cpuWarningSpin->setToolTip(tr("CPU use at or above this value is shown as a warning."));
    m_ramWarningSpin = new QSpinBox(alertsGroup);
    m_ramWarningSpin->setObjectName(QStringLiteral("ramWarningSpin"));
    m_ramWarningSpin->setRange(50, 100);
    m_ramWarningSpin->setSuffix("%");
    m_ramWarningSpin->setToolTip(tr("Configured RAM use at or above this value is shown as a warning."));
    m_diskWarningSpin = new QSpinBox(alertsGroup);
    m_diskWarningSpin->setObjectName(QStringLiteral("diskWarningSpin"));
    m_diskWarningSpin->setRange(1, 1000);
    m_diskWarningSpin->setSuffix(tr(" GB free"));
    m_diskWarningSpin->setToolTip(tr("Free disk space at or below this value is shown as a warning."));
    alertsForm->addRow(tr("CPU warning:"), m_cpuWarningSpin);
    alertsForm->addRow(tr("RAM warning:"), m_ramWarningSpin);
    alertsForm->addRow(tr("Disk warning:"), m_diskWarningSpin);
    layout->addWidget(alertsGroup);

    auto *historyGroup = new QGroupBox(tr("Automation Activity"), content);
    auto *historyLayout = new QVBoxLayout(historyGroup);
    m_historyList = new QListWidget(historyGroup);
    m_historyList->setObjectName(QStringLiteral("automationHistoryList"));
    m_historyList->setSelectionMode(QAbstractItemView::NoSelection);
    m_historyList->setAlternatingRowColors(true);
    m_historyList->setToolTip(tr("The 50 most recent scheduled or manually run maintenance actions for this server."));
    m_historyList->setMinimumHeight(140);
    historyLayout->addWidget(m_historyList);
    layout->addWidget(historyGroup);

    auto *diagnosticsGroup = new QGroupBox(tr("Crash Diagnostics"), content);
    auto *diagnosticsLayout = new QVBoxLayout(diagnosticsGroup);
    m_diagnosticsLabel = new QLabel(tr("No crash report recorded for this server."), diagnosticsGroup);
    m_diagnosticsLabel->setObjectName(QStringLiteral("diagnosticsLabel"));
    m_diagnosticsLabel->setWordWrap(true);
    m_viewCrashReportButton = new QPushButton(tr("View Latest Crash Report"), diagnosticsGroup);
    m_viewCrashReportButton->setObjectName(QStringLiteral("viewCrashReportButton"));
    diagnosticsLayout->addWidget(m_diagnosticsLabel);
    diagnosticsLayout->addWidget(m_viewCrashReportButton, 0, Qt::AlignLeft);
    layout->addWidget(diagnosticsGroup);
    layout->addStretch();

    connect(m_saveButton, &QPushButton::clicked, this, &ServerAutomationTab::save);
    connect(m_runButton, &QPushButton::clicked, this, &ServerAutomationTab::runNow);
    connect(m_viewCrashReportButton, &QPushButton::clicked, this, &ServerAutomationTab::showCrashReport);
}

void ServerAutomationTab::setServerManager(ServerManager *manager)
{
    m_serverManager = manager;
}

void ServerAutomationTab::setServerId(const QString &serverId)
{
    m_serverId = serverId;
}

void ServerAutomationTab::refresh()
{
    if (!m_serverManager || m_serverId.isEmpty()) {
        m_infoLabel->setText(tr("Select a server to configure automated maintenance."));
    } else if (const auto server = m_serverManager->getServer(m_serverId)) {
        QSettings settings;
        const QString prefix = QString("ServerAutomation/%1/").arg(server->id());
        m_scheduleEnabledCheck->setChecked(settings.value(prefix + "enabled", false).toBool());
        const QString action = settings.value(prefix + "action", "start").toString();
        const int actionIndex = m_scheduleActionCombo->findData(action);
        m_scheduleActionCombo->setCurrentIndex(actionIndex >= 0 ? actionIndex : 0);
        const QTime time = QTime::fromString(settings.value(prefix + "time", "03:00").toString(), "HH:mm");
        m_scheduleTimeEdit->setTime(time.isValid() ? time : QTime(3, 0));
        m_backupRetentionSpin->setValue(settings.value(prefix + "retention", 0).toInt());
        m_gracefulStopTimeoutSpin->setValue(server->gracefulStopTimeoutSeconds());
        m_autoRestartCheck->setChecked(server->autoRestartOnCrash());
        const QString monitoringPrefix = QString("ServerMonitoring/%1/").arg(server->id());
        m_cpuWarningSpin->setValue(settings.value(monitoringPrefix + "cpuWarning", 85).toInt());
        m_ramWarningSpin->setValue(settings.value(monitoringPrefix + "ramWarning", 90).toInt());
        m_diskWarningSpin->setValue(settings.value(monitoringPrefix + "diskWarningGb", 2).toInt());
        m_infoLabel->setText(tr("Daily schedules are checked every 30 seconds. The server must be stopped for an automatic backup."));
        refreshHistory();
    }
    refreshDiagnostics();
}

void ServerAutomationTab::save()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server || server->isRunning()) return;
    QSettings settings;
    const QString prefix = QString("ServerAutomation/%1/").arg(server->id());
    settings.setValue(prefix + "enabled", m_scheduleEnabledCheck->isChecked());
    settings.setValue(prefix + "action", m_scheduleActionCombo->currentData().toString());
    settings.setValue(prefix + "time", m_scheduleTimeEdit->time().toString("HH:mm"));
    settings.setValue(prefix + "retention", m_backupRetentionSpin->value());
    const QString monitoringPrefix = QString("ServerMonitoring/%1/").arg(server->id());
    settings.setValue(monitoringPrefix + "cpuWarning", m_cpuWarningSpin->value());
    settings.setValue(monitoringPrefix + "ramWarning", m_ramWarningSpin->value());
    settings.setValue(monitoringPrefix + "diskWarningGb", m_diskWarningSpin->value());
    server->setAutoRestartOnCrash(m_autoRestartCheck->isChecked());
    server->setGracefulStopTimeoutSeconds(m_gracefulStopTimeoutSpin->value());
    m_serverManager->save();
    m_infoLabel->setText(tr("Automation saved for %1.").arg(server->name()));
}

void ServerAutomationTab::refreshHistory()
{
    m_historyList->clear();
    if (m_serverId.isEmpty()) {
        m_historyList->addItem(tr("Select a server to view automation activity."));
        return;
    }
    QSettings settings;
    const QStringList history = settings.value(QString("ServerAutomation/%1/history").arg(m_serverId)).toStringList();
    if (history.isEmpty()) {
        m_historyList->addItem(tr("No automated actions have run for this server yet."));
        return;
    }
    m_historyList->addItems(history);
}

void ServerAutomationTab::runNow()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) return;
    m_serverManager->runAutomation(server, m_scheduleActionCombo->currentData().toString(),
                                   m_backupRetentionSpin->value());
    m_infoLabel->setText(tr("Ran %1 action for %2.").arg(m_scheduleActionCombo->currentText(), server->name()));
    emit automationRun();
}

void ServerAutomationTab::refreshDiagnostics()
{
    if (m_serverId.isEmpty()) {
        m_diagnosticsLabel->setText(tr("No server selected."));
        m_viewCrashReportButton->setEnabled(false);
        return;
    }
    QSettings settings;
    const QString crash = settings.value(QString("ServerDiagnostics/%1/lastCrash").arg(m_serverId)).toString();
    m_diagnosticsLabel->setText(crash.isEmpty() ? tr("No crash report recorded for this server.") : tr("Latest crash: %1").arg(crash));
    m_viewCrashReportButton->setEnabled(!crash.isEmpty());
}

void ServerAutomationTab::showCrashReport()
{
    if (m_serverId.isEmpty()) return;
    QSettings settings;
    const QString details = settings.value(QString("ServerDiagnostics/%1/details").arg(m_serverId)).toString();
    if (details.isEmpty()) return;
    QMessageBox dialog(this);
    dialog.setWindowTitle(tr("Latest Crash Report"));
    dialog.setIcon(QMessageBox::Critical);
    dialog.setText(m_diagnosticsLabel->text());
    dialog.setDetailedText(details);
    dialog.exec();
}

void ServerAutomationTab::updateActions()
{
    const auto server = m_serverManager && !m_serverId.isEmpty()
        ? m_serverManager->getServer(m_serverId) : nullptr;
    const bool hasSelection = server != nullptr;
    const bool canEditFiles = server
        && (server->status() == ServerStatus::Stopped || server->status() == ServerStatus::Error);
    m_scheduleEnabledCheck->setEnabled(hasSelection);
    m_scheduleActionCombo->setEnabled(hasSelection);
    m_scheduleTimeEdit->setEnabled(hasSelection);
    m_backupRetentionSpin->setEnabled(hasSelection);
    m_cpuWarningSpin->setEnabled(hasSelection && canEditFiles);
    m_ramWarningSpin->setEnabled(hasSelection && canEditFiles);
    m_diskWarningSpin->setEnabled(hasSelection && canEditFiles);
    m_autoRestartCheck->setEnabled(hasSelection && canEditFiles);
    m_saveButton->setEnabled(hasSelection && canEditFiles);
    m_runButton->setEnabled(hasSelection);
    m_viewCrashReportButton->setEnabled(hasSelection && !m_diagnosticsLabel->text().startsWith(tr("No crash")));
}
