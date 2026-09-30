// SPDX-License-Identifier: GPL-3.0-only

#include "ServerUpdatesTab.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QUrlQuery>
#include <QVBoxLayout>

#include "Application.h"
#include "BuildConfig.h"
#include "InstanceList.h"
#include "minecraft/MinecraftInstance.h"
#include "modplatform/flame/FlameAPI.h"
#include "server/ServerContentUpdater.h"
#include "server/ServerDownloader.h"
#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "server/ServerModpackInstaller.h"
#include "ServerPageStyle.h"

using ServerPageStyle::applyMutedLabelPalette;

namespace {
/// Whether CurseForge requests can be made. False without the launcher's Application object,
/// which is how the page runs in tests.
bool curseForgeAvailable()
{
    Application *application = APPLICATION_DYN;
    return application && (application->capabilities() & Application::SupportsFlame);
}

QStringList modrinthLoadersForServer(const QString &loaderType)
{
    const QString loader = loaderType.trimmed().toLower();
    if (loader == QStringLiteral("paper"))
        return { QStringLiteral("paper"), QStringLiteral("spigot"), QStringLiteral("bukkit") };
    if (loader == QStringLiteral("purpur"))
        return { QStringLiteral("purpur"), QStringLiteral("paper"),
                 QStringLiteral("spigot"), QStringLiteral("bukkit") };
    return { loader };
}

QNetworkRequest updateRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "JLauncher/1.0");
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

QNetworkRequest curseForgeUpdateRequest(const QUrl &url)
{
    QNetworkRequest request = updateRequest(url);
    request.setRawHeader("x-api-key", APPLICATION->getFlameAPIKey().toUtf8());
    return request;
}

void showContentUpdate(QTreeWidgetItem *item,
                       const ServerContentUpdateCandidate &update)
{
    if (!item) return;
    if (!update.available && !update.upToDate) {
        item->setText(2, QObject::tr("No compatible update"));
    } else if (update.upToDate) {
        item->setText(2, QObject::tr("Up to date"));
    } else {
        item->setText(2, QObject::tr("Update available: %1").arg(update.versionNumber));
        item->setData(0, Qt::UserRole, update.url.toString());
        item->setData(0, Qt::UserRole + 3, update.fileName);
        item->setData(0, Qt::UserRole + 4, static_cast<int>(update.hashAlgorithm));
        item->setData(0, Qt::UserRole + 5, update.expectedHash);
        item->setData(0, Qt::UserRole + 6, update.versionId);
    }
}
}  // namespace

ServerUpdatesTab::ServerUpdatesTab(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("updatesTab"));
    m_network = new QNetworkAccessManager(this);
    auto *layout = new QVBoxLayout(this);
    m_infoLabel = new QLabel(tr("Keep the server stopped while applying updates."), this);
    m_infoLabel->setObjectName(QStringLiteral("updatesInfoLabel"));
    m_infoLabel->setWordWrap(true);
    layout->addWidget(m_infoLabel);

    auto *softwareGroup = new QGroupBox(tr("Server Software"), this);
    auto *softwareLayout = new QGridLayout(softwareGroup);
    auto *softwareDescription = new QLabel(
        tr("Update the current Minecraft version's server build, or explicitly change the Minecraft version."),
        softwareGroup);
    softwareDescription->setWordWrap(true);
    softwareLayout->addWidget(softwareDescription, 0, 0, 1, 3);
    m_updateServerSoftwareButton = new QPushButton(tr("Update Current Build"), softwareGroup);
    m_changeMinecraftVersionButton = new QPushButton(tr("Change Minecraft Version"), softwareGroup);
    m_restoreLatestUpdateBackupButton = new QPushButton(tr("Restore Latest Update Backup"), softwareGroup);
    m_updateServerSoftwareButton->setObjectName(QStringLiteral("updateServerSoftwareButton"));
    m_changeMinecraftVersionButton->setObjectName(QStringLiteral("changeMinecraftVersionButton"));
    m_restoreLatestUpdateBackupButton->setObjectName(
        QStringLiteral("restoreLatestUpdateBackupButton"));
    m_restoreLatestUpdateBackupButton->setToolTip(tr("Restore the rollback backup made before the most recent server software update."));
    softwareLayout->addWidget(m_updateServerSoftwareButton, 1, 0);
    softwareLayout->addWidget(m_changeMinecraftVersionButton, 1, 1);
    softwareLayout->addWidget(m_restoreLatestUpdateBackupButton, 1, 2);
    layout->addWidget(softwareGroup);

    auto *contentGroup = new QGroupBox(tr("Mod & Plugin Updates"), this);
    auto *contentLayout = new QVBoxLayout(contentGroup);
    m_contentUpdatesTree = new QTreeWidget(contentGroup);
    m_contentUpdatesTree->setObjectName(QStringLiteral("contentUpdatesTree"));
    m_contentUpdatesTree->setColumnCount(3);
    m_contentUpdatesTree->setHeaderLabels({tr("Installed file"), tr("Source"), tr("Update status")});
    m_contentUpdatesTree->setAlternatingRowColors(true);
    contentLayout->addWidget(m_contentUpdatesTree);
    auto *contentActions = new QHBoxLayout();
    m_checkContentUpdatesButton = new QPushButton(tr("Check for Updates"), contentGroup);
    m_setupCurseForgeButton = new QPushButton(tr("Set Up CurseForge"), contentGroup);
    m_installContentUpdateButton = new QPushButton(tr("Install Selected Update"), contentGroup);
    m_checkContentUpdatesButton->setObjectName(QStringLiteral("checkContentUpdatesButton"));
    m_setupCurseForgeButton->setObjectName(QStringLiteral("setupCurseForgeButton"));
    m_installContentUpdateButton->setObjectName(QStringLiteral("installContentUpdateButton"));
    contentActions->addWidget(m_checkContentUpdatesButton);
    contentActions->addWidget(m_setupCurseForgeButton);
    contentActions->addWidget(m_installContentUpdateButton);
    contentActions->addStretch();
    contentLayout->addLayout(contentActions);
    layout->addWidget(contentGroup, 1);

    connect(m_updateServerSoftwareButton, &QPushButton::clicked, this, &ServerUpdatesTab::updateServerSoftware);
    connect(m_changeMinecraftVersionButton, &QPushButton::clicked, this, &ServerUpdatesTab::changeMinecraftVersion);
    connect(m_restoreLatestUpdateBackupButton, &QPushButton::clicked, this, &ServerUpdatesTab::restoreLatestUpdateBackup);
    connect(m_checkContentUpdatesButton, &QPushButton::clicked, this, &ServerUpdatesTab::checkContentUpdates);
    connect(m_setupCurseForgeButton, &QPushButton::clicked, this, &ServerUpdatesTab::setUpCurseForge);
    connect(m_installContentUpdateButton, &QPushButton::clicked, this, &ServerUpdatesTab::installContentUpdate);
    connect(m_contentUpdatesTree, &QTreeWidget::currentItemChanged, this, &ServerUpdatesTab::updateActions);
}

void ServerUpdatesTab::setServerManager(ServerManager *manager)
{
    m_serverManager = manager;
}

void ServerUpdatesTab::setServerId(const QString &serverId)
{
    m_serverId = serverId;
}

void ServerUpdatesTab::clearUpdateList()
{
    m_contentUpdatesTree->clear();
    m_infoLabel->setText(m_serverId.isEmpty()
        ? tr("Select a server to check for updates.")
        : tr("Check tracked Modrinth and CurseForge content for compatible updates. New modpack servers import mod tracking automatically."));
}

QString ServerUpdatesTab::latestRollbackBackupPath(const ServerInstance &server) const
{
    const ServerDataStore &records = m_serverManager->dataStore();
    QString backupPath =
        records.value(server.id(), ServerDataGroup::Updates, "latestRollbackBackupPath").toString();
    if (backupPath.isEmpty()) {
        const QString backupName =
            records.value(server.id(), ServerDataGroup::Updates, "latestRollbackBackup").toString();
        if (!backupName.isEmpty()) {
            backupPath = QDir(QDir(server.serverDirectory()).filePath("backups"))
                             .filePath(backupName);
        }
    }
    return backupPath;
}

void ServerUpdatesTab::updateActions()
{
    const auto server = m_serverManager && !m_serverId.isEmpty()
        ? m_serverManager->getServer(m_serverId) : nullptr;
    const bool hasSelection = server != nullptr;
    const ServerStatus status = server ? server->status() : ServerStatus::Stopped;
    const bool canEditFiles = status == ServerStatus::Stopped || status == ServerStatus::Error;
    const bool supportsContentBrowser = server && server->contentType() != ServerContentType::None;
    m_updateServerSoftwareButton->setEnabled(hasSelection && canEditFiles);
    m_changeMinecraftVersionButton->setEnabled(hasSelection && canEditFiles);
    const QString backupPath = server ? latestRollbackBackupPath(*server) : QString();
    const bool hasRollbackBackup = !backupPath.isEmpty() && QFileInfo(backupPath).isDir();
    m_restoreLatestUpdateBackupButton->setEnabled(hasRollbackBackup && canEditFiles);
    m_checkContentUpdatesButton->setEnabled(hasSelection && canEditFiles && supportsContentBrowser);
    m_setupCurseForgeButton->setVisible(!curseForgeAvailable());
    m_setupCurseForgeButton->setEnabled(hasSelection && canEditFiles && supportsContentBrowser);
    const bool hasContentUpdate = m_contentUpdatesTree->currentItem()
        && !m_contentUpdatesTree->currentItem()->data(0, Qt::UserRole).toString().isEmpty();
    if (m_activeContentUpdater) {
        m_installContentUpdateButton->setText(tr("Cancel Update"));
        m_installContentUpdateButton->setEnabled(true);
    } else {
        m_installContentUpdateButton->setText(tr("Install Selected Update"));
        m_installContentUpdateButton->setEnabled(
            hasContentUpdate && canEditFiles && supportsContentBrowser);
    }
}

void ServerUpdatesTab::showSoftwareDownloadResult(const std::shared_ptr<ServerInstance> &server,
                                                  const QString &targetVersion, bool success,
                                                  bool cancelled, const QString &errorMessage)
{
    if (success) {
        m_infoLabel->setText(server->loaderVersion().isEmpty()
            ? tr("Server software for Minecraft %1 was installed successfully. Restart the server to verify the world and content.")
                  .arg(targetVersion)
            : tr("Server software build %1 for Minecraft %2 was installed successfully. Restart the server to verify the world and content.")
                  .arg(server->loaderVersion(), targetVersion));
    } else if (cancelled) {
        m_infoLabel->setText(
            tr("Server software download was cancelled. The previous server file and version were kept."));
    } else {
        m_infoLabel->setText(
            tr("Server software update failed. The previous server file and version were kept: %1")
                .arg(errorMessage));
    }
}

void ServerUpdatesTab::setUpCurseForge()
{
    APPLICATION->ShowGlobalSettings(this, QStringLiteral("apis"));
    updateActions();
    if (curseForgeAvailable()) {
        m_infoLabel->setText(
            tr("CurseForge is enabled. Check for updates again to include CurseForge mods."));
    }
}

void ServerUpdatesTab::updateServerSoftware()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server || server->isRunning()) return;

    if (server->loaderType().compare(QStringLiteral("vanilla"), Qt::CaseInsensitive) == 0) {
        QMessageBox::information(
            this, tr("Vanilla Server Builds"),
            tr("Mojang publishes one official server JAR for each Minecraft version, so Vanilla has no separate build numbers. "
               "J Launcher can download a fresh verified copy of the official JAR."));
        startServerSoftwareUpdate(server, server->version(), false);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Update Current Build"));
    dialog.setMinimumWidth(520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(
        tr("Minecraft %1 · %2\nCurrent build: %3")
            .arg(server->version(), server->loaderType(),
                 server->loaderVersion().isEmpty() ? tr("Not recorded") : server->loaderVersion()),
        &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *form = new QFormLayout();
    auto *build = new QComboBox(&dialog);
    build->setObjectName(QStringLiteral("updateServerBuildCombo"));
    form->addRow(tr("Provider build:"), build);
    layout->addLayout(form);
    auto *status = new QLabel(
        tr("Loading builds published by %1 for Minecraft %2...")
            .arg(server->loaderType(), server->version()), &dialog);
    status->setWordWrap(true);
    applyMutedLabelPalette(status);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Install Build"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);

    ServerDownloader downloader(&dialog);
    connect(&downloader, &ServerDownloader::buildsReady, &dialog,
            [&](const QStringList &builds) {
        build->clear();
        build->addItems(builds);
        const bool different = !build->currentText().isEmpty()
            && build->currentText() != server->loaderVersion();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(different);
        status->setText(different
            ? tr("%1 builds available. Newest provider build is selected.").arg(builds.size())
            : tr("%1 builds available. Select a build different from the installed build.").arg(builds.size()));
    });
    connect(&downloader, &ServerDownloader::buildsFailed, &dialog,
            [&](const QString &error) {
        status->setText(tr("Could not load builds: %1").arg(error));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    });
    connect(build, &QComboBox::currentTextChanged, &dialog, [&](const QString &selected) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            !selected.isEmpty() && selected != server->loaderVersion());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    downloader.fetchAvailableBuilds(server->version(), server->loaderType());

    if (dialog.exec() != QDialog::Accepted) return;
    const QString targetBuild = build->currentText().trimmed();
    if (targetBuild.isEmpty() || targetBuild == server->loaderVersion()) return;
    startServerSoftwareUpdate(server, server->version(), false, targetBuild);
}

void ServerUpdatesTab::changeMinecraftVersion()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server || server->isRunning()) return;

    const QString loader = server->loaderType().trimmed().toLower();
    if (loader == QStringLiteral("fabric") || loader == QStringLiteral("forge")
        || loader == QStringLiteral("neoforge")) {
        QMessageBox::information(
            this, tr("Modded Version Change Blocked"),
            tr("J Launcher will not automatically move a modded server to another Minecraft version. "
               "The loader and every server mod must be compatible with the target version.\n\n"
               "Create a new compatible modpack server, verify it, and then migrate the world. "
               "Updating the current build remains available."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Change Minecraft Version"));
    dialog.setMinimumWidth(520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *warning = new QLabel(
        tr("Current version: %1. Changing Minecraft versions can make worlds or plugins incompatible. "
           "A rollback backup will be created before downloading.").arg(server->version()),
        &dialog);
    warning->setWordWrap(true);
    layout->addWidget(warning);

    auto *form = new QFormLayout();
    auto *channel = new QComboBox(&dialog);
    channel->setObjectName(QStringLiteral("upgradeVersionChannelCombo"));
    channel->addItem(tr("Releases"), static_cast<int>(ServerDownloader::VersionChannel::Release));
    channel->addItem(tr("Snapshots"), static_cast<int>(ServerDownloader::VersionChannel::Snapshot));
    channel->addItem(tr("Betas"), static_cast<int>(ServerDownloader::VersionChannel::Beta));
    channel->addItem(tr("All versions"), -1);
    auto *version = new QComboBox(&dialog);
    version->setObjectName(QStringLiteral("upgradeMinecraftVersionCombo"));
    form->addRow(tr("Version channel:"), channel);
    form->addRow(tr("Target version:"), version);
    layout->addLayout(form);

    auto *status = new QLabel(tr("Loading versions published by %1...").arg(server->loaderType()), &dialog);
    status->setWordWrap(true);
    applyMutedLabelPalette(status);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Change Version"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);

    QStringList availableVersions;
    const auto applyFilter = [&]() {
        const int selectedChannel = channel->currentData().toInt();
        const QString previous = version->currentText();
        QStringList filtered;
        for (const QString &candidate : std::as_const(availableVersions)) {
            if (selectedChannel < 0
                || static_cast<int>(ServerDownloader::versionChannel(candidate)) == selectedChannel) {
                filtered.append(candidate);
            }
        }
        const QSignalBlocker blocker(version);
        version->clear();
        version->addItems(filtered);
        if (filtered.contains(previous)) version->setCurrentText(previous);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            !version->currentText().isEmpty() && version->currentText() != server->version());
        status->setText(tr("%1 versions shown. Select a version different from %2.")
                            .arg(filtered.size()).arg(server->version()));
    };

    ServerDownloader downloader(&dialog);
    connect(&downloader, &ServerDownloader::versionsReady, &dialog,
            [&](const QStringList &versions) {
        availableVersions = versions;
        applyFilter();
    });
    connect(&downloader, &ServerDownloader::versionsFailed, &dialog,
            [&](const QString &error) {
        status->setText(tr("Could not load versions: %1").arg(error));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    });
    connect(channel, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [&](int) { applyFilter(); });
    connect(version, &QComboBox::currentTextChanged, &dialog, [&](const QString &selected) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            !selected.isEmpty() && selected != server->version());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    downloader.fetchAvailableVersions(server->loaderType());

    if (dialog.exec() != QDialog::Accepted) return;
    const QString targetVersion = version->currentText().trimmed();
    if (targetVersion.isEmpty() || targetVersion == server->version()) return;
    startServerSoftwareUpdate(server, targetVersion, true);
}

bool ServerUpdatesTab::startServerSoftwareUpdate(
    const std::shared_ptr<ServerInstance> &server, const QString &targetVersion,
    bool changeVersion, const QString &targetBuild)
{
    if (!server || server->isRunning() || targetVersion.trimmed().isEmpty()) return false;
    const QString prompt = changeVersion
        ? tr("Change this %1 server from Minecraft %2 to %3?\n\n"
             "A complete rollback backup will be created first. Verify the world and all plugins before deleting that backup.")
              .arg(server->loaderType(), server->version(), targetVersion)
        : (targetBuild.isEmpty()
            ? tr("Download a fresh %1 server for Minecraft %2?\n\n"
                 "A complete rollback backup will be created first.")
                  .arg(server->loaderType(), server->version())
            : tr("Install %1 build %2 for Minecraft %3?\n\n"
                 "A complete rollback backup will be created first.")
                  .arg(server->loaderType(), targetBuild, server->version()));
    if (QMessageBox::question(this,
                              changeVersion ? tr("Change Minecraft Version")
                                            : tr("Update Current Build"),
                              prompt, QMessageBox::Yes | QMessageBox::No)
        != QMessageBox::Yes) {
        return false;
    }

    QString backupError;
    ServerBackupInfo rollbackBackup;
    if (!m_serverManager->createServerBackup(
            server->id(), tr("Before update"), &rollbackBackup, &backupError)) {
        QMessageBox::warning(this, tr("Update Cancelled"), tr("Could not create the rollback backup:\n%1").arg(backupError));
        return false;
    }
    const QString backupFolderName = QFileInfo(rollbackBackup.path).fileName();
    ServerDataStore &records = m_serverManager->dataStore();
    const QString updateSource = changeVersion ? server->version() : server->loaderVersion();
    const QString updateTarget = targetBuild.isEmpty() ? targetVersion : targetBuild;
    records.addToList(server->id(), ServerDataGroup::Updates, "history",
                      QString("%1 — %2 %3 -> %4; rollback backup %5")
                          .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm"),
                               changeVersion ? tr("version change") : tr("build update"),
                               updateSource, updateTarget, backupFolderName),
                      30, ServerDataStore::Order::NewestLast);
    records.setValue(server->id(), ServerDataGroup::Updates, "latestRollbackBackup", backupFolderName);
    records.setValue(server->id(), ServerDataGroup::Updates, "latestRollbackBackupPath", rollbackBackup.path);
    const bool started = changeVersion
        ? server->downloadServerJarForVersion(targetVersion, server->javaPath(), false)
        : (targetBuild.isEmpty()
            ? server->downloadServerJar(server->javaPath(), false)
            : server->downloadServerBuild(targetBuild, server->javaPath(), false));
    if (started) {
        m_infoLabel->setText(
            targetBuild.isEmpty()
                ? tr("Created rollback backup %1. Downloading server software for Minecraft %2; follow progress in Console.")
                      .arg(rollbackBackup.name, targetVersion)
                : tr("Created rollback backup %1. Downloading %2 build %3; follow progress in Console.")
                      .arg(rollbackBackup.name, server->loaderType(), targetBuild));
        emit consoleMessage(
            tr("[INFO] Created rollback backup %1, then downloading server software for Minecraft %2 without starting the server.")
                .arg(backupFolderName, targetVersion));
        emit backupsChanged();
        emit actionsChanged();
        emit serverRecordChanged();
        return true;
    }
    m_infoLabel->setText(tr("The server software download could not be started. The rollback backup was kept."));
    return false;
}

void ServerUpdatesTab::restoreLatestUpdateBackup()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server || server->isRunning()) return;
    const QString backupPath = latestRollbackBackupPath(*server);
    if (backupPath.isEmpty()) {
        QMessageBox::information(this, tr("No Update Backup"), tr("No rollback backup has been recorded for this server yet."));
        return;
    }
    if (!QFileInfo(backupPath).isDir()) {
        QMessageBox::warning(this, tr("Update Backup Missing"), tr("The latest recorded rollback backup could not be found."));
        return;
    }
    emit restoreBackupRequested(backupPath);
}

void ServerUpdatesTab::checkContentUpdates()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server || server->isRunning()) return;

    const QString loader = server->loaderType().toLower();
    if (server->contentType() == ServerContentType::None) return;

    m_contentUpdatesTree->clear();
    const QString contentDirectory = server->contentDirectory();
    const QFileInfoList files = QDir(contentDirectory).entryInfoList(QStringList() << "*.jar", QDir::Files);
    ServerDataStore &records = m_serverManager->dataStore();
    int requests = 0;
    int untracked = 0;
    int curseForgeNeedsKey = 0;
    int recoveredTracking = 0;
    // Each instance's metadata is read at most once, and only if an untracked file needs it.
    QHash<QString, ContentMetadataIndex> instanceMetadata;
    for (const QFileInfo &installed : files) {
        QString source = records.value(server->id(), ServerDataGroup::ContentSources, installed.fileName()).toString();
        if (source.isEmpty() && APPLICATION_DYN && APPLICATION->instances()) {
            for (int instanceIndex = 0;
                 instanceIndex < APPLICATION->instances()->count(); ++instanceIndex) {
                MinecraftInstance *instance = APPLICATION->instances()->at(instanceIndex);
                if (!instance) continue;
                const QString gameRoot = instance->gameRoot();
                auto metadata = instanceMetadata.find(gameRoot);
                if (metadata == instanceMetadata.end()) {
                    metadata = instanceMetadata.insert(
                        gameRoot, ServerModpackInstaller::loadContentMetadata(gameRoot));
                }
                source = ServerModpackInstaller::contentTrackingSource(
                    gameRoot, *metadata, installed.absoluteFilePath());
                if (!source.isEmpty()) {
                    records.setValue(server->id(), ServerDataGroup::ContentSources, installed.fileName(), source);
                    ++recoveredTracking;
                    break;
                }
            }
        }
        const QString provider = source.section(':', 0, 0).toLower();
        const QString projectId = source.section(':', 1, 1);
        const QString installedVersionId = source.section(':', 2, 2);
        auto *item = new QTreeWidgetItem(m_contentUpdatesTree);
        item->setText(0, installed.fileName());
        item->setText(1, source.isEmpty() ? tr("Not tracked")
            : provider == QStringLiteral("curseforge") ? tr("CurseForge") : tr("Modrinth"));
        item->setData(0, Qt::UserRole + 1, installed.absoluteFilePath());
        item->setData(0, Qt::UserRole + 2, source);
        if (source.isEmpty() || projectId.isEmpty()) {
            item->setText(2, source.isEmpty()
                ? tr("Downloaded before update tracking")
                : tr("Check from the content browser"));
            ++untracked;
            continue;
        }
        if (provider == QStringLiteral("curseforge") && !curseForgeAvailable()) {
            item->setText(2, tr("CurseForge API key required — use Set Up CurseForge"));
            ++curseForgeNeedsKey;
            continue;
        }
        if (provider != QStringLiteral("modrinth")
            && provider != QStringLiteral("curseforge")) {
            item->setText(2, tr("Unknown update provider"));
            ++untracked;
            continue;
        }

        QUrl url(provider == QStringLiteral("curseforge")
            ? QString(BuildConfig.FLAME_BASE_URL + "/mods/%1/files").arg(projectId)
            : QString("https://api.modrinth.com/v2/project/%1/version").arg(projectId));
        QUrlQuery query;
        if (provider == QStringLiteral("curseforge")) {
            query.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("10000"));
            query.addQueryItem(QStringLiteral("gameVersion"), server->version());
        } else {
            const QJsonArray loaders = QJsonArray::fromStringList(modrinthLoadersForServer(loader));
            query.addQueryItem(QStringLiteral("loaders"), QString::fromUtf8(
                QJsonDocument(loaders).toJson(QJsonDocument::Compact)));
            query.addQueryItem("game_versions", QString("[\"%1\"]").arg(server->version()));
        }
        url.setQuery(query);
        item->setText(2, tr("Checking…"));
        ++requests;
        QNetworkReply *reply = m_network->get(
            provider == QStringLiteral("curseforge")
                ? curseForgeUpdateRequest(url) : updateRequest(url));
        const QString installedName = installed.fileName();
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, source, provider, projectId, installedVersionId,
                 installedName, loader]() {
            QTreeWidgetItem *item = nullptr;
            for (int row = 0; row < m_contentUpdatesTree->topLevelItemCount(); ++row) {
                QTreeWidgetItem *candidate = m_contentUpdatesTree->topLevelItem(row);
                if (candidate->text(0) == installedName && candidate->data(0, Qt::UserRole + 2).toString() == source) {
                    item = candidate;
                    break;
                }
            }
            if (!item) { reply->deleteLater(); return; }
            if (reply->error() != QNetworkReply::NoError) {
                item->setText(2, tr("Could not check — %1").arg(reply->errorString()));
                reply->deleteLater();
                return;
            }
            QString metadataError;
            ServerContentUpdateCandidate update = provider == QStringLiteral("curseforge")
                ? ServerContentUpdater::parseCurseForgeFilesResponse(
                      reply->readAll(), installedName, loader, &metadataError)
                : ServerContentUpdater::parseModrinthVersionResponse(
                      reply->readAll(), installedName, &metadataError);
            if (!installedVersionId.isEmpty() && update.versionId == installedVersionId) {
                update.available = false;
                update.upToDate = true;
            }
            if (!metadataError.isEmpty()) {
                item->setText(2, tr("Could not check — %1").arg(metadataError));
            } else if (provider == QStringLiteral("curseforge") && update.available
                       && update.url.isEmpty()) {
                item->setText(2, tr("Resolving CurseForge download…"));
                const QUrl downloadUrlEndpoint =
                    FlameAPI::fileDownloadUrlEndpoint(projectId, update.providerFileId);
                QNetworkReply *downloadReply = m_network->get(
                    curseForgeUpdateRequest(downloadUrlEndpoint));
                connect(downloadReply, &QNetworkReply::finished, this,
                        [this, downloadReply, source, installedName, update]() mutable {
                    QTreeWidgetItem *currentItem = nullptr;
                    for (int row = 0; row < m_contentUpdatesTree->topLevelItemCount(); ++row) {
                        QTreeWidgetItem *candidate = m_contentUpdatesTree->topLevelItem(row);
                        if (candidate->text(0) == installedName
                            && candidate->data(0, Qt::UserRole + 2).toString() == source) {
                            currentItem = candidate;
                            break;
                        }
                    }
                    if (!currentItem) {
                        downloadReply->deleteLater();
                        return;
                    }
                    if (downloadReply->error() != QNetworkReply::NoError) {
                        currentItem->setText(
                            2, tr("CurseForge download unavailable — %1")
                                   .arg(downloadReply->errorString()));
                    } else {
                        QString urlError;
                        update.url = FlameAPI::loadFileDownloadUrl(
                            downloadReply->readAll(), &urlError);
                        if (update.url.isEmpty()) {
                            currentItem->setText(
                                2, tr("CurseForge download unavailable — %1").arg(urlError));
                        } else {
                            showContentUpdate(currentItem, update);
                        }
                    }
                    downloadReply->deleteLater();
                    updateActions();
                });
            } else {
                showContentUpdate(item, update);
            }
            reply->deleteLater();
            updateActions();
        });
    }
    if (requests) {
        m_infoLabel->setText(
            tr("Checking %1 tracked file(s) from Modrinth and CurseForge. Recovered tracking for %2 old file(s); %3 file(s) still need manual source selection; %4 CurseForge file(s) need an API key.")
                .arg(requests).arg(recoveredTracking).arg(untracked)
                .arg(curseForgeNeedsKey));
    } else if (curseForgeNeedsKey) {
        m_infoLabel->setText(
            tr("CurseForge updates need an API key. Use Set Up CurseForge, save a valid key in Services, then check again."));
    } else {
        m_infoLabel->setText(
            tr("No tracked files were found. Mods imported by new modpack servers and files downloaded from the Mods or Plugins tab are tracked automatically."));
    }
    updateActions();
}

void ServerUpdatesTab::installContentUpdate()
{
    if (m_activeContentUpdater) {
        m_activeContentUpdater->cancel();
        m_installContentUpdateButton->setEnabled(false);
        m_infoLabel->setText(
            tr("Cancelling content update; the installed file will be kept."));
        return;
    }
    auto *item = m_contentUpdatesTree ? m_contentUpdatesTree->currentItem() : nullptr;
    if (!item || !m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server || server->isRunning()) return;
    const QUrl url(item->data(0, Qt::UserRole).toString());
    const QString replacementName = item->data(0, Qt::UserRole + 3).toString();
    const QString oldPath = item->data(0, Qt::UserRole + 1).toString();
    const QString source = item->data(0, Qt::UserRole + 2).toString();
    const auto hashAlgorithm = static_cast<QCryptographicHash::Algorithm>(item->data(0, Qt::UserRole + 4).toInt());
    const QByteArray expectedHash = item->data(0, Qt::UserRole + 5).toByteArray();
    const QString versionId = item->data(0, Qt::UserRole + 6).toString();
    if (!url.isValid() || replacementName.isEmpty() || oldPath.isEmpty() || expectedHash.isEmpty()) return;

    const QString serverId = m_serverId;
    auto *updater = new ServerContentUpdater(this);
    m_activeContentUpdater = updater;
    item->setText(2, tr("Downloading…"));
    m_installContentUpdateButton->setText(tr("Cancel Update"));
    m_installContentUpdateButton->setEnabled(true);
    connect(updater, &ServerContentUpdater::progress, this,
            [this, oldPath](qint64 received, qint64 total) {
        if (total <= 0) return;
        for (int row = 0; row < m_contentUpdatesTree->topLevelItemCount(); ++row) {
            QTreeWidgetItem* candidate = m_contentUpdatesTree->topLevelItem(row);
            if (candidate->data(0, Qt::UserRole + 1).toString() == oldPath) {
                candidate->setText(
                    2, tr("Downloading… %1%").arg(received * 100 / total));
                break;
            }
        }
    });
    connect(updater, &ServerContentUpdater::finished, this,
            [this, updater, hashAlgorithm, expectedHash, versionId, oldPath, source,
             serverId](const ServerContentUpdateResult& result) {
        QTreeWidgetItem *target = nullptr;
        for (int row = 0; row < m_contentUpdatesTree->topLevelItemCount(); ++row) {
            QTreeWidgetItem *candidate = m_contentUpdatesTree->topLevelItem(row);
            if (candidate->data(0, Qt::UserRole + 1).toString() == oldPath) {
                target = candidate;
                break;
            }
        }
        if (result.success) {
            if (const auto updatedServer = m_serverManager
                    ? m_serverManager->getServer(serverId) : nullptr) {
                QString cacheError;
                if (!updatedServer->invalidateContentCaches(&cacheError)) {
                    QMessageBox::warning(this, tr("Content Cache Could Not Be Cleared"),
                                         cacheError);
                }
            }
            if (m_serverManager) {
                ServerDataStore &records = m_serverManager->dataStore();
                const QString updatedSource = QStringLiteral("%1:%2")
                    .arg(source.section(':', 0, 1), versionId);
                const QString updatedName = QFileInfo(result.destinationPath).fileName();
                records.remove(serverId, ServerDataGroup::ContentSources, QFileInfo(oldPath).fileName());
                records.setValue(serverId, ServerDataGroup::ContentSources, updatedName, updatedSource);
                records.setValue(serverId, ServerDataGroup::ContentMetadata, updatedName, QVariantMap{
                    { "versionId", versionId },
                    { "url", result.finalUrl.toString() },
                    { "hashAlgorithm", static_cast<int>(hashAlgorithm) },
                    { "hash", QString::fromLatin1(expectedHash.toHex()) },
                    { "installedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
                });
            }
            if (target) {
                target->setText(0, QFileInfo(result.destinationPath).fileName());
                target->setText(2, tr("Updated — restart server to load it"));
                target->setData(0, Qt::UserRole, QString());
                target->setData(0, Qt::UserRole + 1, result.destinationPath);
            }
            if (m_serverId == serverId) emit installedContentChanged();
        } else {
            if (target) target->setText(2, result.message);
        }
        m_infoLabel->setText(result.message);
        if (m_activeContentUpdater == updater) m_activeContentUpdater = nullptr;
        updater->deleteLater();
        m_installContentUpdateButton->setText(tr("Install Selected Update"));
        updateActions();
    });

    ServerContentUpdateRequest request;
    request.url = url;
    request.installedPath = oldPath;
    request.replacementName = replacementName;
    request.hashAlgorithm = hashAlgorithm;
    request.expectedHash = expectedHash;
    QString error;
    if (!updater->start(server, request, &error)) {
        m_activeContentUpdater = nullptr;
        updater->deleteLater();
        item->setText(2, tr("Update blocked — %1").arg(error));
        m_infoLabel->setText(error);
        m_installContentUpdateButton->setText(tr("Install Selected Update"));
        updateActions();
    }
}
