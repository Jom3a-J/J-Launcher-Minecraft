// SPDX-License-Identifier: GPL-3.0-only

#include "ServerContentTab.h"

#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "Application.h"
#include "logs/Privacy.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/ModFolderModel.h"
#include "server/ServerFiles.h"
#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "server/ServerModpackInstaller.h"
#include "settings/INISettingsObject.h"
#include "tasks/ConcurrentTask.h"
#include "ui/dialogs/ProgressDialog.h"
#include "ui/dialogs/ResourceDownloadDialog.h"
#include "ui/pages/modplatform/ResourcePage.h"
#include "ServerPageStyle.h"

using ServerPageStyle::launcherIcon;

namespace {
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

ServerContentTab::ServerContentTab(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("installedContentTab"));
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    layout->setContentsMargins(18, 16, 18, 18);
    m_infoLabel = new QLabel(tr("Manage installed mods or plugins for this server."), this);
    m_infoLabel->setObjectName(QStringLiteral("installedContentInfoLabel"));
    m_infoLabel->setWordWrap(true);
    layout->addWidget(m_infoLabel);

    m_contentTree = new QTreeWidget(this);
    m_contentTree->setObjectName(QStringLiteral("installedContentTree"));
    m_contentTree->setColumnCount(4);
    m_contentTree->setAlternatingRowColors(true);
    m_contentTree->setRootIsDecorated(false);
    m_contentTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_contentTree->setHeaderLabels({ tr("Name"), tr("Version"), tr("Status"), tr("Size") });
    m_contentTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_contentTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_contentTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_contentTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(m_contentTree);

    auto *actions = new QGridLayout();
    actions->setHorizontalSpacing(8);
    actions->setVerticalSpacing(8);
    m_browseButton = new QPushButton(tr("Download Mods"), this);
    m_browseButton->setObjectName(QStringLiteral("browseModsButton"));
    m_addLocalButton = new QPushButton(tr("Add Local JAR"), this);
    m_addLocalButton->setObjectName(QStringLiteral("addLocalContentButton"));
    m_openFolderButton = new QPushButton(tr("Open Mods Folder"), this);
    m_openFolderButton->setObjectName(QStringLiteral("openContentFolderButton"));
    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_refreshButton->setObjectName(QStringLiteral("refreshInstalledButton"));
    m_toggleButton = new QPushButton(tr("Enable / Disable"), this);
    m_toggleButton->setObjectName(QStringLiteral("toggleInstalledButton"));
    m_removeButton = new QPushButton(tr("Remove"), this);
    m_removeButton->setObjectName(QStringLiteral("removeInstalledButton"));
    for (QPushButton *button : { m_browseButton, m_addLocalButton, m_openFolderButton,
                                 m_toggleButton, m_removeButton }) {
        button->setEnabled(false);
    }
    actions->addWidget(m_browseButton, 0, 0);
    actions->addWidget(m_addLocalButton, 0, 1);
    actions->addWidget(m_openFolderButton, 0, 2);
    actions->addWidget(m_refreshButton, 1, 0);
    actions->addWidget(m_toggleButton, 1, 1);
    actions->addWidget(m_removeButton, 1, 2);
    layout->addLayout(actions);

    connect(m_browseButton, &QPushButton::clicked, this, [this]() { browseContent(); });
    connect(m_addLocalButton, &QPushButton::clicked, this, &ServerContentTab::addLocalContent);
    connect(m_toggleButton, &QPushButton::clicked, this, &ServerContentTab::toggleSelectedContent);
    connect(m_removeButton, &QPushButton::clicked, this, &ServerContentTab::removeSelectedContent);
    connect(m_openFolderButton, &QPushButton::clicked, this, &ServerContentTab::openContentFolder);
    connect(m_refreshButton, &QPushButton::clicked, this, &ServerContentTab::refresh);
    connect(m_contentTree, &QTreeWidget::currentItemChanged, this, &ServerContentTab::updateActions);
}

void ServerContentTab::setServerManager(ServerManager *manager)
{
    m_serverManager = manager;
}

void ServerContentTab::setServerId(const QString &serverId)
{
    m_serverId = serverId;
}

void ServerContentTab::updateActions()
{
    const auto server = m_serverManager && !m_serverId.isEmpty()
        ? m_serverManager->getServer(m_serverId) : nullptr;
    const bool hasSelection = !m_serverId.isEmpty();
    const ServerStatus status = server ? server->status() : ServerStatus::Stopped;
    const bool canEditFiles = status == ServerStatus::Stopped || status == ServerStatus::Error;
    const bool supportsContentBrowser = server && server->contentType() != ServerContentType::None;
    const bool pluginServer = server && server->contentType() == ServerContentType::Plugin;
    m_browseButton->setText(pluginServer ? tr("Download Plugins") : tr("Download Mods"));
    m_openFolderButton->setText(pluginServer ? tr("Open Plugins Folder") : tr("Open Mods Folder"));
    m_browseButton->setEnabled(hasSelection && canEditFiles && supportsContentBrowser);
    const bool hasInstalledSelection = m_contentTree->currentItem()
        && !m_contentTree->currentItem()->data(0, Qt::UserRole).toString().isEmpty();
    m_refreshButton->setEnabled(hasSelection && supportsContentBrowser);
    m_addLocalButton->setEnabled(hasSelection && canEditFiles && supportsContentBrowser);
    m_toggleButton->setEnabled(hasInstalledSelection && canEditFiles && supportsContentBrowser);
    m_removeButton->setEnabled(hasInstalledSelection && canEditFiles && supportsContentBrowser);
    m_openFolderButton->setEnabled(hasSelection && supportsContentBrowser);
}

void ServerContentTab::findMissingMods(const QStringList &missingIds)
{
    if (!missingIds.isEmpty()) {
        browseContent(dependencySearchTerm(missingIds.first()));
    }
}

void ServerContentTab::refresh()
{
    m_contentTree->clear();

    if (!m_serverManager || m_serverId.isEmpty()) {
        m_infoLabel->setText(tr("Select a modded or plugin server to view installed content."));
        emit contentLabelChanged(tr("Mods"));
        return;
    }

    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) {
        return;
    }

    const QString loader = server->loaderType().toLower();
    const bool pluginServer = loader == "paper" || loader == "purpur";
    const bool supportsContent = pluginServer || loader == "fabric" || loader == "forge" || loader == "neoforge";
    const QString contentLabel = pluginServer ? tr("Plugins") : tr("Mods");
    emit contentLabelChanged(supportsContent ? contentLabel : tr("Mods"));

    if (!supportsContent) {
        m_infoLabel->setText(tr("This server type does not use launcher-managed mods or plugins."));
        return;
    }

    const QString directoryPath = server->contentDirectory();
    const QDir directory(directoryPath);
    const QFileInfoList files = directory.entryInfoList(QStringList() << "*.jar" << "*.jar.disabled",
                                                         QDir::Files, QDir::Name | QDir::IgnoreCase);
    m_infoLabel->setText(tr("Installed %1 for %2 (%3).")
        .arg(contentLabel.toLower(), server->name().toHtmlEscaped(), directoryPath.toHtmlEscaped()));

    if (files.isEmpty()) {
        auto *emptyItem = new QTreeWidgetItem({tr("No %1 installed yet.").arg(contentLabel.toLower()), QString(), QString(), QString()});
        emptyItem->setFlags(emptyItem->flags() & ~Qt::ItemIsSelectable);
        emptyItem->setForeground(0, palette().brush(QPalette::Mid));
        m_contentTree->addTopLevelItem(emptyItem);
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
        m_contentTree->addTopLevelItem(item);
    }
}

QStringList ServerContentTab::missingDependencies(ServerInstance *server) const
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

void ServerContentTab::offerDependencyRepair(const QStringList &missingIds,
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
        browseContent(dependencySearchTerm(missingIds.first()));
    }
}

void ServerContentTab::browseContent(const QString &initialSearch)
{
    if (!m_serverManager || m_serverId.isEmpty()) {
        return;
    }

    auto server = m_serverManager->getServer(m_serverId);
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
        connect(download.get(), &Task::succeeded, this, [this, serverId, filename, source]() {
            if (m_serverManager) {
                m_serverManager->dataStore().setValue(serverId, ServerDataGroup::ContentSources,
                                                      filename, source);
            }
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
        emit consoleMessage(pluginServer
                                ? tr("[INFO] Downloaded server plugins: %1").arg(downloadedNames.join(", "))
                                : tr("[INFO] Downloaded server mods: %1").arg(downloadedNames.join(", ")));
    }
    // A cancelled or partially failed batch can still contain successfully
    // installed files, so always bring the server views back in sync.
    refresh();
    emit contentChanged();

    // Installed mods can require further mods of their own, so recheck here
    // rather than letting the next start attempt be the one that reports it.
    const QStringList stillMissing = missingDependencies(server.get());
    if (!stillMissing.isEmpty()) {
        offerDependencyRepair(
            stillMissing,
            tr("Required mods are still missing after this installation."));
    }
}

void ServerContentTab::addLocalContent()
{
    if (!m_serverManager || m_serverId.isEmpty()) {
        return;
    }
    const auto server = m_serverManager->getServer(m_serverId);
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
    refresh();
    emit contentChanged();
}

void ServerContentTab::toggleSelectedContent()
{
    auto *item = m_contentTree->currentItem();
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
        ? m_serverManager->getServer(m_serverId) : nullptr;
    QString cacheError;
    if (server && !server->invalidateContentCaches(&cacheError)) {
        QMessageBox::warning(this, tr("Could Not Change Content"), cacheError);
        return;
    }
    if (!QFile::rename(source, destination)) {
        QMessageBox::warning(this, tr("Could Not Change Content"), tr("The selected file could not be renamed."));
        return;
    }
    refresh();
    emit contentChanged();
}

void ServerContentTab::removeSelectedContent()
{
    auto *item = m_contentTree->currentItem();
    if (!item) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty()) return;
    if (QMessageBox::question(this, tr("Remove Installed Content"),
                              tr("Remove %1 from this server? It will be moved to the Recycle Bin.")
                                  .arg(QFileInfo(path).fileName()),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    const auto server = m_serverManager
        ? m_serverManager->getServer(m_serverId) : nullptr;
    QString error;
    if (!server || !server->removeContentFile(path, &error)) {
        QMessageBox::warning(this, tr("Could Not Remove Content"),
                             error.isEmpty() ? tr("The selected file could not be removed.") : error);
        return;
    }
    refresh();
    emit contentChanged();
}

void ServerContentTab::openContentFolder()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) return;
    const QString directory = server->contentDirectory();
    if (!directory.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    }
}
