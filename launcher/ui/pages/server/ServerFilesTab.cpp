// SPDX-License-Identifier: GPL-3.0-only

#include "ServerFilesTab.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPair>
#include <QPushButton>
#include <QSaveFile>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "server/ServerFiles.h"
#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "ServerBusyDialog.h"
#include "ServerPageStyle.h"

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
}  // namespace

ServerFilesTab::ServerFilesTab(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("filesTab"));
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    layout->setContentsMargins(18, 16, 18, 18);
    m_infoLabel = new QLabel(tr("Browse server files and transfer server profiles."), this);
    m_infoLabel->setObjectName(QStringLiteral("filesInfoLabel"));
    m_infoLabel->setWordWrap(true);
    layout->addWidget(m_infoLabel);

    m_filesTree = new QTreeWidget(this);
    m_filesTree->setObjectName(QStringLiteral("serverFilesTree"));
    m_filesTree->setColumnCount(3);
    m_filesTree->setAlternatingRowColors(true);
    m_filesTree->setHeaderLabels({ tr("Name"), tr("Type"), tr("Size") });
    layout->addWidget(m_filesTree);

    auto *actions = new QHBoxLayout();
    actions->setSpacing(8);
    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_refreshButton->setObjectName(QStringLiteral("refreshFilesButton"));
    m_openFolderButton = new QPushButton(tr("Open Server Folder"), this);
    m_openFolderButton->setObjectName(QStringLiteral("openFolderButton"));
    m_exportProfileButton = new QPushButton(tr("Export Profile"), this);
    m_importProfileButton = new QPushButton(tr("Import Profile"), this);
    m_importPackButton = new QPushButton(tr("Import Server Pack"), this);
    m_importPackButton->setObjectName(QStringLiteral("importPackButton"));
    for (QPushButton *button : { m_refreshButton, m_openFolderButton, m_exportProfileButton,
                                 m_importProfileButton, m_importPackButton }) {
        actions->addWidget(button);
    }
    m_refreshButton->setEnabled(false);
    m_openFolderButton->setEnabled(false);
    m_importPackButton->setEnabled(false);
    actions->addStretch();
    layout->addLayout(actions);

    connect(m_refreshButton, &QPushButton::clicked, this, &ServerFilesTab::refresh);
    connect(m_openFolderButton, &QPushButton::clicked, this, &ServerFilesTab::openServerFolder);
    connect(m_importPackButton, &QPushButton::clicked, this, &ServerFilesTab::importServerPack);
    connect(m_exportProfileButton, &QPushButton::clicked, this, &ServerFilesTab::exportProfile);
    connect(m_importProfileButton, &QPushButton::clicked, this, &ServerFilesTab::importProfile);
    connect(m_filesTree, &QTreeWidget::itemExpanded, this, &ServerFilesTab::populateItem);
    connect(m_filesTree, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem *item, int) {
        if (!item) return;
        const QFileInfo file(item->data(0, Qt::UserRole).toString());
        if (file.isDir()) {
            item->setExpanded(!item->isExpanded());
        } else if (file.exists()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(file.absoluteFilePath()));
        }
    });
}

void ServerFilesTab::setServerManager(ServerManager *manager)
{
    m_serverManager = manager;
}

void ServerFilesTab::setServerId(const QString &serverId)
{
    m_serverId = serverId;
}

void ServerFilesTab::updateActions()
{
    const auto server = m_serverManager && !m_serverId.isEmpty()
        ? m_serverManager->getServer(m_serverId) : nullptr;
    const bool hasSelection = !m_serverId.isEmpty();
    const ServerStatus status = server ? server->status() : ServerStatus::Stopped;
    const bool canEditFiles = status == ServerStatus::Stopped || status == ServerStatus::Error;
    m_importPackButton->setEnabled(hasSelection && canEditFiles);
    m_openFolderButton->setEnabled(hasSelection);
    m_refreshButton->setEnabled(hasSelection);
    m_exportProfileButton->setEnabled(hasSelection);
    m_importProfileButton->setEnabled(m_serverManager != nullptr);
}

void ServerFilesTab::refresh()
{
    m_filesTree->clear();

    if (!m_serverManager || m_serverId.isEmpty()) {
        m_infoLabel->setText(tr("Select a server to browse its files."));
        return;
    }

    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) {
        return;
    }

    const QString directoryPath = server->serverDirectory();
    const QFileInfo rootInfo(directoryPath);
    m_infoLabel->setText(tr("Server files: %1").arg(directoryPath));
    if (!rootInfo.isDir()) {
        m_infoLabel->setText(tr("The server folder does not exist yet: %1").arg(directoryPath));
        return;
    }

    auto *rootItem = new QTreeWidgetItem(m_filesTree);
    rootItem->setText(0, server->name());
    rootItem->setText(1, tr("Server folder"));
    rootItem->setData(0, Qt::UserRole, directoryPath);
    rootItem->setIcon(0, serverCardIcon());
    rootItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    populateItem(rootItem);
    rootItem->setExpanded(true);
    m_filesTree->resizeColumnToContents(1);
    m_filesTree->resizeColumnToContents(2);
}

void ServerFilesTab::populateItem(QTreeWidgetItem *item)
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

void ServerFilesTab::openServerFolder()
{
    if (!m_serverManager || m_serverId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_serverId);
    if (server) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(server->serverDirectory()));
    }
}

void ServerFilesTab::importServerPack()
{
    if (!m_serverManager || m_serverId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_serverId);
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
    ServerBusyDialog importProgress(this, tr("Import Server Pack"), tr("Importing server pack..."));
    using ImportResult = QPair<bool, QString>;
    const ImportResult imported =
        importProgress.run([serverDirectory = server->serverDirectory(), archive]() {
            QString importError;
            const bool imported =
                ServerInstance::importServerPackFiles(serverDirectory, archive, &importError);
            return ImportResult(imported, importError);
        });
    server->finishServerPackImport(imported.first);
    importProgress.close();
    if (!imported.first) {
        QMessageBox::warning(this, tr("Could Not Import Server Pack"), imported.second);
        return;
    }

    emit consoleMessage(tr("[INFO] Imported server pack: %1").arg(archive));
    const bool recordSaved = m_serverManager->save();
    emit serverRecordChanged();
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

void ServerFilesTab::exportProfile()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
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

void ServerFilesTab::importProfile()
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
    emit serverCreated(server->id());
    QMessageBox::information(this, tr("Profile Imported"), tr("Created %1. Start it to download its server software.").arg(name));
}
