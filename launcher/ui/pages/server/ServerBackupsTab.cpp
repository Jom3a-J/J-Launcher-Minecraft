// SPDX-License-Identifier: GPL-3.0-only

#include "ServerBackupsTab.h"

#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "server/ServerFiles.h"
#include "server/ServerInstance.h"
#include "server/ServerManager.h"

ServerBackupsTab::ServerBackupsTab(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("backupsTab"));
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    layout->setContentsMargins(18, 16, 18, 18);
    m_infoLabel = new QLabel(tr("Create and restore complete server backups. Stop the server before making changes."), this);
    m_infoLabel->setObjectName(QStringLiteral("backupsInfoLabel"));
    m_infoLabel->setWordWrap(true);
    layout->addWidget(m_infoLabel);

    m_backupsTree = new QTreeWidget(this);
    m_backupsTree->setObjectName(QStringLiteral("backupsTree"));
    m_backupsTree->setColumnCount(3);
    m_backupsTree->setAlternatingRowColors(true);
    m_backupsTree->setHeaderLabels({ tr("Name"), tr("Created"), tr("Size") });
    layout->addWidget(m_backupsTree);

    auto *createActions = new QHBoxLayout();
    createActions->setSpacing(8);
    m_nameInput = new QLineEdit(this);
    m_nameInput->setObjectName(QStringLiteral("backupNameInput"));
    m_nameInput->setMinimumHeight(36);
    m_nameInput->setPlaceholderText(tr("Backup name, e.g. Before update"));
    m_createButton = new QPushButton(tr("Create Backup"), this);
    m_createButton->setObjectName(QStringLiteral("createBackupButton"));
    m_createButton->setMinimumHeight(36);
    m_createButton->setEnabled(false);
    createActions->addWidget(m_nameInput);
    createActions->addWidget(m_createButton);
    layout->addLayout(createActions);

    auto *actions = new QHBoxLayout();
    actions->setSpacing(8);
    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_refreshButton->setObjectName(QStringLiteral("refreshBackupsButton"));
    m_openFolderButton = new QPushButton(tr("Open Folder"), this);
    m_openFolderButton->setObjectName(QStringLiteral("openBackupsFolderButton"));
    m_restoreButton = new QPushButton(tr("Restore Selected"), this);
    m_restoreButton->setObjectName(QStringLiteral("restoreBackupButton"));
    m_removeButton = new QPushButton(tr("Remove Selected"), this);
    m_removeButton->setObjectName(QStringLiteral("removeBackupButton"));
    for (QPushButton *button : { m_refreshButton, m_openFolderButton, m_restoreButton, m_removeButton }) {
        button->setEnabled(false);
        actions->addWidget(button);
    }
    actions->addStretch();
    layout->addLayout(actions);

    connect(m_createButton, &QPushButton::clicked, this, &ServerBackupsTab::createBackup);
    connect(m_restoreButton, &QPushButton::clicked, this, &ServerBackupsTab::restoreSelectedBackup);
    connect(m_openFolderButton, &QPushButton::clicked, this, &ServerBackupsTab::openBackupsFolder);
    connect(m_removeButton, &QPushButton::clicked, this, &ServerBackupsTab::removeSelectedBackup);
    connect(m_refreshButton, &QPushButton::clicked, this, &ServerBackupsTab::refresh);
    connect(m_backupsTree, &QTreeWidget::currentItemChanged, this, &ServerBackupsTab::updateActions);
}

void ServerBackupsTab::setServerManager(ServerManager *manager)
{
    m_serverManager = manager;
}

void ServerBackupsTab::setServerId(const QString &serverId)
{
    m_serverId = serverId;
}

void ServerBackupsTab::updateActions()
{
    const auto server = m_serverManager && !m_serverId.isEmpty()
        ? m_serverManager->getServer(m_serverId) : nullptr;
    const bool hasSelection = !m_serverId.isEmpty();
    const ServerStatus status = server ? server->status() : ServerStatus::Stopped;
    const bool canEditFiles = status == ServerStatus::Stopped || status == ServerStatus::Error;
    const bool hasBackupSelection = m_backupsTree->currentItem()
        && !m_backupsTree->currentItem()->data(0, Qt::UserRole).toString().isEmpty();
    const bool hasValidBackupSelection = hasBackupSelection
        && m_backupsTree->currentItem()->data(0, Qt::UserRole + 1).toBool();
    m_nameInput->setEnabled(hasSelection && canEditFiles);
    m_createButton->setEnabled(hasSelection && canEditFiles);
    m_refreshButton->setEnabled(hasSelection);
    m_openFolderButton->setEnabled(hasSelection);
    m_restoreButton->setEnabled(hasValidBackupSelection && canEditFiles);
    m_removeButton->setEnabled(hasBackupSelection && canEditFiles);
}

bool ServerBackupsTab::restoreBackupAt(const QString &backupPath)
{
    for (int index = 0; index < m_backupsTree->topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = m_backupsTree->topLevelItem(index);
        if (QDir::cleanPath(item->data(0, Qt::UserRole).toString()) == QDir::cleanPath(backupPath)) {
            m_backupsTree->setCurrentItem(item);
            restoreSelectedBackup();
            return true;
        }
    }
    return false;
}

void ServerBackupsTab::createBackup()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) return;

    const QString requestedName = m_nameInput->text().trimmed();
    if (requestedName.isEmpty()) {
        QMessageBox::information(this, tr("Backup Name Required"), tr("Enter a name for this backup before creating it."));
        return;
    }

    QString error;
    ServerBackupInfo backup;
    if (!m_serverManager->createServerBackup(
            m_serverId, requestedName, &backup, &error)) {
        QMessageBox::warning(this, tr("Backup Failed"), error);
        return;
    }
    m_nameInput->clear();
    refresh();
    emit backupsChanged();
    QMessageBox::information(this, tr("Backup Created"), tr("Created backup: %1").arg(backup.name));
}

void ServerBackupsTab::restoreSelectedBackup()
{
    auto *item = m_backupsTree->currentItem();
    if (!item || !m_serverManager || m_serverId.isEmpty()) return;
    const QString backupPath = item->data(0, Qt::UserRole).toString();
    if (backupPath.isEmpty()) return;

    const auto server = m_serverManager->getServer(m_serverId);
    if (!server || server->isRunning()) return;
    if (QMessageBox::question(this, tr("Restore Backup"),
                              tr("This replaces the server files, worlds, player data, and configuration with the selected backup. Continue?"),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    QString error;
    QString safetyBackup;
    if (!m_serverManager->restoreServerBackup(m_serverId, backupPath, &error, &safetyBackup)) {
        QMessageBox::warning(this, tr("Restore Failed"), error);
        return;
    }
    emit serverFilesRestored();
    QMessageBox::information(
        this, tr("Backup Restored"),
        tr("The selected backup has been restored.\nA safety backup of the previous state was saved as %1.")
            .arg(safetyBackup));
}

void ServerBackupsTab::openBackupsFolder()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) return;
    const QString backupRoot = QDir(server->serverDirectory()).filePath("backups");
    QDir().mkpath(backupRoot);
    QDesktopServices::openUrl(QUrl::fromLocalFile(backupRoot));
}

void ServerBackupsTab::removeSelectedBackup()
{
    auto *item = m_backupsTree->currentItem();
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
            m_serverId, backupPath, &error)) {
        QMessageBox::warning(this, tr("Could Not Remove Backup"), error);
        return;
    }
    refresh();
    emit backupsChanged();
}

void ServerBackupsTab::refresh()
{
    m_backupsTree->clear();

    if (!m_serverManager || m_serverId.isEmpty()) {
        m_infoLabel->setText(tr("Select a server to view its backups."));
        return;
    }

    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) return;

    int count = 0;
    for (const ServerBackupInfo &backup : m_serverManager->listServerBackups(m_serverId)) {
        auto *item = new QTreeWidgetItem(m_backupsTree);
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
    m_backupsTree->resizeColumnToContents(1);
    m_backupsTree->resizeColumnToContents(2);
    m_infoLabel->setText(tr("%1 complete server backup(s) saved for %2. Enter a name to create a new backup.")
        .arg(count).arg(server->name()));
}
