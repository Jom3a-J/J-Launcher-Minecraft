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
#include "ui/pages/server/ServerContentTab.h"
#include "Application.h"
#include "server/ServerManager.h"
#include "server/ServerInstance.h"
#include "server/ServerModpackInstaller.h"
#include "ui/dialogs/NewInstanceDialog.h"
#include "ui/dialogs/ProgressDialog.h"
#include "ui/pages/server/ServerBusyDialog.h"
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
#include <QFileInfo>
#include <QInputDialog>
#include <utility>

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
    ServerBusyDialog preparationProgress(
        this, tr("Creating Server"), tr("Preparing server files from %1...").arg(packName));
    PreparedServerModpack prepared = preparationProgress.run(
        [profile, stagingPath, gameRoot, stagingParent, selectedServerRoot,
         knownClientOnlyHashes]() {
            return ServerModpackInstaller::prepareMatchingServer(
                profile, stagingPath, gameRoot, stagingParent, selectedServerRoot,
                knownClientOnlyHashes);
        });
    // Installing moves the prepared files into place on this thread; keep the window up.
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
