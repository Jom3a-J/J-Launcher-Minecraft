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
#include "archive/ArchiveReader.h"
#include "Application.h"
#include "server/ServerManager.h"
#include "server/ServerInstance.h"
#include "server/ServerDownloader.h"
#include "server/ServerDiagnostics.h"
#include "server/ServerContentUpdater.h"
#include "server/ServerFiles.h"
#include "server/ServerProcessStats.h"
#include "server/ServerModpackInstaller.h"
#include "server/ServerPlayerAccess.h"
#include "ui/dialogs/CreateServerDialog.h"
#include "ui/dialogs/NewInstanceDialog.h"
#include "ui/dialogs/ProgressDialog.h"
#include "ui/dialogs/ResourceDownloadDialog.h"
#include "ui/pages/modplatform/ResourcePage.h"
#include "ui/pages/server/ServerAutomationTab.h"
#include "ui/pages/server/ServerPageStyle.h"
#include "ui/pages/server/ServerPlayersTab.h"
#include "ui/pages/server/ServerUpdatesTab.h"
#include "ui/pages/server/ServerSettingsPage.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/ModFolderModel.h"
#include "modplatform/flame/FlameAPI.h"
#include "BuildConfig.h"
#include "FileSystem.h"
#include "InstanceList.h"
#include "InstanceTask.h"
#include "QObjectPtr.h"
#include "server/ServerMemory.h"
#include "settings/INIFile.h"
#include "settings/INISettingsObject.h"
#include "tasks/ConcurrentTask.h"
#include "logs/Privacy.h"
#include <QMessageBox>
#include <QAbstractButton>
#include <QFileDialog>
#include <QFile>
#include <QDesktopServices>
#include <QUrl>
#include <QUrlQuery>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>
#include <QDialog>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QColor>
#include <QStyle>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QLocale>
#include <QMenu>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QClipboard>
#include <QApplication>
#include <QInputDialog>
#include <QSpinBox>
#include <QStorageInfo>
#include <QEvent>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimeEdit>
#include <QThread>
#include <QTemporaryDir>
#include <QProgressBar>
#include <QProgressDialog>
#include <QGroupBox>
#include <QHeaderView>
#include <QScrollArea>
#include <QTabBar>
#include <QTabWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
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

QIcon serverFileIcon(const QFileInfo &file)
{
    if (file.isDir()) {
        const QString name = file.fileName().toLower();
        if (name == "world" || name == "world_nether" || name == "world_the_end") {
            return launcherIcon("worlds", QStyle::SP_DirIcon);
        }
        return launcherIcon("viewfolder", QStyle::SP_DirIcon);
    }

    const QString suffix = file.suffix().toLower();
    const QString name = file.fileName().toLower();
    if (suffix == "jar") {
        return launcherIcon(name.contains("server") || name.contains("paper") || name.contains("purpur")
                                ? "server" : "loadermods", QStyle::SP_FileIcon);
    }
    if (suffix == "zip" || suffix == "rar" || suffix == "7z" || suffix == "gz") {
        return launcherIcon("jarmods", QStyle::SP_FileIcon);
    }
    if (suffix == "properties" || suffix == "json" || suffix == "toml" || suffix == "yml" || suffix == "yaml") {
        return launcherIcon("settings", QStyle::SP_FileIcon);
    }
    if (suffix == "log") return launcherIcon("log", QStyle::SP_FileIcon);
    if (suffix == "png" || suffix == "jpg" || suffix == "jpeg") return launcherIcon("screenshots", QStyle::SP_FileIcon);
    if (suffix == "java") return launcherIcon("java", QStyle::SP_FileIcon);
    return launcherIcon("notes", QStyle::SP_FileIcon);
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
    ui->overviewInfoLabel->hide();
    ui->overviewDetailsGroup->hide();
    ui->overviewBackupsCard->hide();
    ui->overviewWorldsCard->hide();
    ui->overviewStorageCard->hide();
    ui->overviewMemoryCard->setTitle(tr("Uptime"));
    ui->overviewContentCard->setTitle(tr("Storage"));

    auto *summaryGroup = new QGroupBox(tr("Server Summary"), ui->overviewTab);
    summaryGroup->setObjectName("overviewSummaryGroup");
    auto *summaryLayout = new QVBoxLayout(summaryGroup);
    m_overviewSummaryLabel = new QLabel(tr("No server data available."), summaryGroup);
    m_overviewSummaryLabel->setObjectName("overviewSummaryLabel");
    m_overviewSummaryLabel->setWordWrap(true);
    m_overviewSummaryLabel->setTextFormat(Qt::RichText);
    m_overviewSummaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summaryLayout->addWidget(m_overviewSummaryLabel);
    ui->overviewTabLayout->insertWidget(2, summaryGroup);

    auto *liveUsageGroup = new QGroupBox(tr("Live Resource Use"), ui->overviewTab);
    liveUsageGroup->setObjectName("overviewLiveUsageGroup");
    auto *liveUsageLayout = new QGridLayout(liveUsageGroup);
    liveUsageLayout->addWidget(new QLabel(tr("CPU"), liveUsageGroup), 0, 0);
    m_overviewCpuBar = new QProgressBar(liveUsageGroup);
    m_overviewCpuBar->setRange(0, 100);
    liveUsageLayout->addWidget(m_overviewCpuBar, 0, 1);
    liveUsageLayout->addWidget(new QLabel(tr("RAM"), liveUsageGroup), 1, 0);
    m_overviewRamBar = new QProgressBar(liveUsageGroup);
    liveUsageLayout->addWidget(m_overviewRamBar, 1, 1);
    m_overviewUpdatedLabel = new QLabel(tr("Waiting for live data."), liveUsageGroup);
    m_overviewUpdatedLabel->setAlignment(Qt::AlignRight);
    applyMutedLabelPalette(m_overviewUpdatedLabel);
    liveUsageLayout->addWidget(m_overviewUpdatedLabel, 2, 0, 1, 2);
    ui->overviewTabLayout->insertWidget(3, liveUsageGroup);
    m_liveStatisticsTimer.setInterval(2000);
    connect(&m_liveStatisticsTimer, &QTimer::timeout, this, [this]() {
        if (ui->serverTabs->currentWidget() == ui->overviewTab) refreshLiveStatistics();
    });
    m_liveStatisticsTimer.start();

    m_exportProfileButton = new QPushButton(tr("Export Profile"), this);
    m_importProfileButton = new QPushButton(tr("Import Profile"), this);
    ui->filesActions->insertWidget(2, m_exportProfileButton);
    ui->filesActions->insertWidget(3, m_importProfileButton);

    auto *consoleHeader = new QHBoxLayout();
    auto *consoleTitle = new QLabel(tr("Live output"), ui->consoleTab);
    consoleTitle->setObjectName("consoleTitleLabel");
    QFont consoleTitleFont = consoleTitle->font();
    consoleTitleFont.setBold(true);
    consoleTitle->setFont(consoleTitleFont);
    auto *consoleHint = new QLabel(tr("Server output and commands"), ui->consoleTab);
    consoleHint->setObjectName("consoleHintLabel");
    consoleHeader->addWidget(consoleTitle);
    consoleHeader->addWidget(consoleHint);
    consoleHeader->addStretch();
    ui->consoleTabLayout->insertLayout(0, consoleHeader);

    auto *consoleTools = new QHBoxLayout();
    m_consoleSearchInput = new QLineEdit(ui->consoleTab);
    m_consoleSearchInput->setObjectName("consoleSearchInput");
    m_consoleSearchInput->setPlaceholderText(tr("Search console output…"));
    m_findConsoleButton = new QPushButton(tr("Find Next"), ui->consoleTab);
    m_findConsoleButton->setObjectName("findConsoleButton");
    m_copyConsoleErrorsButton = new QPushButton(tr("Copy Errors"), ui->consoleTab);
    m_copyConsoleErrorsButton->setObjectName("copyConsoleErrorsButton");
    m_pauseConsoleScrollCheck = new QCheckBox(tr("Pause auto-scroll"), ui->consoleTab);
    m_clearConsoleButton = new QPushButton(tr("Clear View"), ui->consoleTab);
    m_exportConsoleButton = new QPushButton(tr("Export Log"), ui->consoleTab);
    consoleTools->addWidget(m_consoleSearchInput, 1);
    consoleTools->addWidget(m_findConsoleButton);
    consoleTools->addWidget(m_copyConsoleErrorsButton);
    consoleTools->addWidget(m_pauseConsoleScrollCheck);
    consoleTools->addWidget(m_clearConsoleButton);
    consoleTools->addWidget(m_exportConsoleButton);
    ui->consoleTabLayout->insertLayout(2, consoleTools);
    ui->consoleOutput->document()->setMaximumBlockCount(5000);

    ui->installedContentTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->installedContentTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->installedContentTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->installedContentTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    m_maintenanceTab = new QTabWidget(ui->serverTabs);
    m_maintenanceTab->setDocumentMode(true);
    m_updatesTab = new ServerUpdatesTab(m_maintenanceTab);
    connect(m_updatesTab, &ServerUpdatesTab::consoleMessage, this, &ServerListPage::appendConsoleOutput);
    connect(m_updatesTab, &ServerUpdatesTab::backupsChanged, this, &ServerListPage::refreshServerBackups);
    connect(m_updatesTab, &ServerUpdatesTab::actionsChanged, this, &ServerListPage::updateUI);
    connect(m_updatesTab, &ServerUpdatesTab::serverRecordChanged, this, &ServerListPage::updateServerList);
    connect(m_updatesTab, &ServerUpdatesTab::installedContentChanged, this, &ServerListPage::refreshInstalledContent);
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
    connect(ui->openFolderButton, &QPushButton::clicked, this, &ServerListPage::onOpenFolder);
    connect(ui->browseModsButton, &QPushButton::clicked, this, &ServerListPage::onBrowseMods);
    connect(ui->addLocalContentButton, &QPushButton::clicked, this, &ServerListPage::onAddLocalContent);
    connect(ui->toggleInstalledButton, &QPushButton::clicked, this, &ServerListPage::onToggleInstalledContent);
    connect(ui->removeInstalledButton, &QPushButton::clicked, this, &ServerListPage::onRemoveInstalledContent);
    connect(ui->openContentFolderButton, &QPushButton::clicked, this, &ServerListPage::onOpenContentFolder);
    connect(ui->createBackupButton, &QPushButton::clicked, this, &ServerListPage::onCreateBackup);
    connect(ui->restoreBackupButton, &QPushButton::clicked, this, &ServerListPage::onRestoreBackup);
    connect(ui->openBackupsFolderButton, &QPushButton::clicked, this, &ServerListPage::onOpenBackupsFolder);
    connect(ui->removeBackupButton, &QPushButton::clicked, this, &ServerListPage::onRemoveBackup);
    connect(ui->refreshBackupsButton, &QPushButton::clicked, this, &ServerListPage::refreshServerBackups);
    connect(ui->backupsTree, &QTreeWidget::currentItemChanged, this, [this]() { updateUI(); });
    connect(ui->refreshInstalledButton, &QPushButton::clicked, this, &ServerListPage::refreshInstalledContent);
    connect(ui->refreshOverviewButton, &QPushButton::clicked, this, [this]() {
        refreshOverview();
        refreshLiveStatistics();
    });
    connect(ui->installedContentTree, &QTreeWidget::currentItemChanged, this, [this]() { updateUI(); });
    connect(ui->refreshFilesButton, &QPushButton::clicked, this, &ServerListPage::refreshServerFiles);
    connect(ui->importPackButton, &QPushButton::clicked, this, &ServerListPage::onImportServerPack);
    connect(ui->sendCommandButton, &QPushButton::clicked, this, &ServerListPage::onSendCommand);
    connect(m_exportProfileButton, &QPushButton::clicked, this, &ServerListPage::onExportProfile);
    connect(m_importProfileButton, &QPushButton::clicked, this, &ServerListPage::onImportProfile);
    connect(m_findConsoleButton, &QPushButton::clicked, this, &ServerListPage::onFindConsole);
    connect(m_copyConsoleErrorsButton, &QPushButton::clicked, this, &ServerListPage::onCopyConsoleErrors);
    connect(m_clearConsoleButton, &QPushButton::clicked, ui->consoleOutput, &QPlainTextEdit::clear);
    connect(m_exportConsoleButton, &QPushButton::clicked, this, [this]() {
        QString suggestedName = tr("server-console.log");
        if (m_serverManager && !m_selectedServerId.isEmpty()) {
            const auto server = m_serverManager->getServer(m_selectedServerId);
            if (server) {
                suggestedName = server->name() + "-console.log";
            }
        }
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export Server Console"), suggestedName, tr("Log files (*.log);;Text files (*.txt)"));
        if (path.isEmpty()) {
            return;
        }
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Text)
            || output.write(ui->consoleOutput->toPlainText().toUtf8()) < 0) {
            QMessageBox::warning(this, tr("Export Console"), tr("Could not write the selected log file."));
        }
    });
    connect(m_consoleSearchInput, &QLineEdit::returnPressed, this, &ServerListPage::onFindConsole);
    connect(ui->serverList, &QListWidget::currentRowChanged, this, &ServerListPage::onServerSelectionChanged);
    connect(ui->serverSearchInput, &QLineEdit::textChanged, this, [this](const QString &) { updateServerList(); });
    connect(ui->serverFilesTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        populateServerFileItem(item);
    });
    connect(ui->serverFilesTree, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem *item, int) {
        if (!item) return;
        const QFileInfo file(item->data(0, Qt::UserRole).toString());
        if (file.isDir()) {
            item->setExpanded(!item->isExpanded());
        } else if (file.exists()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(file.absoluteFilePath()));
        }
    });

    protectInputFromWheel(this);

    // Allow pressing Enter in command input to send
    connect(ui->commandInput, &QLineEdit::returnPressed, this, &ServerListPage::onSendCommand);

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
        {ui->overviewTab, tr("Home")},
        {ui->consoleTab, tr("Console")},
        {ui->installedContentTab, tr("Mods")},
        {m_playersTab, tr("Players")},
        {ui->filesTab, tr("Files")},
        {ui->backupsTab, tr("Backups")},
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

    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->overviewTab),
                                   tr("Server status, live resource use, and quick statistics."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->consoleTab),
                                   tr("Live server output and commands."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->installedContentTab),
                                   tr("Installed mods or plugins and downloads."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(m_playersTab),
                                   tr("Player access, operator levels, bans, and history."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->filesTab),
                                   tr("Browse server files and import or export server profiles."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->backupsTab),
                                   tr("Create, restore, and manage complete server backups."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(m_maintenanceTab),
                                   tr("Software updates, automation, monitoring thresholds, and crash diagnostics."));
    ui->serverTabs->setTabToolTip(ui->serverTabs->indexOf(ui->settingsTab),
                                   tr("Server properties, Java, memory, and launch settings."));
    ui->serverTabs->setCurrentWidget(ui->overviewTab);
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
    ui->sendCommandButton->setIcon(launcherIcon("launch", QStyle::SP_ArrowForward));

    ui->createServerButton->setProperty("role", "primary");
    ui->startServerButton->setProperty("role", "primary");
    ui->deleteServerButton->setProperty("role", "danger");
    ui->sendCommandButton->setProperty("role", "primary");

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
                refreshServerBackups();
                refreshOverview();
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
            const QStringList missing = missingServerDependencies(server.get());
            if (!missing.isEmpty()) {
                offerDependencyRepair(
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
        ui->consoleOutput->clear();
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

void ServerListPage::onOpenFolder()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (server) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(server->serverDirectory()));
    }
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

// A mod's identifier inside its jar is not always what catalogs list it under:
// "cloth-config2" is published as "Cloth Config". Searching a looser form of the
// identifier matches far more of them. The exact identifier stays in the message
// so the user can still confirm they picked the right mod.
QString dependencySearchTerm(const QString &dependencyId)
{
    QString term = dependencyId;
    term.replace(QLatin1Char('_'), QLatin1Char(' '));
    term.replace(QLatin1Char('-'), QLatin1Char(' '));
    term.remove(QRegularExpression(QStringLiteral("\\s*\\d+$")));
    term = term.simplified();
    // Identifiers commonly carry a suffix the catalog listing drops:
    // "connectormod" is published as "Connector". Removing it is the difference
    // between no results at all and the right mod ranked first.
    static const QStringList redundantSuffixes{
        QStringLiteral("mod"), QStringLiteral("forge"), QStringLiteral("fabric")
    };
    for (const QString &suffix : redundantSuffixes) {
        if (term.size() > suffix.size() + 2 && term.endsWith(suffix)
            && !term.endsWith(QLatin1Char(' ') + suffix)) {
            term.chop(suffix.size());
            term = term.simplified();
            break;
        }
    }
    return term.isEmpty() ? dependencyId : term;
}
}  // namespace

QStringList ServerListPage::missingServerDependencies(ServerInstance *server) const
{
    if (!server) {
        return {};
    }
    const QString root = server->serverDirectory();
    if (!QFileInfo(QDir(root).filePath(
                       QStringLiteral("jlauncher_derived_server.txt"))).isFile()) {
        return {};
    }
    const ServerDependencyCheckResult check =
        ServerModpackInstaller::checkServerDependencies(
            root, server->loaderType(), server->version(), server->loaderVersion());
    if (check.state == ServerDependencyCheckState::DefiniteFailure
        || check.state == ServerDependencyCheckState::Unsafe) {
        return check.missingDependencyIds;
    }
    return {};
}

void ServerListPage::offerDependencyRepair(const QStringList &missingIds,
                                           const QString &introduction)
{
    if (missingIds.isEmpty()) {
        return;
    }
    QMessageBox prompt(this);
    prompt.setWindowTitle(tr("Missing Server Mods"));
    prompt.setIcon(QMessageBox::Warning);
    prompt.setText(introduction);
    prompt.setInformativeText(
        tr("Missing mod IDs: %1\n\n"
           "Choose Find Missing Mods to search the same trusted catalogs. "
           "J Launcher installs the file you pick, together with anything it "
           "requires, into this server's mods folder.")
            .arg(missingIds.join(QStringLiteral(", "))));
    auto *findButton = prompt.addButton(tr("Find Missing Mods"),
                                        QMessageBox::AcceptRole);
    prompt.addButton(QMessageBox::Close);
    prompt.exec();
    if (prompt.clickedButton() == findButton) {
        browseServerContent(dependencySearchTerm(missingIds.first()));
    }
}

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
            browseServerContent(dependencySearchTerm(result.missingDependencyIds.first()));
        }
    } else {
        QMessageBox::information(this, tr("Modpack Ready"), details);
    }
}

void ServerListPage::onBrowseMods()
{
    browseServerContent();
}

void ServerListPage::browseServerContent(const QString &initialSearch)
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) {
        return;
    }

    const QString loader = server->loaderType().toLower();
    const ServerContentType contentType = server->contentType();
    const bool pluginServer = contentType == ServerContentType::Plugin;
    const QMap<QString, QString> loaderComponents = {
        { QStringLiteral("fabric"), QStringLiteral("net.fabricmc.fabric-loader") },
        { QStringLiteral("forge"), QStringLiteral("net.minecraftforge") },
        { QStringLiteral("neoforge"), QStringLiteral("net.neoforged") },
        { QStringLiteral("quilt"), QStringLiteral("org.quiltmc.quilt-loader") },
    };
    const QString loaderComponent = loaderComponents.value(loader);
    if (contentType == ServerContentType::None
        || (!pluginServer && loaderComponent.isEmpty())) {
        QMessageBox::information(this, tr("Download Mods"),
                                 tr("The app downloader requires a Fabric, Forge, NeoForge, Quilt, Paper, or Purpur server."));
        return;
    }
    const ModPlatform::ResourceType resourceType =
        pluginServer ? ModPlatform::ResourceType::Plugin : ModPlatform::ResourceType::Mod;
    QStringList loaderNames;
    if (loader == "paper") {
        loaderNames = { QStringLiteral("paper"), QStringLiteral("spigot"), QStringLiteral("bukkit") };
    } else if (loader == "purpur") {
        loaderNames = { QStringLiteral("purpur"), QStringLiteral("paper"),
                        QStringLiteral("spigot"), QStringLiteral("bukkit") };
    }

    QTemporaryDir compatibilityRoot(QDir::tempPath() + "/jlauncher-server-content-download-XXXXXX");
    if (!compatibilityRoot.isValid()) {
        QMessageBox::warning(this, tr("Download Mods"),
                             tr("Could not prepare the compatibility data for the mod downloader."));
        return;
    }

    auto compatibilitySettings =
        std::make_unique<INISettingsObject>(QDir(compatibilityRoot.path()).filePath("instance.cfg"));
    MinecraftInstance compatibilityInstance(APPLICATION->settings(), std::move(compatibilitySettings),
                                             compatibilityRoot.path());
    compatibilityInstance.setName(server->name());
    PackProfile *profile = compatibilityInstance.getPackProfile();
    profile->buildingFromScratch();
    profile->setComponentVersion(QStringLiteral("net.minecraft"), server->version());
    if (!pluginServer) {
        profile->setComponentVersion(loaderComponent, QStringLiteral("server"));
    }
    // This profile exists only to provide compatibility filters to the instance
    // downloader, so it is not resolved through the normal component metadata
    // task. Populate the cached versions that downloader filters read directly.
    profile->getComponent(QStringLiteral("net.minecraft"))->m_cachedVersion = server->version();
    if (!pluginServer) {
        profile->getComponent(loaderComponent)->m_cachedVersion = QStringLiteral("server");
    }

    const QString contentDirectory = server->contentDirectory();
    ModFolderModel serverContent(QDir(contentDirectory), &compatibilityInstance, true, true);
    QEventLoop initialScan;
    connect(&serverContent, &ModFolderModel::updateFinished, &initialScan, &QEventLoop::quit);
    if (serverContent.update()) {
        initialScan.exec();
    }

    ResourceDownload::ModDownloadDialog dialog(this, &serverContent, &compatibilityInstance,
                                               false, resourceType, loaderNames);
    if (!initialSearch.trimmed().isEmpty() && dialog.selectedPage()) {
        dialog.selectedPage()->setSearchTerm(initialSearch.trimmed());
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const auto selectedDownloads = dialog.getTasks();
    if (selectedDownloads.isEmpty()) {
        return;
    }

    auto downloads = std::make_unique<ConcurrentTask>(
        pluginServer ? tr("Download Server Plugins") : tr("Download Server Mods"),
        APPLICATION->settings()->get("NumberOfConcurrentDownloads").toInt());
    QStringList downloadedNames;
    const QString serverId = server->id();
    for (const auto &download : selectedDownloads) {
        const QString filename = download->getFilename();
        const QString provider = download->getProvider() == ModPlatform::ResourceProvider::MODRINTH
            ? QStringLiteral("modrinth")
            : QStringLiteral("curseforge");
        const QString source = QString("%1:%2:%3")
            .arg(provider, download->getPack()->addonId.toString(),
                 download->getVersion().fileId.toString());
        connect(download.get(), &Task::succeeded, this, [serverId, filename, source]() {
            QSettings().setValue(QString("ServerContentSources/%1/%2").arg(serverId, filename), source);
        });
        downloadedNames.append(filename);
        downloads->addTask(download);
    }

    ProgressDialog progress(this);
    progress.setWindowTitle(pluginServer ? tr("Downloading Server Plugins") : tr("Downloading Server Mods"));
    progress.setSkipButton(true, tr("Abort"));
    progress.execWithTask(downloads.get());

    if (downloads->getState() == Task::State::Failed) {
        QMessageBox::critical(this, pluginServer ? tr("Download Plugins") : tr("Download Mods"),
                              tr("One or more files could not be downloaded:\n%1").arg(
                                  Privacy::sanitizeText(downloads->failReason())));
    } else if (downloads->getState() == Task::State::AbortedByUser) {
        QMessageBox::information(this, pluginServer ? tr("Download Plugins") : tr("Download Mods"),
                                 tr("Download stopped by user."));
    } else if (downloads->wasSuccessful()) {
        const QStringList warnings = downloads->warnings();
        if (!warnings.isEmpty()) {
            QMessageBox::warning(this, tr("Download Warnings"), warnings.join('\n'));
        }
        appendConsoleOutput(pluginServer
                                ? tr("[INFO] Downloaded server plugins: %1").arg(downloadedNames.join(", "))
                                : tr("[INFO] Downloaded server mods: %1").arg(downloadedNames.join(", ")));
    }
    // A cancelled or partially failed batch can still contain successfully
    // installed files, so always bring the server views back in sync.
    refreshInstalledContent();
    refreshOverview();

    // Installed mods can require further mods of their own, so recheck here
    // rather than letting the next start attempt be the one that reports it.
    const QStringList stillMissing = missingServerDependencies(server.get());
    if (!stillMissing.isEmpty()) {
        offerDependencyRepair(
            stillMissing,
            tr("Required mods are still missing after this installation."));
    }
}

void ServerListPage::onAddLocalContent()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->contentType() == ServerContentType::None) {
        return;
    }
    const bool plugins = server->contentType() == ServerContentType::Plugin;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, plugins ? tr("Add Local Plugins") : tr("Add Local Mods"),
        QString(), tr("Java Archives (*.jar)"));
    if (paths.isEmpty()) {
        return;
    }
    QString error;
    if (!server->addContentFiles(paths, &error)) {
        QMessageBox::warning(
            this, plugins ? tr("Could Not Add Plugins") : tr("Could Not Add Mods"), error);
        return;
    }
    refreshInstalledContent();
    refreshOverview();
}

void ServerListPage::onToggleInstalledContent()
{
    auto *item = ui->installedContentTree->currentItem();
    if (!item) return;
    const QString source = item->data(0, Qt::UserRole).toString();
    if (source.isEmpty()) return;

    const bool disabled = source.endsWith(".disabled", Qt::CaseInsensitive);
    const QString destination = disabled ? source.left(source.size() - QString(".disabled").size()) : source + ".disabled";
    if (QFile::exists(destination)) {
        QMessageBox::warning(this, tr("Could Not Change Content"), tr("A file with the target name already exists."));
        return;
    }
    const auto server = m_serverManager
        ? m_serverManager->getServer(m_selectedServerId) : nullptr;
    QString cacheError;
    if (server && !server->invalidateContentCaches(&cacheError)) {
        QMessageBox::warning(this, tr("Could Not Change Content"), cacheError);
        return;
    }
    if (!QFile::rename(source, destination)) {
        QMessageBox::warning(this, tr("Could Not Change Content"), tr("The selected file could not be renamed."));
        return;
    }
    refreshInstalledContent();
    refreshOverview();
}

void ServerListPage::onRemoveInstalledContent()
{
    auto *item = ui->installedContentTree->currentItem();
    if (!item) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty()) return;
    if (QMessageBox::question(this, tr("Remove Installed Content"),
                              tr("Remove %1 from this server?").arg(QFileInfo(path).fileName()),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    const auto server = m_serverManager
        ? m_serverManager->getServer(m_selectedServerId) : nullptr;
    QString cacheError;
    if (server && !server->invalidateContentCaches(&cacheError)) {
        QMessageBox::warning(this, tr("Could Not Remove Content"), cacheError);
        return;
    }
    if (!QFile::remove(path)) {
        QMessageBox::warning(this, tr("Could Not Remove Content"), tr("The selected file could not be removed."));
        return;
    }
    refreshInstalledContent();
    refreshOverview();
}

void ServerListPage::onOpenContentFolder()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;
    const QString directory = server->contentDirectory();
    if (!directory.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    }
}

void ServerListPage::onCreateBackup()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;

    const QString requestedName = ui->backupNameInput->text().trimmed();
    if (requestedName.isEmpty()) {
        QMessageBox::information(this, tr("Backup Name Required"), tr("Enter a name for this backup before creating it."));
        return;
    }

    QString error;
    ServerBackupInfo backup;
    if (!m_serverManager->createServerBackup(
            m_selectedServerId, requestedName, &backup, &error)) {
        QMessageBox::warning(this, tr("Backup Failed"), error);
        return;
    }
    ui->backupNameInput->clear();
    refreshServerBackups();
    refreshOverview();
    QMessageBox::information(this, tr("Backup Created"), tr("Created backup: %1").arg(backup.name));
}

void ServerListPage::onRestoreBackup()
{
    auto *item = ui->backupsTree->currentItem();
    if (!item || !m_serverManager || m_selectedServerId.isEmpty()) return;
    const QString backupPath = item->data(0, Qt::UserRole).toString();
    if (backupPath.isEmpty()) return;

    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) return;
    if (QMessageBox::question(this, tr("Restore Backup"),
                              tr("This replaces the server files, worlds, player data, and configuration with the selected backup. Continue?"),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    QString error;
    QString safetyBackup;
    if (!m_serverManager->restoreServerBackup(m_selectedServerId, backupPath, &error, &safetyBackup)) {
        QMessageBox::warning(this, tr("Restore Failed"), error);
        return;
    }
    refreshInstalledContent();
    refreshServerFiles();
    refreshOverview();
    QMessageBox::information(
        this, tr("Backup Restored"),
        tr("The selected backup has been restored.\nA safety backup of the previous state was saved as %1.")
            .arg(safetyBackup));
}

void ServerListPage::onOpenBackupsFolder()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;
    const QString backupRoot = QDir(server->serverDirectory()).filePath("backups");
    QDir().mkpath(backupRoot);
    QDesktopServices::openUrl(QUrl::fromLocalFile(backupRoot));
}

void ServerListPage::onRemoveBackup()
{
    auto *item = ui->backupsTree->currentItem();
    if (!item) return;
    const QString backupPath = item->data(0, Qt::UserRole).toString();
    if (backupPath.isEmpty()) return;
    if (QMessageBox::question(this, tr("Remove Backup"),
                              tr("Permanently remove backup '%1'?").arg(item->text(0)),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!m_serverManager || !m_serverManager->deleteServerBackup(
            m_selectedServerId, backupPath, &error)) {
        QMessageBox::warning(this, tr("Could Not Remove Backup"), error);
        return;
    }
    refreshServerBackups();
    refreshOverview();
}

void ServerListPage::onExportProfile()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Server Profile"),
        server->name() + ".jserver.json", tr("J Launcher Server Profile (*.jserver.json)"));
    if (path.isEmpty()) return;
    QJsonObject profile = server->toJson();
    profile.remove("id");
    profile.remove("serverDirectory");
    profile["profileFormat"] = "JLauncherServerProfile";
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(profile).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        QMessageBox::warning(this, tr("Export Profile"), tr("Could not save the server profile."));
        return;
    }
    QMessageBox::information(this, tr("Profile Exported"), tr("The server profile was exported without worlds, mods, plugins, or player data."));
}

void ServerListPage::onImportProfile()
{
    if (!m_serverManager) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Server Profile"), QString(),
        tr("J Launcher Server Profile (*.jserver.json);;JSON files (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import Profile"), tr("Could not read the selected profile."));
        return;
    }
    const QJsonObject profile = QJsonDocument::fromJson(file.readAll()).object();
    if (profile.value("profileFormat").toString() != "JLauncherServerProfile") {
        QMessageBox::warning(this, tr("Import Profile"), tr("This is not a J Launcher server profile."));
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Import Server Profile"), tr("Server name:"),
        QLineEdit::Normal, profile.value("name").toString(), &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    const auto server = m_serverManager->createServer(name, profile.value("version").toString(), profile.value("loaderType").toString("vanilla"), profile.value("loaderVersion").toString());
    if (!server) {
        QMessageBox::warning(this, tr("Import Profile"), tr("Could not create a server from this profile."));
        return;
    }
    server->setPort(profile.value("port").toInt(25565));
    server->setMinMemory(profile.value("minMemory").toInt(1024));
    server->setMaxMemory(profile.value("maxMemory").toInt(2048));
    server->setJavaPath(profile.value("javaPath").toString());
    server->setExtraJvmArguments(profile.value("extraJvmArguments").toString());
    server->setAutoRestartOnCrash(profile.value("autoRestartOnCrash").toBool(false));
    m_serverManager->save();
    setSelectedServerId(server->id());
    updateServerList();
    QMessageBox::information(this, tr("Profile Imported"), tr("Created %1. Start it to download its server software.").arg(name));
}

void ServerListPage::onFindConsole()
{
    const QString term = m_consoleSearchInput->text().trimmed();
    if (term.isEmpty()) return;
    if (!ui->consoleOutput->find(term)) {
        ui->consoleOutput->moveCursor(QTextCursor::Start);
        ui->consoleOutput->find(term);
    }
}

void ServerListPage::onCopyConsoleErrors()
{
    QStringList errors;
    for (const QString &line : ui->consoleOutput->toPlainText().split('\n')) {
        if (line.contains("[ERROR]", Qt::CaseInsensitive) || line.contains("exception", Qt::CaseInsensitive) || line.contains("[CRASH]", Qt::CaseInsensitive)) errors.append(line);
    }
    QApplication::clipboard()->setText(errors.isEmpty() ? tr("No errors found in this console log.") : errors.join('\n'));
}

void ServerListPage::restoreBackupAt(const QString &backupPath)
{
    for (int index = 0; index < ui->backupsTree->topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = ui->backupsTree->topLevelItem(index);
        if (QDir::cleanPath(item->data(0, Qt::UserRole).toString()) == QDir::cleanPath(backupPath)) {
            ui->backupsTree->setCurrentItem(item);
            onRestoreBackup();
            return;
        }
    }
    QMessageBox::warning(this, tr("Update Backup Missing"), tr("The latest rollback backup is not available in the backup list."));
}

void ServerListPage::onImportServerPack()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server || server->isRunning()) {
        return;
    }

    const QString archive = QFileDialog::getOpenFileName(
        this, tr("Import Server Pack"), QString(), tr("ZIP archives (*.zip)"));
    if (archive.isEmpty()) {
        return;
    }

    QString error;
    if (!server->beginServerPackImport(archive, &error)) {
        QMessageBox::warning(this, tr("Could Not Import Server Pack"), error);
        return;
    }

    // Large packs take a while to unpack and copy; do it off the UI thread so the window
    // keeps painting. The server refuses to start until finishServerPackImport.
    QProgressDialog importProgress(tr("Importing server pack..."), QString(), 0, 0, this);
    importProgress.setWindowTitle(tr("Import Server Pack"));
    importProgress.setCancelButton(nullptr);
    importProgress.setWindowModality(Qt::ApplicationModal);
    importProgress.setMinimumDuration(0);
    importProgress.show();

    using ImportResult = QPair<bool, QString>;
    QFutureWatcher<ImportResult> watcher;
    QEventLoop waitLoop;
    connect(&watcher, &QFutureWatcher<ImportResult>::finished, &waitLoop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([serverDirectory = server->serverDirectory(), archive]() {
        QString importError;
        const bool imported =
            ServerInstance::importServerPackFiles(serverDirectory, archive, &importError);
        return ImportResult(imported, importError);
    }));
    if (!watcher.isFinished()) {
        waitLoop.exec();
    }
    const ImportResult imported = watcher.result();
    server->finishServerPackImport(imported.first);
    importProgress.close();
    if (!imported.first) {
        QMessageBox::warning(this, tr("Could Not Import Server Pack"), imported.second);
        return;
    }

    appendConsoleOutput(tr("[INFO] Imported server pack: %1").arg(archive));
    const bool recordSaved = m_serverManager->save();
    updateServerList();
    updateSelectedServerInfo();
    if (!recordSaved) {
        QMessageBox::warning(
            this, tr("Server Pack Imported With Warning"),
            tr("The server pack files were imported, but the updated server record could not be saved. "
               "The imported files are already present; check that the J Launcher data folder is writable, "
               "then save the server settings again."));
        return;
    }
    QMessageBox::information(this, tr("Server Pack Imported"),
                             tr("The server pack's mods and supported configuration files were imported. "
                                "Start or restart the server to load them."));
}

void ServerListPage::onSendCommand()
{
    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_selectedServerId);
    if (server && server->isRunning()) {
        QString command = ui->commandInput->text().trimmed();
        if (!command.isEmpty()) {
            server->writeStdin(command);
            const QString safeCommand = Privacy::sanitizeCommandForDisplay(command);
            server->appendLog(safeCommand);
            appendConsoleOutput(safeCommand);
            ui->commandInput->clear();
        }
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
                // Clear console and load cached log
                ui->consoleOutput->clear();
                ui->consoleOutput->setPlainText(
                    Privacy::sanitizeText(m_currentConnectedServer->consoleLog(),
                                          100000));
                // Scroll to bottom
                ui->consoleOutput->moveCursor(QTextCursor::End);

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
        ui->consoleOutput->clear();
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
    ui->browseModsButton->setEnabled(hasSelection && canEditFiles && supportsContentBrowser);
    ui->serverTabs->setTabEnabled(ui->serverTabs->indexOf(ui->backupsTab), hasSelection);
    ui->serverTabs->setTabEnabled(ui->serverTabs->indexOf(ui->installedContentTab), supportsContentBrowser);
    if (hasSelection && m_serverManager) {
        const auto server = m_serverManager->getServer(m_selectedServerId);
        const bool pluginServer =
            server && server->contentType() == ServerContentType::Plugin;
        ui->browseModsButton->setText(pluginServer ? tr("Download Plugins") : tr("Download Mods"));
        ui->openContentFolderButton->setText(pluginServer ? tr("Open Plugins Folder") : tr("Open Mods Folder"));
        ui->serverTabs->setTabText(ui->serverTabs->indexOf(ui->installedContentTab),
                                   pluginServer ? tr("Plugins") : tr("Mods"));
    } else {
        ui->browseModsButton->setText(tr("Download Mods"));
        ui->openContentFolderButton->setText(tr("Open Mods Folder"));
        ui->serverTabs->setTabText(ui->serverTabs->indexOf(ui->installedContentTab), tr("Mods"));
    }
    ui->importPackButton->setEnabled(hasSelection && canEditFiles);
    ui->deleteServerButton->setEnabled(hasSelection && canEditFiles);
    ui->settingsScrollArea->setEnabled(hasSelection && canEditFiles);
    ui->openFolderButton->setEnabled(hasSelection);
    ui->refreshFilesButton->setEnabled(hasSelection);
    ui->refreshOverviewButton->setEnabled(hasSelection);
    const bool hasInstalledSelection = ui->installedContentTree->currentItem()
        && !ui->installedContentTree->currentItem()->data(0, Qt::UserRole).toString().isEmpty();
    ui->refreshInstalledButton->setEnabled(hasSelection && supportsContentBrowser);
    ui->addLocalContentButton->setEnabled(hasSelection && canEditFiles && supportsContentBrowser);
    ui->toggleInstalledButton->setEnabled(hasInstalledSelection && canEditFiles && supportsContentBrowser);
    ui->removeInstalledButton->setEnabled(hasInstalledSelection && canEditFiles && supportsContentBrowser);
    ui->openContentFolderButton->setEnabled(hasSelection && supportsContentBrowser);
    const bool hasBackupSelection = ui->backupsTree->currentItem()
        && !ui->backupsTree->currentItem()->data(0, Qt::UserRole).toString().isEmpty();
    const bool hasValidBackupSelection = hasBackupSelection
        && ui->backupsTree->currentItem()->data(0, Qt::UserRole + 1).toBool();
    ui->backupNameInput->setEnabled(hasSelection && canEditFiles);
    ui->createBackupButton->setEnabled(hasSelection && canEditFiles);
    ui->refreshBackupsButton->setEnabled(hasSelection);
    ui->openBackupsFolderButton->setEnabled(hasSelection);
    ui->restoreBackupButton->setEnabled(hasValidBackupSelection && canEditFiles);
    ui->removeBackupButton->setEnabled(hasBackupSelection && canEditFiles);
    ui->sendCommandButton->setEnabled(hasSelection && status == ServerStatus::Running);
    ui->commandInput->setEnabled(hasSelection && status == ServerStatus::Running);
    m_exportProfileButton->setEnabled(hasSelection);
    m_importProfileButton->setEnabled(m_serverManager != nullptr);
    m_consoleSearchInput->setEnabled(hasSelection);
    m_findConsoleButton->setEnabled(hasSelection);
    m_copyConsoleErrorsButton->setEnabled(hasSelection);
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
        auto *statusLabel = new QLabel(getStatusString(static_cast<int>(server->status())), card);
        QColor statusColor;
        switch (server->status()) {
            case ServerStatus::Running: statusColor = QColor("#43a047"); break;
            case ServerStatus::Starting:
            case ServerStatus::Stopping:
            case ServerStatus::Downloading: statusColor = QColor("#f9a825"); break;
            case ServerStatus::Error: statusColor = QColor("#e53935"); break;
            default: statusColor = QColor("#607d8b"); break;
        }
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
    if (page == ui->overviewTab) {
        refreshOverview();
        refreshLiveStatistics();
    } else if (page == ui->installedContentTab) {
        refreshInstalledContent();
    } else if (page == m_playersTab) {
        m_playersTab->refresh();
    } else if (page == ui->filesTab) {
        refreshServerFiles();
    } else if (page == ui->backupsTab) {
        refreshServerBackups();
    } else if (page == ui->settingsTab) {
        rebuildSettingsPage();
    } else if (page == m_maintenanceTab && m_maintenanceTab->currentWidget() == m_automationTab) {
        m_automationTab->refresh();
    }
}

void ServerListPage::refreshOverview()
{
    const quint64 generation = ++m_overviewRefreshGeneration;
    const auto setEmpty = [this]() {
        ui->overviewStatusValue->setText("-");
        ui->overviewStatusValue->setStyleSheet({});
        ui->overviewMemoryValue->setText("-");
        ui->overviewContentValue->setText("-");
        if (m_overviewSummaryLabel) {
            m_overviewSummaryLabel->setText(tr("No server data available."));
            m_overviewSummaryLabel->setToolTip({});
        }
    };

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        setEmpty();
        return;
    }

    const auto server = m_serverManager->getServer(m_selectedServerId);
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

    const QString status = getStatusString(static_cast<int>(server->status()));
    QColor statusColor;
    switch (server->status()) {
        case ServerStatus::Running: statusColor = QColor("#43a047"); break;
        case ServerStatus::Starting:
        case ServerStatus::Stopping:
        case ServerStatus::Downloading: statusColor = QColor("#f9a825"); break;
        case ServerStatus::Error: statusColor = QColor("#e53935"); break;
        default: statusColor = QColor("#607d8b"); break;
    }

    ui->overviewStatusValue->setText(status);
    ui->overviewStatusValue->setStyleSheet(QString("color: %1;").arg(statusColor.name()));
    ui->overviewMemoryValue->setText(server->status() == ServerStatus::Running ? tr("Starting...") : tr("Not running"));
    ui->overviewContentValue->setText(tr("Calculating..."));

    const QString serverType = server->loaderType().isEmpty() ? tr("Vanilla") : server->loaderType();
    const QString contentSummary = supportsContent
        ? tr("%1 %2").arg(contentCount).arg(pluginServer ? tr("plugins") : tr("mods"))
        : tr("Not applicable");
    if (m_overviewSummaryLabel) {
        m_overviewSummaryLabel->setText(
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
        m_overviewSummaryLabel->setToolTip(
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
        if (generation != m_overviewRefreshGeneration || selectedServerId != m_selectedServerId) return;
        if (!m_serverManager) return;
        const auto currentServer = m_serverManager->getServer(selectedServerId);
        if (!currentServer) return;
        ui->overviewContentValue->setText(ServerFiles::formatByteSize(sizes.first));
        if (m_overviewSummaryLabel) {
            m_overviewSummaryLabel->setText(
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

void ServerListPage::refreshLiveStatistics()
{
    const auto setInactive = [this](const QString &text) {
        if (!m_overviewCpuBar || !m_overviewRamBar || !m_overviewUpdatedLabel) return;
        m_overviewCpuBar->setValue(0);
        m_overviewCpuBar->setFormat(text);
        m_overviewRamBar->setValue(0);
        m_overviewRamBar->setFormat(text);
        m_overviewUpdatedLabel->setText(text);
    };

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        ui->overviewMemoryValue->setText("-");
        setInactive(tr("No server selected"));
        return;
    }
    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        ui->overviewMemoryValue->setText("-");
        setInactive(tr("Server unavailable"));
        return;
    }

    const QString status = getStatusString(static_cast<int>(server->status()));
    QColor statusColor;
    switch (server->status()) {
        case ServerStatus::Running: statusColor = QColor("#43a047"); break;
        case ServerStatus::Starting:
        case ServerStatus::Stopping:
        case ServerStatus::Downloading: statusColor = QColor("#f9a825"); break;
        case ServerStatus::Error: statusColor = QColor("#e53935"); break;
        default: statusColor = QColor("#607d8b"); break;
    }
    ui->overviewStatusValue->setText(status);
    ui->overviewStatusValue->setStyleSheet(QString("color: %1;").arg(statusColor.name()));

    if (server->status() != ServerStatus::Running || server->processId() <= 0) {
        ui->overviewMemoryValue->setText(tr("Not running"));
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
        ui->overviewMemoryValue->setText(hours > 0 ? tr("%1h %2m").arg(hours).arg(minutes) : tr("%1m").arg(minutes));
    } else {
        ui->overviewMemoryValue->setText(tr("This session"));
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
    if (m_overviewCpuBar && m_overviewRamBar && m_overviewUpdatedLabel) {
        QSettings monitoringSettings;
        const QString monitoringPrefix = QString("ServerMonitoring/%1/").arg(server->id());
        const int cpuWarning = monitoringSettings.value(monitoringPrefix + "cpuWarning", 85).toInt();
        const int ramWarning = monitoringSettings.value(monitoringPrefix + "ramWarning", 90).toInt();
        const int diskWarningGb = monitoringSettings.value(monitoringPrefix + "diskWarningGb", 2).toInt();
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
        m_overviewCpuBar->setRange(0, 100);
        m_overviewCpuBar->setValue(qBound(0, qRound(visibleCpu), 100));
        m_overviewCpuBar->setFormat(cpu < 0.0 ? tr("Measuring...") : QString::number(visibleCpu, 'f', 1) + "%");
        setBarColor(m_overviewCpuBar, metricColor(health.cpu));

        const int ramMegabytes = qMax(0, qRound(snapshot.workingSetBytes / (1024.0 * 1024.0)));
        const int configuredRam = healthInput.configuredRamMiB;
        m_overviewRamBar->setRange(0, qMax(configuredRam, ramMegabytes));
        m_overviewRamBar->setValue(ramMegabytes);
        m_overviewRamBar->setFormat(tr("%1 MB / %2 MB configured").arg(ramMegabytes).arg(configuredRam));
        setBarColor(m_overviewRamBar, metricColor(health.ram));
        QStringList warnings;
        if (health.cpu == ServerMetricLevel::Warning)
            warnings << tr("CPU %1%").arg(cpu, 0, 'f', 1);
        if (health.ram == ServerMetricLevel::Warning)
            warnings << tr("RAM %1%").arg(health.ramPercent, 0, 'f', 1);
        if (health.disk == ServerMetricLevel::Warning) {
            warnings << tr("Disk %1 GB free").arg(storage.bytesAvailable() / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
        }
        if (health.state == ServerHealthState::Warning) {
            m_overviewUpdatedLabel->setText(tr("Warning: %1").arg(warnings.join(", ")));
            m_overviewUpdatedLabel->setStyleSheet("color: #e53935; font-weight: 600;");
        } else if (health.state == ServerHealthState::Measuring) {
            m_overviewUpdatedLabel->setText(tr("Measuring live process usage..."));
            m_overviewUpdatedLabel->setStyleSheet("color: palette(mid);");
        } else {
            m_overviewUpdatedLabel->setText(tr("Updated %1").arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
            m_overviewUpdatedLabel->setStyleSheet("color: palette(mid);");
        }
    }
    m_liveStatisticsPid = processId;
    m_previousProcessCpuMs = snapshot.cpuMilliseconds;
    m_previousSampleTimeMs = now;
#else
    setInactive(tr("Live process metrics are available on Windows."));
#endif
}

void ServerListPage::refreshInstalledContent()
{
    ui->installedContentTree->clear();

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        ui->installedContentInfoLabel->setText(tr("Select a modded or plugin server to view installed content."));
        ui->serverTabs->setTabText(ui->serverTabs->indexOf(ui->installedContentTab), tr("Mods"));
        return;
    }

    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        return;
    }

    const QString loader = server->loaderType().toLower();
    const bool pluginServer = loader == "paper" || loader == "purpur";
    const bool supportsContent = pluginServer || loader == "fabric" || loader == "forge" || loader == "neoforge";
    const QString contentLabel = pluginServer ? tr("Plugins") : tr("Mods");
    ui->serverTabs->setTabText(ui->serverTabs->indexOf(ui->installedContentTab),
                               supportsContent ? contentLabel : tr("Mods"));

    if (!supportsContent) {
        ui->installedContentInfoLabel->setText(tr("This server type does not use launcher-managed mods or plugins."));
        return;
    }

    const QString directoryPath = server->contentDirectory();
    const QDir directory(directoryPath);
    const QFileInfoList files = directory.entryInfoList(QStringList() << "*.jar" << "*.jar.disabled",
                                                         QDir::Files, QDir::Name | QDir::IgnoreCase);
    ui->installedContentInfoLabel->setText(tr("Installed %1 for %2 (%3).")
        .arg(contentLabel.toLower(), server->name().toHtmlEscaped(), directoryPath.toHtmlEscaped()));

    if (files.isEmpty()) {
        auto *emptyItem = new QTreeWidgetItem({tr("No %1 installed yet.").arg(contentLabel.toLower()), QString(), QString(), QString()});
        emptyItem->setFlags(emptyItem->flags() & ~Qt::ItemIsSelectable);
        emptyItem->setForeground(0, palette().brush(QPalette::Mid));
        ui->installedContentTree->addTopLevelItem(emptyItem);
        return;
    }

    for (const QFileInfo &file : files) {
        const ServerContentFileDetails details = ServerFiles::describeContentFile(file.fileName());
        auto *item = new QTreeWidgetItem({details.name,
                                          details.version.isEmpty() ? tr("—") : details.version,
                                          details.enabled ? tr("Enabled") : tr("Disabled"),
                                          QLocale().formattedDataSize(file.size())});
        item->setData(0, Qt::UserRole, file.absoluteFilePath());
        item->setIcon(0, launcherIcon("loadermods", QStyle::SP_FileIcon));
        item->setToolTip(0, file.fileName());
        if (!details.enabled) {
            for (int column = 0; column < item->columnCount(); ++column) {
                item->setForeground(column, palette().brush(QPalette::Mid));
            }
        }
        ui->installedContentTree->addTopLevelItem(item);
    }
}

void ServerListPage::refreshServerBackups()
{
    ui->backupsTree->clear();

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        ui->backupsInfoLabel->setText(tr("Select a server to view its backups."));
        return;
    }

    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) return;

    int count = 0;
    for (const ServerBackupInfo &backup : m_serverManager->listServerBackups(m_selectedServerId)) {
        auto *item = new QTreeWidgetItem(ui->backupsTree);
        item->setText(0, backup.name);
        item->setText(1, backup.createdAt.isValid()
                             ? backup.createdAt.toLocalTime().toString("yyyy-MM-dd HH:mm")
                             : tr("Invalid"));
        item->setText(2, ServerFiles::formatByteSize(backup.size));
        item->setData(0, Qt::UserRole, backup.path);
        item->setData(0, Qt::UserRole + 1, backup.valid);
        item->setToolTip(
            0, backup.valid
                   ? tr("Includes: %1").arg(backup.includedCategories.join(", "))
                   : backup.validationError);
        if (!backup.valid) {
            item->setForeground(0, palette().brush(QPalette::Mid));
        }
        ++count;
    }
    ui->backupsTree->resizeColumnToContents(1);
    ui->backupsTree->resizeColumnToContents(2);
    ui->backupsInfoLabel->setText(tr("%1 complete server backup(s) saved for %2. Enter a name to create a new backup.")
        .arg(count).arg(server->name()));
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

void ServerListPage::refreshServerFiles()
{
    ui->serverFilesTree->clear();

    if (!m_serverManager || m_selectedServerId.isEmpty()) {
        ui->filesInfoLabel->setText(tr("Select a server to browse its files."));
        return;
    }

    const auto server = m_serverManager->getServer(m_selectedServerId);
    if (!server) {
        return;
    }

    const QString directoryPath = server->serverDirectory();
    const QFileInfo rootInfo(directoryPath);
    ui->filesInfoLabel->setText(tr("Server files: %1").arg(directoryPath));
    if (!rootInfo.isDir()) {
        ui->filesInfoLabel->setText(tr("The server folder does not exist yet: %1").arg(directoryPath));
        return;
    }

    auto *rootItem = new QTreeWidgetItem(ui->serverFilesTree);
    rootItem->setText(0, server->name());
    rootItem->setText(1, tr("Server folder"));
    rootItem->setData(0, Qt::UserRole, directoryPath);
    rootItem->setIcon(0, serverCardIcon());
    rootItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    populateServerFileItem(rootItem);
    rootItem->setExpanded(true);
    ui->serverFilesTree->resizeColumnToContents(1);
    ui->serverFilesTree->resizeColumnToContents(2);
}

void ServerListPage::populateServerFileItem(QTreeWidgetItem *item)
{
    if (!item || item->data(0, Qt::UserRole + 1).toBool()) {
        return;
    }

    const QFileInfo parentInfo(item->data(0, Qt::UserRole).toString());
    if (!parentInfo.isDir()) {
        return;
    }

    const QDir directory(parentInfo.absoluteFilePath());
    const QFileInfoList entries = directory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                                                          QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &entry : entries) {
        auto *child = new QTreeWidgetItem(item);
        child->setText(0, entry.fileName());
        child->setText(1, entry.isDir() ? tr("Folder") : tr("File"));
        child->setText(2, entry.isDir() ? QString() : ServerFiles::formatByteSize(entry.size()));
        child->setData(0, Qt::UserRole, entry.absoluteFilePath());
        child->setIcon(0, serverFileIcon(entry));
        if (entry.isDir()) {
            child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        }
    }
    item->setData(0, Qt::UserRole + 1, true);
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
        .arg(getStatusString(static_cast<int>(server->status()))));
}

void ServerListPage::setSelectedServerId(const QString &serverId)
{
    m_selectedServerId = serverId;
    m_automationTab->setServerId(serverId);
    m_playersTab->setServerId(serverId);
    m_updatesTab->setServerId(serverId);
}

void ServerListPage::appendConsoleOutput(const QString &text)
{
    ui->consoleOutput->appendPlainText(Privacy::sanitizeText(text, 8192));
    if (!m_pauseConsoleScrollCheck || !m_pauseConsoleScrollCheck->isChecked()) {
        QScrollBar *scrollBar = ui->consoleOutput->verticalScrollBar();
        scrollBar->setValue(scrollBar->maximum());
    }
}

QString ServerListPage::getStatusString(int status) const
{
    switch (static_cast<ServerStatus>(status)) {
        case ServerStatus::Stopped: return tr("Stopped");
        case ServerStatus::Starting: return tr("Starting");
        case ServerStatus::Running: return tr("Running");
        case ServerStatus::Stopping: return tr("Stopping");
        case ServerStatus::Error: return tr("Error");
        case ServerStatus::Downloading: return tr("Downloading");
        default: return tr("Unknown");
    }
}
