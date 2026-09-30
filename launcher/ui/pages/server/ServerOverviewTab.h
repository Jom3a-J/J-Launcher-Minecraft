// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QWidget>

class ServerManager;
class QLabel;
class QProgressBar;
class QPushButton;

/// The Server Manager's "Home" tab: status, uptime, storage, a summary, and live CPU/RAM use.
class ServerOverviewTab : public QWidget
{
    Q_OBJECT

public:
    explicit ServerOverviewTab(QWidget *parent = nullptr);

    void setServerManager(ServerManager *manager);
    /// The server the tab shows; empty for none. Call refresh() to load it.
    void setServerId(const QString &serverId);

    /// Reloads the status and summary; folder sizes are measured in the background.
    void refresh();
    /// Samples the server process's CPU and memory; call it periodically while the tab shows.
    void refreshLiveStatistics();
    /// Enables the actions that apply to the current selection.
    void updateActions();

private:
    ServerManager *m_serverManager = nullptr;
    QString m_serverId;
    QLabel *m_statusValue = nullptr;
    QLabel *m_uptimeValue = nullptr;
    QLabel *m_storageValue = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QProgressBar *m_cpuBar = nullptr;
    QProgressBar *m_ramBar = nullptr;
    QLabel *m_updatedLabel = nullptr;
    QPushButton *m_refreshButton = nullptr;
    /// Discards folder-size results from a refresh that a newer one replaced.
    quint64 m_refreshGeneration = 0;
    /// The previous CPU sample, for turning CPU time into a percentage.
    qint64 m_liveStatisticsPid = 0;
    quint64 m_previousProcessCpuMs = 0;
    qint64 m_previousSampleTimeMs = 0;
};
