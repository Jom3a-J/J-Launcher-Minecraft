// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QWidget>

class ServerManager;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

/// The Server Manager's "Backups" tab: complete server backups to create, restore and remove.
class ServerBackupsTab : public QWidget
{
    Q_OBJECT

public:
    explicit ServerBackupsTab(QWidget *parent = nullptr);

    void setServerManager(ServerManager *manager);
    /// The server the tab shows; empty for none. Call refresh() to load it.
    void setServerId(const QString &serverId);

    /// Reloads the backup list for the current server.
    void refresh();
    /// Enables the actions that apply to the selected backup and the server's state.
    void updateActions();
    /// Selects the listed backup at backupPath and restores it, after asking. False when the
    /// backup is not in the list.
    bool restoreBackupAt(const QString &backupPath);

signals:
    /// A backup was created or removed.
    void backupsChanged();
    /// A backup replaced the server's files.
    void serverFilesRestored();

private:
    void createBackup();
    void restoreSelectedBackup();
    void openBackupsFolder();
    void removeSelectedBackup();

    ServerManager *m_serverManager = nullptr;
    QString m_serverId;
    QLabel *m_infoLabel = nullptr;
    QTreeWidget *m_backupsTree = nullptr;
    QLineEdit *m_nameInput = nullptr;
    QPushButton *m_createButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_openFolderButton = nullptr;
    QPushButton *m_restoreButton = nullptr;
    QPushButton *m_removeButton = nullptr;
};
