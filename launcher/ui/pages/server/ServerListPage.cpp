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
#include "Application.h"
#include "server/ServerManager.h"
#include "server/ServerInstance.h"
#include "server/ServerDiagnostics.h"
#include "server/ServerModpackInstaller.h"
#include "ui/pages/server/ServerAutomationTab.h"
#include "ui/pages/server/ServerBackupsTab.h"
#include "ui/pages/server/ServerConsoleTab.h"
#include "ui/pages/server/ServerContentTab.h"
#include "ui/pages/server/ServerFilesTab.h"
#include "ui/pages/server/ServerOverviewTab.h"
#include "ui/pages/server/ServerPageStyle.h"
#include "ui/pages/server/ServerPlayersTab.h"
#include "ui/pages/server/ServerUpdatesTab.h"
#include "ui/pages/server/ServerSettingsPage.h"
#include "logs/Privacy.h"
#include <QMessageBox>
#include <QAbstractButton>
#include <QFile>
#include <QVBoxLayout>
#include <QColor>
#include <QStyle>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QEvent>
#include <QElapsedTimer>
#include <QTabWidget>
#include <QSignalBlocker>
#include <utility>

using ServerPageStyle::applyMutedLabelPalette;
using ServerPageStyle::launcherIcon;

namespace {

QIcon serverCardIcon()
{
    return launcherIcon("server", QStyle::SP_ComputerIcon);
}

void showServerFailureDialog(QWidget *parent,
                             const std::shared_ptr<ServerInstance> &server,
                             const QString &message, const QString &rawLog)
{
    const QString likelyCause = ServerDiagnostics::crashCauseExplanation(
        ServerDiagnostics::classifyCrash(rawLog));
    const QString reportedError = Privacy::sanitizeText(
        ServerDiagnostics::crashRelevantLine(rawLog), 1000);

    QMessageBox dialog(parent);
    dialog.setWindowTitle(QObject::tr("Server Failure"));
    dialog.setIcon(QMessageBox::Critical);
    dialog.setText(QObject::tr("The server stopped before it was ready or ended unexpectedly."));

    QString explanation = QObject::tr("Likely cause: %1").arg(likelyCause);
    if (!reportedError.isEmpty()) {
        explanation += QObject::tr("\n\nServer reported:\n%1").arg(reportedError);
    }
    const QStringList suspectFiles = server
        ? ServerDiagnostics::suspectedModFiles(server->modsDirectory(), rawLog) : QStringList();
    if (!suspectFiles.isEmpty()) {
        explanation += QObject::tr(
            "\n\nThe log identifies this installed mod as the likely cause: %1. "
            "You can disable it on this server and retry; the client copy is not changed.")
                           .arg(QFileInfo(suspectFiles.constFirst()).fileName());
    }
    explanation += QObject::tr("\n\nChoose Show Details to view the server version, Java runtime, installed content, and final log lines.");
    dialog.setInformativeText(explanation);
    dialog.setDetailedText(ServerDiagnostics::structuredCrashDetails(*server, message, rawLog));
    dialog.setStandardButtons(QMessageBox::Close);
    QAbstractButton *disableAndRetryButton = nullptr;
    if (!suspectFiles.isEmpty()) {
        disableAndRetryButton = dialog.addButton(
            QObject::tr("Disable and Retry"), QMessageBox::ActionRole);
    }
    dialog.exec();

    if (disableAndRetryButton && dialog.clickedButton() == disableAndRetryButton) {
        QString cacheError;
        if (server && !server->invalidateContentCaches(&cacheError)) {
            QMessageBox::warning(parent, QObject::tr("Could Not Retry Server"), cacheError);
            return;
        }
        QStringList failures;
        QStringList disabledNames;
        for (const QString& source : suspectFiles) {
            const QString destination = source + QStringLiteral(".disabled");
            if (QFileInfo::exists(destination) || !QFile::rename(source, destination)) {
                failures << QFileInfo(source).fileName();
            } else {
                ServerModpackInstaller::markKnownClientOnlyFile(destination);
                disabledNames << QFileInfo(source).fileName();
            }
        }
        if (!failures.isEmpty()) {
            QMessageBox::warning(
                parent, QObject::tr("Could Not Disable Mod"),
                QObject::tr("These files could not be disabled: %1")
                    .arg(failures.join(QStringLiteral(", "))));
            return;
        }
        if (!disabledNames.isEmpty() && server) {
            QTimer::singleShot(0, server.get(), [server]() { server->start(); });
        }
    }
}
}

ServerListPage::ServerListPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ServerListPage)
{
    QElapsedTimer performanceTimer;
    performanceTimer.start();
    ui->setupUi(this);
    m_serverTrackingContext = new QObject(this);
    ui->serverListPanel->setMinimumWidth(264);
    ui->serverListPanel->setMaximumWidth(328);
    ui->serverSplitter->setSizes(QList<int>() << 292 << 888);
    ui->serverList->setSpacing(6);
    applyServerVisualHierarchy();

    // Do not present a creation action as preselected when this page opens.
    setFocusPolicy(Qt::StrongFocus);
    setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, this, [this]() { setFocus(Qt::OtherFocusReason); });

    m_undoDeleteButton = new QPushButton(tr("Undo Delete"), this);
    m_undoDeleteButton->setObjectName(QStringLiteral("undoDeleteButton"));
    m_undoDeleteButton->setIcon(launcherIcon("refresh", QStyle::SP_ArrowBack));
    m_undoDeleteButton->setEnabled(false);
    m_undoDeleteButton->hide();
    ui->headerLayout->insertWidget(ui->headerLayout->count() - 2, m_undoDeleteButton);

    m_emptyServerListWidget = new QWidget(ui->serverListPanel);
    m_emptyServerListWidget->setObjectName("serverEmptyState");
    auto *emptyListLayout = new QVBoxLayout(m_emptyServerListWidget);
    emptyListLayout->addStretch();
    auto *emptyListIcon = new QLabel(m_emptyServerListWidget);
    emptyListIcon->setPixmap(serverCardIcon().pixmap(58, 58));
    emptyListIcon->setAlignment(Qt::AlignCenter);
    emptyListLayout->addWidget(emptyListIcon);
    auto *emptyListTitle = new QLabel(tr("No servers yet"), m_emptyServerListWidget);
    emptyListTitle->setObjectName(QStringLiteral("emptyServerListTitle"));
    QFont emptyTitleFont = emptyListTitle->font();
    emptyTitleFont.setBold(true);
    emptyListTitle->setFont(emptyTitleFont);
    emptyListTitle->setAlignment(Qt::AlignCenter);
    emptyListLayout->addWidget(emptyListTitle);
    auto *emptyListHint = new QLabel(tr("Create a server to manage it from the launcher."), m_emptyServerListWidget);
    emptyListHint->setObjectName(QStringLiteral("emptyServerListHint"));
    emptyListHint->setWordWrap(true);
    emptyListHint->setAlignment(Qt::AlignCenter);
    applyMutedLabelPalette(emptyListHint);
    emptyListLayout->addWidget(emptyListHint);
    auto *emptyListActions = new QHBoxLayout;
    emptyListActions->setAlignment(Qt::AlignCenter);
    auto *emptyListCreateButton = new QPushButton(tr("Create Server"), m_emptyServerListWidget);
    emptyListCreateButton->setObjectName(QStringLiteral("emptyCreateServerButton"));
    emptyListCreateButton->setProperty("role", "primary");
    emptyListCreateButton->setIcon(launcherIcon("new", QStyle::SP_FileDialogNewFolder));
    emptyListCreateButton->setIconSize(QSize(20, 20));
    emptyListActions->addWidget(emptyListCreateButton);
    auto *emptyListCreateFromModpackButton = new QPushButton(tr("Create from Modpack"), m_emptyServerListWidget);
    emptyListCreateFromModpackButton->setObjectName(QStringLiteral("emptyCreateFromModpackButton"));
    emptyListCreateFromModpackButton->setIcon(launcherIcon("centralmods", QStyle::SP_FileDialogNewFolder));
    emptyListCreateFromModpackButton->setIconSize(QSize(20, 20));
    const QSize emptyListActionSize = emptyListCreateButton->sizeHint()
                                         .expandedTo(emptyListCreateFromModpackButton->sizeHint())
                                         .expandedTo(QSize(132, 36));
    emptyListCreateButton->setFixedSize(emptyListActionSize);
    emptyListCreateFromModpackButton->setFixedSize(emptyListActionSize);
    emptyListActions->addWidget(emptyListCreateFromModpackButton);
    emptyListLayout->addLayout(emptyListActions);
    emptyListLayout->addStretch();
    ui->serverListPanelLayout->addWidget(m_emptyServerListWidget, 1);
    connect(emptyListCreateButton, &QPushButton::clicked, this, &ServerListPage::onCreateServer);
    connect(emptyListCreateFromModpackButton, &QPushButton::clicked, this, &ServerListPage::onInstallModpack);

    m_emptyDetailWidget = new QWidget(ui->serverDetailPanel);
    m_emptyDetailWidget->setObjectName("serverDetailEmptyState");
    auto *emptyDetailLayout = new QVBoxLayout(m_emptyDetailWidget);
    emptyDetailLayout->addStretch();
    auto *emptyDetailTitle = new QLabel(tr("Select a server"), m_emptyDetailWidget);
    emptyDetailTitle->setFont(emptyTitleFont);
    emptyDetailTitle->setAlignment(Qt::AlignCenter);
    emptyDetailLayout->addWidget(emptyDetailTitle);
    auto *emptyDetailHint =
        new QLabel(tr("Choose a server from the list to view status, console, files, backups, and settings."),
                   m_emptyDetailWidget);
    emptyDetailHint->setWordWrap(true);
    emptyDetailHint->setAlignment(Qt::AlignCenter);
    applyMutedLabelPalette(emptyDetailHint);
    emptyDetailLayout->addWidget(emptyDetailHint);
    emptyDetailLayout->addStretch();
    ui->serverDetailLayout->insertWidget(1, m_emptyDetailWidget, 1);
    m_overviewTab = new ServerOverviewTab(ui->serverTabs);
    m_liveStatisticsTimer.setInterval(2000);
    connect(&m_liveStatisticsTimer, &QTimer::timeout, this, [this]() {
        if (ui->serverTabs->currentWidget() == m_overviewTab) m_overviewTab->refreshLiveStatistics();
    });
    m_liveStatisticsTimer.start();

    m_consoleTab = new ServerConsoleTab(ui->serverTabs);

    m_contentTab = new ServerContentTab(ui->serverTabs);
    connect(m_contentTab, &ServerContentTab::consoleMessage, this, &ServerListPage::appendConsoleOutput);
    connect(m_contentTab, &ServerContentTab::contentChanged, m_overviewTab, &ServerOverviewTab::refresh);
    connect(m_contentTab, &ServerContentTab::contentLabelChanged, this, [this](const QString &label) {
        ui->serverTabs->setTabText(ui->serverTabs->indexOf(m_contentTab), label);
    });

    m_maintenanceTab = new QTabWidget(ui->serverTabs);
    m_maintenanceTab->setDocumentMode(true);
    m_updatesTab = new ServerUpdatesTab(m_maintenanceTab);
    connect(m_updatesTab, &ServerUpdatesTab::consoleMessage, this, &ServerListPage::appendConsoleOutput);
    m_filesTab = new ServerFilesTab(ui->serverTabs);
    connect(m_filesTab, &ServerFilesTab::consoleMessage, this, &ServerListPage::appendConsoleOutput);
    connect(m_filesTab, &ServerFilesTab::serverRecordChanged, this, [this]() {
        updateServerList();
        updateSelectedServerInfo();
    });
    connect(m_filesTab, &ServerFilesTab::serverCreated, this, [this](const QString &serverId) {
        setSelectedServerId(serverId);
        updateServerList();
    });
    m_backupsTab = new ServerBackupsTab(ui->serverTabs);
    connect(m_backupsTab, &ServerBackupsTab::backupsChanged, m_overviewTab, &ServerOverviewTab::refresh);
    connect(m_backupsTab, &ServerBackupsTab::serverFilesRestored, this, [this]() {
        m_contentTab->refresh();
        m_filesTab->refresh();
        m_overviewTab->refresh();
    });
    connect(m_updatesTab, &ServerUpdatesTab::backupsChanged, m_backupsTab, &ServerBackupsTab::refresh);
    connect(m_updatesTab, &ServerUpdatesTab::actionsChanged, this, &ServerListPage::updateUI);
    connect(m_updatesTab, &ServerUpdatesTab::serverRecordChanged, this, &ServerListPage::updateServerList);
    connect(m_updatesTab, &ServerUpdatesTab::installedContentChanged, m_contentTab, &ServerContentTab::refresh);
    connect(m_updatesTab, &ServerUpdatesTab::restoreBackupRequested, this, &ServerListPage::restoreBackupAt);
    m_maintenanceTab->addTab(m_updatesTab, tr("Updates"));

    m_playersTab = new ServerPlayersTab(ui->serverTabs);
    ui->serverTabs->insertTab(ui->serverTabs->indexOf(ui->settingsTab), m_playersTab, tr("Players"));

    m_automationTab = new ServerAutomationTab(m_maintenanceTab);
    connect(m_automationTab, &ServerAutomationTab::automationRun, this, &ServerListPage::updateUI);
    m_maintenanceTab->addTab(m_automationTab, tr("Automation & Health"));
    m_maintenanceTab->setObjectName(QStringLiteral("maintenanceTabs"));
    setupServerNavigation();
    connect(ui->serverTabs, &QTabWidget::currentChanged, this, [this](int) {
        QTimer::singleShot(0, this, &ServerListPage::refreshCurrentServerTab);
    });
    connect(m_maintenanceTab, &QTabWidget::currentChanged, this, [this](int) {
        if (ui->serverTabs->currentWidget() == m_maintenanceTab) {
            QTimer::singleShot(0, this, &ServerListPage::refreshCurrentServerTab);
        }
    });
    connect(ui->createServerButton, &QPushButton::clicked, this, &ServerListPage::onCreateServer);
    connect(ui->createFromModpackButton, &QPushButton::clicked, this, &ServerListPage::onInstallModpack);
    connect(ui->startServerButton, &QPushButton::clicked, this, &ServerListPage::onStartServer);
    connect(ui->stopServerButton, &QPushButton::clicked, this, &ServerListPage::onStopServer);
    connect(ui->restartServerButton, &QPushButton::clicked, this, &ServerListPage::onRestartServer);
    connect(ui->deleteServerButton, &QPushButton::clicked, this, &ServerListPage::onDeleteServer);
    connect(m_undoDeleteButton, &QPushButton::clicked, this, &ServerListPage::onUndoDelete);
    connect(ui->serverList, &QListWidget::currentRowChanged, this, &ServerListPage::onServerSelectionChanged);
    connect(ui->serverSearchInput, &QLineEdit::textChanged, this, [this](const QString &) { updateServerList(); });

    protectInputFromWheel(this);

    // Buttons must remain keyboard-accessible. Disable dialog-default styling
    // without removing them from the Tab focus chain, then give the page itself
    // the initial focus so no action appears preselected when the window opens.
    for (QPushButton *button : findChildren<QPushButton *>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    setFocusPolicy(Qt::StrongFocus);
    QTimer::singleShot(0, this, [this]() { setFocus(Qt::OtherFocusReason); });

    updateUI();
    if (qEnvironmentVariableIsSet("JLAUNCHER_PROFILE_UI")) {
        qInfo() << "[Performance] Constructed ServerListPage in"
                << performanceTimer.elapsed() << "ms";
    }
}

bool ServerListPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Wheel
        && (qobject_cast<QAbstractSpinBox *>(watched) || qobject_cast<QComboBox *>(watched))) {
        event->accept();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void ServerListPage::protectInputFromWheel(QWidget *scope)
{
    if (!scope) return;
    for (QAbstractSpinBox *input : scope->findChildren<QAbstractSpinBox *>()) input->installEventFilter(this);
    for (QComboBox *input : scope->findChildren<QComboBox *>()) input->installEventFilter(this);
}

ServerListPage::~ServerListPage()
{
    delete ui;
}

void ServerListPage::setServerManager(ServerManager *manager)
{
    QElapsedTimer performanceTimer;
    performanceTimer.start();
    if (m_serverTrackingContext) {
        m_serverTrackingContext->deleteLater();
    }
    m_serverTrackingContext = new QObject(this);
    m_serverManager = manager;
    m_automationTab->setServerManager(manager);
    m_playersTab->setServerManager(manager);
    m_updatesTab->setServerManager(manager);
    m_backupsTab->setServerManager(manager);
    m_filesTab->setServerManager(manager);
    m_consoleTab->setServerManager(manager);
    m_contentTab->setServerManager(manager);
    m_overviewTab->setServerManager(manager);
    if (m_serverManager) {
        for (const auto &server : m_serverManager->getAllServers()) {
            attachServerTracking(server);
        }
        connect(m_serverManager, &ServerManager::serverAdded, m_serverTrackingContext, [this](const QString &id) {
            if (m_serverManager) attachServerTracking(m_serverManager->getServer(id));
        });
        connect(m_serverManager, &ServerManager::automationRecorded, m_serverTrackingContext,
                [this](const QString &serverId) {
            if (m_selectedServerId != serverId) {
                return;
            }
            m_automationTab->refreshHistory();
            const QStringList history =
                m_serverManager->dataStore().list(serverId, ServerDataGroup::Automation, "history");
            if (!history.isEmpty() && history.first().contains(QStringLiteral("BACKUP:"))) {
                m_backupsTab->refresh();
                m_overviewTab->refresh();
            }
        });
        connect(m_serverManager, &ServerManager::serverDiagnosticsRecorded,
                m_serverTrackingContext, [this](const QString &serverId) {
            if (m_selectedServerId == serverId) m_automationTab->refreshDiagnostics();
        });
        connect(m_serverManager, &ServerManager::playerHistoryRecorded,
                m_serverTrackingContext, [this](const QString &serverId) {
            if (m_selectedServerId == serverId
                && ui->serverTabs->currentWidget() == m_playersTab) {
                m_playersTab->refresh();
            }
        });
    }
    updateServerList();
    if (qEnvironmentVariableIsSet("JLAUNCHER_PROFILE_UI")) {
        qInfo() << "[Performance] Populated ServerListPage in"
                << performanceTimer.elapsed() << "ms";
    }
}

void ServerListPage::attachServerTracking(const std::shared_ptr<ServerInstance> &server)
{
    if (!server || !m_serverTrackingContext) return;
    const QString serverId = server->id();
    connect(server.get(), &ServerInstance::serverSoftwareDownloadFinished,
            m_serverTrackingContext,
            [this, server, serverId](const QString &targetVersion, bool success,
                                     bool cancelled, const QString &errorMessage) {
        if (success && m_serverManager) {
            m_serverManager->save();
        }
        if (m_selectedServerId != serverId) return;
        m_updatesTab->showSoftwareDownloadResult(server, targetVersion, success, cancelled, errorMessage);
        if (success) {
            updateSelectedServerInfo();
            updateServerList();
        }
        updateUI();
    });
}

void ServerListPage::onServerSelectionChanged()
{
    // Disconnect previously selected server
    if (m_currentConnectedServer) {
        disconnect(m_currentConnectedServer.get(), nullptr, this, nullptr);
        m_currentConnectedServer = nullptr;
    }

    int row = ui->serverList->currentRow();
    if (row >= 0) {
        setSelectedServerId(ui->serverList->item(row)->data(Qt::UserRole).toString());

        if (m_serverManager) {
            m_currentConnectedServer = m_serverManager->getServer(m_selectedServerId);
            if (m_currentConnectedServer) {
                m_consoleTab->showLog(m_currentConnectedServer->consoleLog());

                // Connect signals
                connect(m_currentConnectedServer.get(), &ServerInstance::outputReceived, this, [this](const QString &line) {
                    appendConsoleOutput(line);
                });
                connect(m_currentConnectedServer.get(), &ServerInstance::errorReceived, this, [this](const QString &line) {
                    appendConsoleOutput("[ERROR] " + line);
                });
                connect(m_currentConnectedServer.get(), &ServerInstance::statusChanged, this, [this]() {
                    updateUI();
                    updateServerList();
                    QTimer::singleShot(0, this, &ServerListPage::refreshCurrentServerTab);
                });
                connect(m_currentConnectedServer.get(),
                        &ServerInstance::crashRestartPendingChanged, this,
                        [this](bool) { updateUI(); });
                connect(m_currentConnectedServer.get(), &ServerInstance::serverCrashed, this,
                        [this](const QString &message, const QString &details) {
                    if (m_selectedServerId.isEmpty()) return;
                    const auto failedServer = m_currentConnectedServer;
                    const QString failedServerId = m_selectedServerId;
                    QTimer::singleShot(0, this,
                                       [this, failedServer, failedServerId, message, details]() {
                        if (!failedServer || m_selectedServerId != failedServerId) return;
                        showServerFailureDialog(this, failedServer, message, details);
                    });
                });
            }
        }
    } else {
        setSelectedServerId(QString());
        m_consoleTab->clear();
    }
    updateSelectedServerInfo();
    updateUI();
    QTimer::singleShot(0, this, &ServerListPage::refreshCurrentServerTab);
    m_updatesTab->clearUpdateList();

    for (int i = 0; i < ui->serverList->count(); ++i) {
        QWidget *card = ui->serverList->itemWidget(ui->serverList->item(i));
        if (!card) continue;
        card->setProperty("selected", i == ui->serverList->currentRow());
        card->style()->unpolish(card);
        card->style()->polish(card);
    }
}

void ServerListPage::onServerStatusChanged(ServerInstance *server)
{
    if (server && server->id() == m_selectedServerId) {
        updateUI();
    }
}

void ServerListPage::updateUI()
{
    bool hasSelection = !m_selectedServerId.isEmpty();
    ServerStatus status = ServerStatus::Stopped;
    bool hasPendingCrashRestart = false;

    if (hasSelection && m_serverManager) {
        auto server = m_serverManager->getServer(m_selectedServerId);
        if (server) {
            status = server->status();
            hasPendingCrashRestart = server->hasPendingCrashRestart();
        }
    }

    const bool canStart = status == ServerStatus::Stopped || status == ServerStatus::Error;
    const bool canStop = status == ServerStatus::Starting || status == ServerStatus::Running
        || status == ServerStatus::Downloading
        || (status == ServerStatus::Error && hasPendingCrashRestart);
    const bool canEditFiles = status == ServerStatus::Stopped || status == ServerStatus::Error;
    bool supportsContentBrowser = false;
    if (hasSelection && m_serverManager) {
        const auto server = m_serverManager->getServer(m_selectedServerId);
        if (server) {
            const ServerContentType contentType = server->contentType();
            supportsContentBrowser = contentType != ServerContentType::None;
        }
    }

    ui->startServerButton->setEnabled(hasSelection && canStart);
    ui->stopServerButton->setEnabled(hasSelection && canStop);
    ui->stopServerButton->setText(status == ServerStatus::Downloading
                                      ? tr("Cancel Download")
                                      : hasPendingCrashRestart ? tr("Cancel Restart") : tr("Stop"));
    ui->stopServerButton->setIcon(status == ServerStatus::Downloading
                                      ? launcherIcon("status-bad", QStyle::SP_DialogCancelButton)
                                      : launcherIcon("status-bad", QStyle::SP_MediaStop));
    ui->restartServerButton->setEnabled(
        hasSelection && (status == ServerStatus::Starting || status == ServerStatus::Running));
    ui->startServerButton->setProperty("role", canStart ? "primary" : "secondary");
    ui->stopServerButton->setProperty("role", canStop ? "primary" : "secondary");
    for (QPushButton *button : {ui->startServerButton, ui->stopServerButton}) {
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
    const bool canUndoDelete = m_serverManager && m_serverManager->hasDeletedServer();
    m_undoDeleteButton->setEnabled(canUndoDelete);
    m_undoDeleteButton->setVisible(canUndoDelete);
    m_emptyDetailWidget->setVisible(!hasSelection);
    ui->serverCommandBar->setVisible(hasSelection);
    ui->serverWorkspacePanel->setVisible(hasSelection);
    ui->selectedServerInfoLabel->setVisible(hasSelection);
    ui->startServerButton->setVisible(hasSelection);
    ui->stopServerButton->setVisible(hasSelection);
    ui->restartServerButton->setVisible(hasSelection);
    ui->deleteServerButton->setVisible(hasSelection);
    ui->serverTabs->setVisible(hasSelection);
    ui->serverTabs->setTabEnabled(ui->serverTabs->indexOf(m_backupsTab), hasSelection);
    ui->serverTabs->setTabEnabled(ui->serverTabs->indexOf(m_contentTab), supportsContentBrowser);
    {
        const auto server = hasSelection && m_serverManager
            ? m_serverManager->getServer(m_selectedServerId) : nullptr;
        const bool pluginServer = server && server->contentType() == ServerContentType::Plugin;
        ui->serverTabs->setTabText(ui->serverTabs->indexOf(m_contentTab),
                                   pluginServer ? tr("Plugins") : tr("Mods"));
    }
    m_filesTab->updateActions();
    ui->deleteServerButton->setEnabled(hasSelection && canEditFiles);
    ui->settingsScrollArea->setEnabled(hasSelection && canEditFiles);
    m_overviewTab->updateActions();
    m_contentTab->updateActions();
    m_backupsTab->updateActions();
    m_consoleTab->updateActions();
    ui->serverTabs->setTabEnabled(ui->serverTabs->indexOf(m_maintenanceTab), hasSelection);
    m_updatesTab->updateActions();
    ui->serverTabs->setTabEnabled(ui->serverTabs->indexOf(m_playersTab), hasSelection);
    m_playersTab->updateActions();
    m_automationTab->updateActions();
    syncServerNavigation();
}

void ServerListPage::updateServerList()
{
    // Rebuilding the cards must not temporarily clear the selected server or
    // disturb the page the operator is currently using.
    const QString savedId = m_selectedServerId;
    QWidget *activeServerTab = ui->serverTabs->currentWidget();
    QWidget *activeMaintenanceTab = m_maintenanceTab ? m_maintenanceTab->currentWidget() : nullptr;
    const QSignalBlocker selectionBlocker(ui->serverList);

    ui->serverList->clear();

    if (!m_serverManager) {
        return;
    }

    auto servers = m_serverManager->getAllServers();
    const bool hasServers = !servers.isEmpty();
    m_emptyServerListWidget->setVisible(!hasServers);
    ui->serverList->setVisible(hasServers);
    ui->serverSearchInput->setVisible(hasServers);
    ui->serverListTitle->setVisible(hasServers);
    ui->createFromModpackButton->setVisible(hasServers);
    ui->createServerButton->setVisible(hasServers);

    // With no servers, use the full content width for the single create prompt
    // instead of showing a second empty-state illustration beside it.
    ui->serverDetailPanel->setVisible(hasServers);
    if (hasServers) {
        ui->serverListPanel->setMinimumWidth(264);
        ui->serverListPanel->setMaximumWidth(328);
        ui->serverSplitter->setSizes(QList<int>() << 292 << 888);
    } else {
        ui->serverListPanel->setMinimumWidth(0);
        ui->serverListPanel->setMaximumWidth(QWIDGETSIZE_MAX);
    }
    const QString filter = ui->serverSearchInput->text().trimmed();
    int running = 0;
    int visible = 0;
    int selectRow = -1;
    for (int i = 0; i < servers.size(); i++) {
        auto &server = servers[i];
        if (server->status() == ServerStatus::Running) ++running;
        const QString searchable = QString("%1 %2 %3").arg(server->name(), server->version(), server->loaderType());
        if (!filter.isEmpty() && !searchable.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        QListWidgetItem *item = new QListWidgetItem();
        item->setData(Qt::UserRole, server->id());
        item->setSizeHint(QSize(0, 82));

        auto *card = new QWidget(ui->serverList);
        card->setObjectName(QStringLiteral("serverCard"));
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(11, 8, 11, 8);
        cardLayout->setSpacing(4);

        auto *topRow = new QHBoxLayout();
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(7);
        auto *serverIconLabel = new QLabel(card);
        serverIconLabel->setPixmap(serverCardIcon().pixmap(24, 24));
        serverIconLabel->setFixedSize(26, 26);
        serverIconLabel->setAlignment(Qt::AlignCenter);
        serverIconLabel->setToolTip(tr("Minecraft server"));
        auto *nameLabel = new QLabel(server->name(), card);
        QFont nameFont = nameLabel->font();
        nameFont.setBold(true);
        nameFont.setPointSize(nameFont.pointSize() + 1);
        nameLabel->setFont(nameFont);
        auto *statusLabel = new QLabel(ServerPageStyle::statusText(server->status()), card);
        const QColor statusColor = ServerPageStyle::statusColor(server->status());
        auto *statusDot = new QLabel(card);
        statusDot->setFixedSize(8, 8);
        statusDot->setStyleSheet(QString("background: %1; border-radius: 4px;").arg(statusColor.name()));
        applyMutedLabelPalette(statusLabel);
        topRow->addWidget(serverIconLabel);
        topRow->addWidget(nameLabel);
        topRow->addStretch();
        topRow->addWidget(statusDot);
        topRow->addWidget(statusLabel);
        cardLayout->addLayout(topRow);

        const QString loader = server->loaderType().isEmpty() ? tr("Vanilla") : server->loaderType();
        auto *detailsLabel = new QLabel(tr("%1  •  %2  •  Port %3").arg(loader, server->version()).arg(server->port()), card);
        applyMutedLabelPalette(detailsLabel);
        detailsLabel->setContentsMargins(33, 0, 0, 0);
        cardLayout->addWidget(detailsLabel);

        ui->serverList->addItem(item);
        ui->serverList->setItemWidget(item, card);

        if (server->id() == savedId) {
            selectRow = visible;
        }
        ++visible;
    }

    if (auto *emptyTitle = m_emptyServerListWidget->findChild<QLabel *>(QStringLiteral("emptyServerListTitle"))) {
        emptyTitle->setText(hasServers ? tr("No matching servers") : tr("No servers yet"));
    }
    if (auto *emptyHint = m_emptyServerListWidget->findChild<QLabel *>(QStringLiteral("emptyServerListHint"))) {
        emptyHint->setText(hasServers ? tr("Try a different name, version, or server type.")
                                      : tr("Create a server, or build one from a modpack."));
    }
    if (auto *emptyCreate = m_emptyServerListWidget->findChild<QPushButton *>(QStringLiteral("emptyCreateServerButton"))) {
        emptyCreate->setVisible(!hasServers);
    }
    if (auto *emptyCreateFromModpack = m_emptyServerListWidget->findChild<QPushButton *>(
            QStringLiteral("emptyCreateFromModpackButton"))) {
        emptyCreateFromModpack->setVisible(!hasServers);
    }
    const bool hasVisibleServers = visible > 0;
    m_emptyServerListWidget->setVisible(!hasVisibleServers);
    ui->serverList->setVisible(hasVisibleServers);

    const bool savedServerExists = !savedId.isEmpty() && m_serverManager->getServer(savedId);
    const bool selectedServerHidden = savedServerExists && selectRow < 0;
    const QString serverCount = servers.size() == 1
        ? tr("1 server")
        : tr("%1 servers").arg(servers.size());
    QString statsText = filter.isEmpty()
        ? tr("%1 • %2 running").arg(serverCount).arg(running)
        : tr("%1 of %2 shown • %3 running").arg(visible).arg(serverCount).arg(running);
    if (selectedServerHidden) {
        statsText += tr(" • selected server hidden by filter");
    }
    ui->serverStatsLabel->setText(statsText);
    ui->serverSearchInput->setToolTip(selectedServerHidden
        ? tr("The selected server remains active in the workspace but is hidden by this filter.")
        : tr("Filter servers by name, version, or server type."));

    // Restore selection. On first open, select the first visible server so the
    // dashboard immediately has useful information instead of an empty state.
    if (selectRow >= 0) {
        ui->serverList->setCurrentRow(selectRow);
    } else if ((!savedServerExists || savedId.isEmpty()) && visible > 0) {
        ui->serverList->setCurrentRow(0);
    } else {
        ui->serverList->setCurrentRow(-1);
    }

    const QString finalId = ui->serverList->currentItem()
        ? ui->serverList->currentItem()->data(Qt::UserRole).toString() : QString();
    if (!selectedServerHidden
        && (finalId != m_selectedServerId
            || (!finalId.isEmpty() && !m_currentConnectedServer))) {
        onServerSelectionChanged();
    } else {
        for (int index = 0; index < ui->serverList->count(); ++index) {
            QWidget *card = ui->serverList->itemWidget(ui->serverList->item(index));
            if (!card) continue;
            card->setProperty("selected", index == ui->serverList->currentRow());
            card->style()->unpolish(card);
            card->style()->polish(card);
        }
    }

    if (activeServerTab && ui->serverTabs->indexOf(activeServerTab) >= 0
        && ui->serverTabs->isTabEnabled(ui->serverTabs->indexOf(activeServerTab))) {
        ui->serverTabs->setCurrentWidget(activeServerTab);
    }
    if (activeMaintenanceTab && m_maintenanceTab
        && m_maintenanceTab->indexOf(activeMaintenanceTab) >= 0) {
        m_maintenanceTab->setCurrentWidget(activeMaintenanceTab);
    }
}

void ServerListPage::refreshCurrentServerTab()
{
    QWidget *page = ui->serverTabs->currentWidget();
    if (page == m_overviewTab) {
        m_overviewTab->refresh();
        m_overviewTab->refreshLiveStatistics();
    } else if (page == m_contentTab) {
        m_contentTab->refresh();
    } else if (page == m_playersTab) {
        m_playersTab->refresh();
    } else if (page == m_filesTab) {
        m_filesTab->refresh();
    } else if (page == m_backupsTab) {
        m_backupsTab->refresh();
    } else if (page == ui->settingsTab) {
        rebuildSettingsPage();
    } else if (page == m_maintenanceTab && m_maintenanceTab->currentWidget() == m_automationTab) {
        m_automationTab->refresh();
    }
}

void ServerListPage::rebuildSettingsPage()
{
    while (QLayoutItem *layoutItem = ui->settingsContentLayout->takeAt(0)) {
        delete layoutItem->widget();
        delete layoutItem;
    }
    m_embeddedSettingsPage = nullptr;

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        auto *placeholder = new QLabel(tr("Select a stopped server to edit its settings."), ui->settingsContentContainer);
        placeholder->setAlignment(Qt::AlignCenter);
        ui->settingsContentLayout->addWidget(placeholder);
        return;
    }

    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        return;
    }

    auto *settingsPage = new ServerSettingsPage(ui->settingsContentContainer);
    m_embeddedSettingsPage = settingsPage;
    settingsPage->setServer(server);
    ui->settingsContentLayout->addWidget(settingsPage);
    protectInputFromWheel(settingsPage);
    for (QPushButton *button : settingsPage->findChildren<QPushButton *>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    connect(settingsPage, &ServerSettingsPage::settingsSaved, this, [this]() {
        if (m_serverManager && !m_serverManager->save()) {
            QMessageBox::warning(
                this,
                tr("Could Not Save Server Settings"),
                tr("The server options were written, but the server settings record could not be saved. "
                   "Check that the J Launcher data folder is writable, then try again."));
            return;
        }

        // The card displays the name and port from the saved server record.
        // Refresh it as well as the detail header, while updateServerList()
        // preserves the selected server and active tab.
        updateServerList();
        updateSelectedServerInfo();
    });
    connect(settingsPage, &ServerSettingsPage::cancelled, this, [settingsPage, server]() {
        settingsPage->setServer(server);
    });
}

void ServerListPage::updateSelectedServerInfo()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        ui->detailTitleLabel->setText(tr("Select a server"));
        ui->selectedServerInfoLabel->setText(tr("Select a server to view its details and controls."));
        return;
    }
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        ui->detailTitleLabel->setText(tr("Select a server"));
        ui->selectedServerInfoLabel->setText(tr("Select a server to view its details and controls."));
        return;
    }
    QString type = server->loaderType().isEmpty() ? tr("Vanilla") : server->loaderType();
    if (!type.isEmpty()) type[0] = type[0].toUpper();
    const auto memoryText = [](int memoryMiB) {
        return memoryMiB % 1024 == 0
            ? QObject::tr("%1 GB").arg(memoryMiB / 1024)
            : QObject::tr("%1 MB").arg(memoryMiB);
    };
    ui->detailTitleLabel->setText(server->name());
    ui->selectedServerInfoLabel->setText(tr("%1 %2  •  Port %3  •  %4–%5  •  <b>%6</b>")
        .arg(type.toHtmlEscaped(), server->version().toHtmlEscaped()).arg(server->port())
        .arg(memoryText(server->minMemory()), memoryText(server->maxMemory()))
        .arg(ServerPageStyle::statusText(server->status())));
}

void ServerListPage::setSelectedServerId(const QString &serverId)
{
    m_selectedServerId = serverId;
    m_automationTab->setServerId(serverId);
    m_playersTab->setServerId(serverId);
    m_updatesTab->setServerId(serverId);
    m_backupsTab->setServerId(serverId);
    m_filesTab->setServerId(serverId);
    m_consoleTab->setServerId(serverId);
    m_contentTab->setServerId(serverId);
    m_overviewTab->setServerId(serverId);
}

void ServerListPage::appendConsoleOutput(const QString &text)
{
    m_consoleTab->appendLine(text);
}
