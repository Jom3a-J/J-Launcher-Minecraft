// SPDX-License-Identifier: GPL-3.0-only

#include "ServerOverviewTab.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStorageInfo>
#include <QThread>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include "logs/Privacy.h"
#include "server/ServerDiagnostics.h"
#include "server/ServerFiles.h"
#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "server/ServerProcessStats.h"
#include "ServerPageStyle.h"

namespace {
/// One of the large figures across the top of the tab.
QLabel *addMetricCard(QGridLayout *grid, int column, const QString &objectName, const QString &title)
{
    auto *card = new QGroupBox(title, grid->parentWidget());
    card->setObjectName(objectName + QStringLiteral("Card"));
    auto *layout = new QVBoxLayout(card);
    auto *value = new QLabel(QStringLiteral("—"), card);
    value->setObjectName(objectName + QStringLiteral("Value"));
    value->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QFont font = value->font();
    font.setPointSize(14);
    font.setBold(true);
    value->setFont(font);
    layout->addWidget(value);
    grid->addWidget(card, 0, column);
    return value;
}
}  // namespace

ServerOverviewTab::ServerOverviewTab(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("overviewTab"));
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(14);
    layout->setContentsMargins(18, 16, 18, 18);

    auto *metrics = new QFrame(this);
    metrics->setObjectName(QStringLiteral("overviewMetricsPanel"));
    metrics->setFrameShape(QFrame::StyledPanel);
    auto *metricsGrid = new QGridLayout(metrics);
    metricsGrid->setHorizontalSpacing(0);
    metricsGrid->setVerticalSpacing(0);
    metricsGrid->setContentsMargins(0, 0, 0, 0);
    m_statusValue = addMetricCard(metricsGrid, 0, QStringLiteral("overviewStatus"), tr("Status"));
    m_uptimeValue = addMetricCard(metricsGrid, 1, QStringLiteral("overviewMemory"), tr("Uptime"));
    m_storageValue = addMetricCard(metricsGrid, 2, QStringLiteral("overviewContent"), tr("Storage"));
    layout->addWidget(metrics);

    auto *summaryGroup = new QGroupBox(tr("Server Summary"), this);
    summaryGroup->setObjectName("overviewSummaryGroup");
    auto *summaryLayout = new QVBoxLayout(summaryGroup);
    m_summaryLabel = new QLabel(tr("No server data available."), summaryGroup);
    m_summaryLabel->setObjectName("overviewSummaryLabel");
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setTextFormat(Qt::RichText);
    m_summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summaryLayout->addWidget(m_summaryLabel);
    layout->addWidget(summaryGroup);

    auto *liveUsageGroup = new QGroupBox(tr("Live Resource Use"), this);
    liveUsageGroup->setObjectName("overviewLiveUsageGroup");
    auto *liveUsageLayout = new QGridLayout(liveUsageGroup);
    liveUsageLayout->addWidget(new QLabel(tr("CPU"), liveUsageGroup), 0, 0);
    m_cpuBar = new QProgressBar(liveUsageGroup);
    m_cpuBar->setRange(0, 100);
    liveUsageLayout->addWidget(m_cpuBar, 0, 1);
    liveUsageLayout->addWidget(new QLabel(tr("RAM"), liveUsageGroup), 1, 0);
    m_ramBar = new QProgressBar(liveUsageGroup);
    liveUsageLayout->addWidget(m_ramBar, 1, 1);
    m_updatedLabel = new QLabel(tr("Waiting for live data."), liveUsageGroup);
    m_updatedLabel->setAlignment(Qt::AlignRight);
    ServerPageStyle::applyMutedLabelPalette(m_updatedLabel);
    liveUsageLayout->addWidget(m_updatedLabel, 2, 0, 1, 2);
    layout->addWidget(liveUsageGroup);

    auto *actions = new QHBoxLayout();
    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_refreshButton->setObjectName(QStringLiteral("refreshOverviewButton"));
    m_refreshButton->setEnabled(false);
    actions->addWidget(m_refreshButton);
    actions->addStretch();
    layout->addLayout(actions);
    layout->addStretch();

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        refresh();
        refreshLiveStatistics();
    });
}

void ServerOverviewTab::setServerManager(ServerManager *manager)
{
    m_serverManager = manager;
}

void ServerOverviewTab::setServerId(const QString &serverId)
{
    m_serverId = serverId;
}

void ServerOverviewTab::updateActions()
{
    m_refreshButton->setEnabled(!m_serverId.isEmpty());
}

void ServerOverviewTab::refresh()
{
    const quint64 generation = ++m_refreshGeneration;
    const auto setEmpty = [this]() {
        m_statusValue->setText("-");
        m_statusValue->setStyleSheet({});
        m_uptimeValue->setText("-");
        m_storageValue->setText("-");
        if (m_summaryLabel) {
            m_summaryLabel->setText(tr("No server data available."));
            m_summaryLabel->setToolTip({});
        }
    };

    if (!m_serverManager || m_serverId.isEmpty()) {
        setEmpty();
        return;
    }

    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) {
        setEmpty();
        return;
    }

    const ServerContentType contentType = server->contentType();
    const bool pluginServer = contentType == ServerContentType::Plugin;
    const bool supportsContent = contentType != ServerContentType::None;
    const QString contentDirectory = server->contentDirectory();
    const int contentCount = supportsContent
        ? QDir(contentDirectory).entryInfoList(QStringList() << "*.jar" << "*.jar.disabled", QDir::Files).size()
        : 0;

    const QDir serverDirectory(server->serverDirectory());
    const QFileInfoList worldDirectories = serverDirectory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    int worldCount = 0;
    QStringList worldPaths;
    for (const QFileInfo &directory : worldDirectories) {
        if (directory.fileName().startsWith("world", Qt::CaseInsensitive)) {
            ++worldCount;
            worldPaths.append(directory.absoluteFilePath());
        }
    }

    const QDir backupsDirectory(serverDirectory.filePath("backups"));
    int backupCount = 0;
    for (const QFileInfo &backup : backupsDirectory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (backup.fileName().startsWith("server-", Qt::CaseInsensitive)) ++backupCount;
    }

    const QString status = ServerPageStyle::statusText(server->status());
    const QColor statusColor = ServerPageStyle::statusColor(server->status());

    m_statusValue->setText(status);
    m_statusValue->setStyleSheet(QString("color: %1;").arg(statusColor.name()));
    m_uptimeValue->setText(server->status() == ServerStatus::Running ? tr("Starting...") : tr("Not running"));
    m_storageValue->setText(tr("Calculating..."));

    const QString serverType = server->loaderType().isEmpty() ? tr("Vanilla") : server->loaderType();
    const QString contentSummary = supportsContent
        ? tr("%1 %2").arg(contentCount).arg(pluginServer ? tr("plugins") : tr("mods"))
        : tr("Not applicable");
    if (m_summaryLabel) {
        m_summaryLabel->setText(
            tr("<b>Version:</b> %1 &nbsp;&bull;&nbsp; <b>Type:</b> %2 &nbsp;&bull;&nbsp; <b>Port:</b> %3<br>"
               "<b>Content:</b> %4 &nbsp;&bull;&nbsp; <b>Backups:</b> %5 &nbsp;&bull;&nbsp; <b>Worlds:</b> %6 (%7)")
                .arg(server->version().toHtmlEscaped(), serverType.toHtmlEscaped())
                .arg(server->port())
                .arg(contentSummary.toHtmlEscaped())
                .arg(backupCount)
                .arg(worldCount)
                .arg(tr("Calculating...").toHtmlEscaped()));
        const QString java = server->javaPath().isEmpty()
            ? tr("Automatic")
            : Privacy::sanitizePath(server->javaPath());
        m_summaryLabel->setToolTip(
            tr("Server folder: %1\nJava: %2\nConfigured memory: %3-%4 MB")
                .arg(Privacy::sanitizePath(server->serverDirectory()), java)
                .arg(server->minMemory())
                .arg(server->maxMemory()));
    }

    const QString selectedServerId = server->id();
    const QString serverDirectoryPath = server->serverDirectory();
    auto *watcher = new QFutureWatcher<QPair<qint64, qint64>>(this);
    connect(watcher, &QFutureWatcher<QPair<qint64, qint64>>::finished, this,
            [this, watcher, generation, selectedServerId, serverType, contentSummary,
             backupCount, worldCount]() {
        const auto sizes = watcher->result();
        watcher->deleteLater();
        if (generation != m_refreshGeneration || selectedServerId != m_serverId) return;
        if (!m_serverManager) return;
        const auto currentServer = m_serverManager->getServer(selectedServerId);
        if (!currentServer) return;
        m_storageValue->setText(ServerFiles::formatByteSize(sizes.first));
        if (m_summaryLabel) {
            m_summaryLabel->setText(
                tr("<b>Version:</b> %1 &nbsp;&bull;&nbsp; <b>Type:</b> %2 &nbsp;&bull;&nbsp; <b>Port:</b> %3<br>"
                   "<b>Content:</b> %4 &nbsp;&bull;&nbsp; <b>Backups:</b> %5 &nbsp;&bull;&nbsp; <b>Worlds:</b> %6 (%7)")
                    .arg(currentServer->version().toHtmlEscaped(), serverType.toHtmlEscaped())
                    .arg(currentServer->port())
                    .arg(contentSummary.toHtmlEscaped())
                    .arg(backupCount)
                    .arg(worldCount)
                    .arg(ServerFiles::formatByteSize(sizes.second).toHtmlEscaped()));
        }
    });
    watcher->setFuture(QtConcurrent::run([serverDirectoryPath, worldPaths]() {
        qint64 worldSize = 0;
        for (const QString &worldPath : worldPaths) worldSize += ServerFiles::directorySize(worldPath);
        return qMakePair(ServerFiles::directorySize(serverDirectoryPath), worldSize);
    }));
}

void ServerOverviewTab::refreshLiveStatistics()
{
    const auto setInactive = [this](const QString &text) {
        if (!m_cpuBar || !m_ramBar || !m_updatedLabel) return;
        m_cpuBar->setValue(0);
        m_cpuBar->setFormat(text);
        m_ramBar->setValue(0);
        m_ramBar->setFormat(text);
        m_updatedLabel->setText(text);
    };

    if (!m_serverManager || m_serverId.isEmpty()) {
        m_uptimeValue->setText("-");
        setInactive(tr("No server selected"));
        return;
    }
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) {
        m_uptimeValue->setText("-");
        setInactive(tr("Server unavailable"));
        return;
    }

    const QString status = ServerPageStyle::statusText(server->status());
    const QColor statusColor = ServerPageStyle::statusColor(server->status());
    m_statusValue->setText(status);
    m_statusValue->setStyleSheet(QString("color: %1;").arg(statusColor.name()));

    if (server->status() != ServerStatus::Running || server->processId() <= 0) {
        m_uptimeValue->setText(tr("Not running"));
        m_liveStatisticsPid = 0;
        m_previousProcessCpuMs = 0;
        m_previousSampleTimeMs = 0;
        setInactive(tr("Not running"));
        return;
    }

    const QDateTime startedAt = server->startedAt();
    if (startedAt.isValid()) {
        const qint64 seconds = startedAt.secsTo(QDateTime::currentDateTime());
        const qint64 hours = seconds / 3600;
        const qint64 minutes = (seconds % 3600) / 60;
        m_uptimeValue->setText(hours > 0 ? tr("%1h %2m").arg(hours).arg(minutes) : tr("%1m").arg(minutes));
    } else {
        m_uptimeValue->setText(tr("This session"));
    }

#ifdef Q_OS_WIN
    ServerProcessSnapshot snapshot;
    const qint64 processId = ServerProcessStats::workProcessId(server->processId());
    if (!ServerProcessStats::read(processId, &snapshot)) {
        ServerHealthInput input;
        input.running = true;
        const ServerHealthAssessment assessment = ServerDiagnostics::assessHealth(input);
        setInactive(assessment.state == ServerHealthState::Unavailable
                        ? tr("Process data unavailable") : tr("Not running"));
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    double cpu = -1.0;
    if (m_liveStatisticsPid == processId && m_previousSampleTimeMs > 0 && now > m_previousSampleTimeMs) {
        const int cores = qMax(1, QThread::idealThreadCount());
        cpu = (snapshot.cpuMilliseconds - m_previousProcessCpuMs) * 100.0
            / (now - m_previousSampleTimeMs) / cores;
    }
    const auto setBarColor = [](QProgressBar *bar, const QColor &color) {
        bar->setStyleSheet(QString("QProgressBar { text-align: center; } QProgressBar::chunk { background: %1; }")
            .arg(color.name()));
    };
    if (m_cpuBar && m_ramBar && m_updatedLabel) {
        const ServerDataStore &records = m_serverManager->dataStore();
        const QString &monitoring = ServerDataGroup::Monitoring;
        const int cpuWarning = records.value(server->id(), monitoring, "cpuWarning", 85).toInt();
        const int ramWarning = records.value(server->id(), monitoring, "ramWarning", 90).toInt();
        const int diskWarningGb = records.value(server->id(), monitoring, "diskWarningGb", 2).toInt();
        const QStorageInfo storage(server->serverDirectory());
        ServerHealthInput healthInput;
        healthInput.running = true;
        healthInput.processMetricsAvailable = true;
        healthInput.cpuPercent = cpu;
        healthInput.workingSetBytes = snapshot.workingSetBytes;
        healthInput.configuredRamMiB = qMax(1, server->maxMemory());
        healthInput.diskAvailable = storage.isValid();
        healthInput.diskBytesAvailable = storage.bytesAvailable();
        healthInput.cpuWarningPercent = cpuWarning;
        healthInput.ramWarningPercent = ramWarning;
        healthInput.diskWarningGiB = diskWarningGb;
        const ServerHealthAssessment health = ServerDiagnostics::assessHealth(healthInput);
        const auto metricColor = [](ServerMetricLevel level) {
            switch (level) {
                case ServerMetricLevel::Warning: return QColor("#e53935");
                case ServerMetricLevel::Approaching: return QColor("#f9a825");
                case ServerMetricLevel::Normal: return QColor("#43a047");
                case ServerMetricLevel::Unavailable: return QColor("#607d8b");
            }
            return QColor("#607d8b");
        };
        const double visibleCpu = qMax(0.0, cpu);
        m_cpuBar->setRange(0, 100);
        m_cpuBar->setValue(qBound(0, qRound(visibleCpu), 100));
        m_cpuBar->setFormat(cpu < 0.0 ? tr("Measuring...") : QString::number(visibleCpu, 'f', 1) + "%");
        setBarColor(m_cpuBar, metricColor(health.cpu));

        const int ramMegabytes = qMax(0, qRound(snapshot.workingSetBytes / (1024.0 * 1024.0)));
        const int configuredRam = healthInput.configuredRamMiB;
        m_ramBar->setRange(0, qMax(configuredRam, ramMegabytes));
        m_ramBar->setValue(ramMegabytes);
        m_ramBar->setFormat(tr("%1 MB / %2 MB configured").arg(ramMegabytes).arg(configuredRam));
        setBarColor(m_ramBar, metricColor(health.ram));
        QStringList warnings;
        if (health.cpu == ServerMetricLevel::Warning)
            warnings << tr("CPU %1%").arg(cpu, 0, 'f', 1);
        if (health.ram == ServerMetricLevel::Warning)
            warnings << tr("RAM %1%").arg(health.ramPercent, 0, 'f', 1);
        if (health.disk == ServerMetricLevel::Warning) {
            warnings << tr("Disk %1 GB free").arg(storage.bytesAvailable() / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
        }
        if (health.state == ServerHealthState::Warning) {
            m_updatedLabel->setText(tr("Warning: %1").arg(warnings.join(", ")));
            m_updatedLabel->setStyleSheet("color: #e53935; font-weight: 600;");
        } else if (health.state == ServerHealthState::Measuring) {
            m_updatedLabel->setText(tr("Measuring live process usage..."));
            m_updatedLabel->setStyleSheet("color: palette(mid);");
        } else {
            m_updatedLabel->setText(tr("Updated %1").arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
            m_updatedLabel->setStyleSheet("color: palette(mid);");
        }
    }
    m_liveStatisticsPid = processId;
    m_previousProcessCpuMs = snapshot.cpuMilliseconds;
    m_previousSampleTimeMs = now;
#else
    setInactive(tr("Live process metrics are available on Windows."));
#endif
}
