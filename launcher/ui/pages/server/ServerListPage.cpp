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
#include "server/ServerFiles.h"
#include "server/ServerModpackInstaller.h"
#include "server/ServerPlayerAccess.h"
#include "ui/dialogs/CreateServerDialog.h"
#include "ui/dialogs/NewInstanceDialog.h"
#include "ui/dialogs/ProgressDialog.h"
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
#include "FileSystem.h"
#include "InstanceList.h"
#include "InstanceTask.h"
#include "server/ServerMemory.h"
#include "settings/INIFile.h"
#include "logs/Privacy.h"
#include <QMessageBox>
#include <QAbstractButton>
#include <QFile>
#include <QDir>
#include <QDialog>
#include <QVBoxLayout>
#include <QColor>
#include <QStyle>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QTreeWidget>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QInputDialog>
#include <QSpinBox>
#include <QEvent>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimeEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QGroupBox>
#include <QHeaderView>
#include <QTabBar>
#include <QTabWidget>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <utility>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

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
            QSettings settings;
            const QStringList history = settings.value(
                QString("ServerAutomation/%1/history").arg(serverId)).toStringList();
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

void ServerListPage::onCreateServer()
{
    if (!m_serverManager) {
        return;
    }

    CreateServerDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QString name = dialog.serverName();
    QString version = dialog.mcVersion();
    QString type = dialog.serverType();

    auto server = m_serverManager->createServer(name, version, type, QString());
    if (server) {
        server->setPort(dialog.port());
        server->setMinMemory(dialog.minMemory());
        server->setMaxMemory(dialog.maxMemory());
        server->setEulaAccepted(dialog.eulaAccepted());
        m_serverManager->save();

        updateServerList();

        // Select the newly created server
        for (int i = 0; i < ui->serverList->count(); i++) {
            if (ui->serverList->item(i)->data(Qt::UserRole).toString() == server->id()) {
                ui->serverList->setCurrentRow(i);
                break;
            }
        }

        QMessageBox::information(this, tr("Server Created"),
                                 tr("Server '%1' has been created.\n"
                                    "Click 'Start' to install the server software and launch it.")
                                 .arg(name));
    } else {
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to create server."));
    }
}

void ServerListPage::onStartServer()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (server && !server->isRunning() && server->status() != ServerStatus::Downloading) {
        if (!server->eulaAccepted()) {
            QMessageBox consent(this);
            consent.setWindowTitle(tr("Minecraft EULA"));
            consent.setIcon(QMessageBox::Information);
            consent.setTextFormat(Qt::RichText);
            consent.setText(tr("Starting this server requires accepting the "
                               "<a href=\"https://aka.ms/MinecraftEULA\">Minecraft End User License Agreement</a>."));
            consent.setInformativeText(tr("Accept the EULA for this server and continue?"));
            auto *acceptButton = consent.addButton(tr("Accept and Start"), QMessageBox::AcceptRole);
            consent.addButton(QMessageBox::Cancel);
            consent.exec();
            if (consent.clickedButton() != acceptButton) {
                return;
            }
            server->setEulaAccepted(true);
            if (!m_serverManager->save()) {
                server->setEulaAccepted(false);
                QMessageBox::warning(this, tr("Minecraft EULA"), tr("Could not save the EULA acceptance."));
                return;
            }
        }
        if (server->start()) {
            appendConsoleOutput("[INFO] Starting server...");
            updateUI();
            updateServerList();
        } else if (server->status() == ServerStatus::Error) {
            QString details;
            const QStringList logLines =
                server->consoleLog().split('\n', Qt::SkipEmptyParts);
            if (!logLines.isEmpty()) {
                details = logLines.constLast().trimmed();
                details.remove(QRegularExpression("^\\[ERROR\\]\\s*"));
            }
            const QStringList missing = m_contentTab->missingDependencies(server.get());
            if (!missing.isEmpty()) {
                m_contentTab->offerDependencyRepair(
                    missing,
                    details.isEmpty()
                        ? tr("The server could not start because required mods are missing.")
                        : details);
            } else {
                QMessageBox::warning(
                    this, tr("Server Could Not Start"),
                    details.isEmpty()
                        ? tr("The server could not be started. Open the Console tab for details.")
                        : details);
            }
        }
    }
}

void ServerListPage::onStopServer()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (server && server->hasPendingCrashRestart()) {
        server->cancelPendingCrashRestart();
        updateUI();
    } else if (server && server->status() == ServerStatus::Downloading) {
        server->cancelDownload();
        updateUI();
        updateServerList();
    } else if (server && (server->status() == ServerStatus::Running
                          || server->status() == ServerStatus::Starting)) {
        appendConsoleOutput("[INFO] Stopping server...");
        server->stop();
        updateUI();
        updateServerList();
    }
}

void ServerListPage::onRestartServer()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (server && (server->status() == ServerStatus::Running
                   || server->status() == ServerStatus::Starting)) {
        appendConsoleOutput("[INFO] Restarting server...");
        server->restart();
        updateUI();
        updateServerList();
    }
}

void ServerListPage::onDeleteServer()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        return;
    }

    QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Delete Server"),
        tr("Move server '%1' and all of its files to the recycle bin?\n"
           "You can undo this action while the launcher remains open.").arg(server->name()),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        if (!m_serverManager->deleteServer(m_selectedServerId)) {
            QMessageBox permanentDeletePrompt(
                QMessageBox::Warning,
                tr("Delete Server"),
                tr("The server could not be moved to the recycle bin."),
                QMessageBox::NoButton,
                this);
            permanentDeletePrompt.setInformativeText(
                tr("Permanently delete '%1' and all of its files instead?\n\n"
                   "This cannot be undone.").arg(server->name()));
            auto *cancelButton = permanentDeletePrompt.addButton(QMessageBox::Cancel);
            auto *permanentDeleteButton = permanentDeletePrompt.addButton(
                tr("Delete Permanently"), QMessageBox::DestructiveRole);
            permanentDeletePrompt.setDefaultButton(qobject_cast<QPushButton *>(cancelButton));
            permanentDeletePrompt.exec();
            if (permanentDeletePrompt.clickedButton() != permanentDeleteButton) {
                return;
            }

            if (!m_serverManager->deleteServerPermanently(m_selectedServerId)) {
                QMessageBox::warning(
                    this,
                    tr("Delete Server"),
                    tr("J Launcher could not delete the server folder. Close any program using its files, then try again.\n\n"
                       "Folder: %1").arg(QDir::toNativeSeparators(server->serverDirectory())));
                return;
            }
        }

        // Keep the live connection intact until deletion has actually succeeded.
        if (m_currentConnectedServer && m_currentConnectedServer->id() == m_selectedServerId) {
            disconnect(m_currentConnectedServer.get(), nullptr, this, nullptr);
            m_currentConnectedServer = nullptr;
        }
        setSelectedServerId(QString());
        m_consoleTab->clear();
        updateServerList();
        updateUI();
    }
}

void ServerListPage::onUndoDelete()
{
    if (!m_serverManager) {
        return;
    }
    QString restoredId;
    if (!m_serverManager->restoreLastDeletedServer(&restoredId)) {
        QMessageBox::warning(this, tr("Undo Delete"), tr("The deleted server could not be restored."));
        return;
    }
    setSelectedServerId(restoredId);
    updateServerList();
    updateUI();
}

namespace {
// The pack is downloaded into a staging folder rather than an instance, so the
// memory hint the pack author exported has to be read from the staged
// instance.cfg instead of a live instance's settings.
int stagedProviderRecommendation(const QString &instanceRoot)
{
    INIFile config;
    if (!config.loadFile(FS::PathCombine(instanceRoot, QStringLiteral("instance.cfg")))) {
        return 0;
    }
    return ServerMemory::resolveProviderRecommendation(
        config.get(QStringLiteral("OverrideMemory"), false).toBool(),
        config.get(QStringLiteral("MaxMemAlloc"), 0).toInt(),
        config.get(QStringLiteral("ExportRecommendedRAM"), 0).toInt());
}

}  // namespace

void ServerListPage::onInstallModpack()
{
    if (!m_serverManager || !APPLICATION->instances()) {
        return;
    }

    const QString initialGroup =
        APPLICATION->settings()->get("LastUsedGroupForNewInstance").toString();
    NewInstanceDialog dialog(initialGroup, QString(), {}, this,
                             NewInstanceDialog::Mode::ServerModpack);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    APPLICATION->settings()->set("LastUsedGroupForNewInstance", dialog.instGroup());

    unique_qobject_ptr<InstanceTask> creationTask(dialog.extractTask());
    if (!creationTask) {
        QMessageBox::warning(this, tr("Create from Modpack"),
                             tr("No modpack was selected."));
        return;
    }

    // The pack is downloaded into a staging folder and the server is built
    // straight from it. Nothing is committed to the instance list, so making a
    // server no longer makes a client instance the user did not ask for.
    const QString stagingPath =
        APPLICATION->instances()->getStagedInstancePath(creationTask->targetDir());
    if (stagingPath.isEmpty()) {
        QMessageBox::critical(
            this, tr("Create from Modpack"),
            tr("A temporary folder for the download could not be created."));
        return;
    }
    creationTask->setStagingPath(stagingPath);
    creationTask->setParentSettings(APPLICATION->settings());

    const QString packName = creationTask->name();

    int progressResult = QDialog::Rejected;
    {
        ProgressDialog progress(this);
        progress.setWindowTitle(tr("Downloading Modpack"));
        progress.setSkipButton(true, tr("Abort"));
        progressResult = progress.execWithTask(creationTask.get());
    }

    if (!creationTask->wasSuccessful()) {
        const QString reason = creationTask->failReason();
        FS::deletePath(stagingPath);
        if (reason.isEmpty() && progressResult == QDialog::Rejected) {
            // Aborted from the progress dialog. The user knows; nothing was kept.
            return;
        }
        QMessageBox::critical(
            this, tr("Create from Modpack"),
            reason.isEmpty()
                ? tr("The modpack could not be downloaded.")
                : tr("The modpack could not be downloaded:\n%1")
                      .arg(Privacy::sanitizeText(reason)));
        return;
    }

    QString selectedServerRoot;
    const QStringList serverRootChoices =
        ServerModpackInstaller::publishedServerRootChoices(stagingPath);
    if (serverRootChoices.size() > 1) {
        bool accepted = false;
        selectedServerRoot = QInputDialog::getItem(
            this, tr("Choose Server Pack Folder"),
            tr("This pack contains multiple possible server folders. Choose the folder containing the server files:"),
            serverRootChoices, 0, false, &accepted);
        if (!accepted || selectedServerRoot.isEmpty()) {
            FS::deletePath(stagingPath);
            QMessageBox::information(
                this, tr("Server Creation Cancelled"),
                tr("No server was created, and the downloaded files were removed."));
            return;
        }
    }

    const ServerModpackProfile profile =
        ServerModpackInstaller::profileFromInstanceRoot(stagingPath);
    const QString gameRoot =
        ServerModpackInstaller::gameRootForInstanceRoot(stagingPath);
    const QString stagingParent = m_serverManager->serversRoot();
    const QStringList knownClientOnlyHashes =
        ServerModpackInstaller::knownClientOnlyHashes();
    QProgressDialog preparationProgress(
        tr("Preparing server files from %1...").arg(packName), QString(), 0, 0, this);
    preparationProgress.setWindowTitle(tr("Creating Server"));
    preparationProgress.setCancelButton(nullptr);
    preparationProgress.setWindowModality(Qt::ApplicationModal);
    preparationProgress.setMinimumDuration(0);
    preparationProgress.show();

    QFutureWatcher<PreparedServerModpack> watcher;
    QEventLoop waitLoop;
    connect(&watcher, &QFutureWatcher<PreparedServerModpack>::finished,
            &waitLoop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(
        [profile, stagingPath, gameRoot, stagingParent, selectedServerRoot,
         knownClientOnlyHashes]() {
            return ServerModpackInstaller::prepareMatchingServer(
                profile, stagingPath, gameRoot, stagingParent, selectedServerRoot,
                knownClientOnlyHashes);
        }));
    if (!watcher.isFinished()) {
        waitLoop.exec();
    }
    PreparedServerModpack prepared = watcher.result();
    const ServerModpackInstallResult result =
        ServerModpackInstaller::installPreparedServer(
            m_serverManager, std::move(prepared), packName + tr(" Server"),
            stagedProviderRecommendation(stagingPath), 0);
    preparationProgress.close();

    // The staged pack has served its purpose either way: on success its files
    // are already copied into the server, on failure there is nothing to keep.
    FS::deletePath(stagingPath);
    if (!result.isValid()) {
        const QString reason = Privacy::sanitizeText(result.error);
        const QString recovery =
            result.failureCategory == ServerModpackFailureCategory::CompatibilityMetadata
            ? tr("Try another pack version or report this problem to the pack author.")
            : tr("Review the reason, then try again.");
        QMessageBox failureDialog(this);
        failureDialog.setWindowTitle(tr("Create from Modpack"));
        failureDialog.setIcon(QMessageBox::Critical);
        failureDialog.setTextFormat(Qt::PlainText);
        failureDialog.setText(tr("Could not create server."));
        failureDialog.setInformativeText(
            tr("Reason:\n%1\n\n%2\n\n%3")
                .arg(reason, recovery,
                     tr("The downloaded files were removed. Nothing was added to your instances.")));
        failureDialog.setDetailedText(
            tr("Category: %1\nStage: %2")
                .arg(serverModpackFailureCategoryName(result.failureCategory),
                     serverModpackFailureStageName(result.failureStage)));
        failureDialog.exec();
        return;
    }

    setSelectedServerId(result.serverId);
    updateServerList();
    updateUI();

    const auto createdServer = m_serverManager->getServer(result.serverId);
    const bool neededServerSoftware =
        createdServer && !createdServer->hasInstalledLaunchTarget();
    const bool softwarePreparationAccepted =
        !neededServerSoftware || createdServer->prepareServerSoftware();

    QString details = tr("Created stopped server \"%1\" from modpack \"%2\". "
                         "No client instance was created.")
                          .arg(m_serverManager->getServer(result.serverId)->name(),
                               packName);
    if (result.hasDedicatedServerPack
        && result.provider.startsWith(QStringLiteral("ftb"), Qt::CaseInsensitive)) {
        details += tr("\n\nServer source: prepared by the official FTB server installer.");
    } else if (result.hasDedicatedServerPack) {
        details += tr("\n\nServer source: dedicated server pack supplied by %1.")
                       .arg(result.provider);
    } else {
        details += tr("\n\nServer source: derived from the %1 client pack because no dedicated "
                      "server pack was supplied. The server loader performs the final version "
                      "and runtime checks when it starts.")
                       .arg(result.provider);
    }
    if (!result.skippedClientFiles.isEmpty()) {
        details += tr("\n\nExcluded %1 client-only file(s) from the server.")
                       .arg(result.skippedClientFiles.size());
    }
    if (!result.missingFiles.isEmpty() || !result.dependencyRequirements.isEmpty()) {
        QStringList requirements;
        for (const QString &path : result.missingFiles) {
            requirements.append(tr("Missing file: %1").arg(path));
        }
        requirements.append(result.dependencyRequirements);
        details += tr("\n\nRequired before startup:\n%1\n\nAdd the missing files from their trusted provider, then start the server again. J Launcher will recheck them locally.")
                       .arg(requirements.join(QStringLiteral("\n")));
    }
    if (createdServer) {
        details += tr("\n\nServer memory: %1 MB minimum / %2 MB maximum (automatic). "
                       "You can change this later in Server Settings.")
                       .arg(createdServer->minMemory())
                       .arg(createdServer->maxMemory());
    }
    if (neededServerSoftware && softwarePreparationAccepted) {
        details += tr("\n\nServer software preparation started. J Launcher will download and verify the exact Minecraft and loader files while the server remains stopped. Follow progress in Console.");
    } else if (neededServerSoftware) {
        details += tr("\n\nServer software could not start downloading. The server was kept; review Console, configure the required Java runtime, and retry from Server Software.");
    }
    if (!result.warnings.isEmpty()) {
        details += tr("\n\nCompatibility note:\n%1")
                       .arg(result.warnings.join(QStringLiteral("\n")));
    }
    if (!result.missingDependencyIds.isEmpty()) {
        QMessageBox ready(this);
        ready.setWindowTitle(tr("Modpack Ready - Dependencies Needed"));
        ready.setIcon(QMessageBox::Warning);
        ready.setText(tr("The server was created, but required mods are missing."));
        ready.setInformativeText(
            details
            + tr("\n\nMissing mod IDs: %1\nModpack source: %2\n\n"
                 "Choose Find Missing Mods to search the same trusted catalogs. "
                 "J Launcher will select compatible files, include their declared "
                 "dependencies, and install them directly into this server's mods folder.")
                  .arg(result.missingDependencyIds.join(QStringLiteral(", ")),
                       result.provider));
        auto *findButton = ready.addButton(tr("Find Missing Mods"),
                                           QMessageBox::AcceptRole);
        ready.addButton(QMessageBox::Close);
        ready.exec();
        if (ready.clickedButton() == findButton) {
            m_contentTab->findMissingMods(result.missingDependencyIds);
        }
    } else {
        QMessageBox::information(this, tr("Modpack Ready"), details);
    }
}

void ServerListPage::restoreBackupAt(const QString &backupPath)
{
    if (!m_backupsTab->restoreBackupAt(backupPath)) {
        QMessageBox::warning(this, tr("Update Backup Missing"), tr("The latest rollback backup is not available in the backup list."));
    }
}

void ServerListPage::onSettingsClicked()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Server Settings - %1").arg(server->name()));
    dialog.setMinimumSize(700, 680);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    ServerSettingsPage *settingsPage = new ServerSettingsPage(&dialog);
    settingsPage->setServer(server);
    layout->addWidget(settingsPage);

    connect(settingsPage, &ServerSettingsPage::settingsSaved, &dialog, [this, &dialog]() {
        if (m_serverManager && !m_serverManager->save()) {
            QMessageBox::warning(
                &dialog,
                tr("Could Not Save Server Settings"),
                tr("The server options were written, but the server settings record could not be saved. "
                   "Check that the J Launcher data folder is writable, then try again."));
            return;
        }
        updateServerList();
        dialog.accept();
    });
    connect(settingsPage, &ServerSettingsPage::cancelled, &dialog, &QDialog::reject);

    dialog.exec();
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

