// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QWidget>
#include <memory>

class ServerContentUpdater;
class ServerInstance;
class ServerManager;
class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QTreeWidget;

/*! The Server Manager's "Updates" tab: server software builds, Minecraft version changes with
 *  a rollback backup, and update checks for mods and plugins from Modrinth and CurseForge.
 */
class ServerUpdatesTab : public QWidget
{
    Q_OBJECT

public:
    explicit ServerUpdatesTab(QWidget *parent = nullptr);

    void setServerManager(ServerManager *manager);
    /// The server the tab works on; empty for none.
    void setServerId(const QString &serverId);

    /// Empties the update list, which belongs to the previously shown server.
    void clearUpdateList();
    /// Enables the actions that apply to the current server's state.
    void updateActions();
    /// Reports how a server software download ended.
    void showSoftwareDownloadResult(const std::shared_ptr<ServerInstance> &server,
                                    const QString &targetVersion, bool success, bool cancelled,
                                    const QString &errorMessage);

signals:
    /// A line for the page's console view.
    void consoleMessage(const QString &text);
    /// A backup was created; the page's backup list is out of date.
    void backupsChanged();
    /// The server's record (version, build) shown elsewhere on the page may have changed.
    void serverRecordChanged();
    /// A mod or plugin file was replaced.
    void installedContentChanged();
    /// The user asked to restore this backup (a folder that exists).
    void restoreBackupRequested(const QString &backupPath);
    /// The server's state may have changed; the page should update its controls.
    void actionsChanged();

private:
    void updateServerSoftware();
    void changeMinecraftVersion();
    bool startServerSoftwareUpdate(const std::shared_ptr<ServerInstance> &server,
                                   const QString &targetVersion, bool changeVersion,
                                   const QString &targetBuild = QString());
    void restoreLatestUpdateBackup();
    void checkContentUpdates();
    void installContentUpdate();
    void setUpCurseForge();
    /// The rollback backup folder recorded by the latest software update, or empty.
    QString latestRollbackBackupPath(const ServerInstance &server) const;

    ServerManager *m_serverManager = nullptr;
    QString m_serverId;
    QNetworkAccessManager *m_network = nullptr;
    ServerContentUpdater *m_activeContentUpdater = nullptr;
    QLabel *m_infoLabel = nullptr;
    QTreeWidget *m_contentUpdatesTree = nullptr;
    QPushButton *m_updateServerSoftwareButton = nullptr;
    QPushButton *m_changeMinecraftVersionButton = nullptr;
    QPushButton *m_restoreLatestUpdateBackupButton = nullptr;
    QPushButton *m_checkContentUpdatesButton = nullptr;
    QPushButton *m_setupCurseForgeButton = nullptr;
    QPushButton *m_installContentUpdateButton = nullptr;
};
