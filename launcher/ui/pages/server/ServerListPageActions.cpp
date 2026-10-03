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
#include "ui/pages/server/ServerAutomationTab.h"
#include "ui/pages/server/ServerConsoleTab.h"
#include "ui/pages/server/ServerContentTab.h"
#include "ui/pages/server/ServerFilesTab.h"
#include "ui/pages/server/ServerOverviewTab.h"
#include "ui/pages/server/ServerPlayersTab.h"
#include "ui/pages/server/ServerUpdatesTab.h"
#include "Application.h"
#include "server/ServerManager.h"
#include "server/ServerInstance.h"
#include "ui/dialogs/CreateServerDialog.h"
#include "ui/pages/server/ServerBackupsTab.h"
#include "ui/pages/server/ServerBusyDialog.h"
#include "ui/pages/server/ServerSettingsPage.h"
#include "logs/Privacy.h"
#include <QMessageBox>
#include <QAbstractButton>
#include <QDialog>
#include <QDir>
#include <QFileInfo>

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
