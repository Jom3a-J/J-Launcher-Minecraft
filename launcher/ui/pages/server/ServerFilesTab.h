// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QWidget>

class ServerManager;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

/*! The Server Manager's "Files" tab: the server folder as a tree, server-pack import, and
 *  server profile export and import.
 */
class ServerFilesTab : public QWidget
{
    Q_OBJECT

public:
    explicit ServerFilesTab(QWidget *parent = nullptr);

    void setServerManager(ServerManager *manager);
    /// The server the tab shows; empty for none. Call refresh() to load it.
    void setServerId(const QString &serverId);

    /// Reloads the top of the file tree; folders load when they are opened.
    void refresh();
    /// Enables the actions that apply to the server's state.
    void updateActions();

signals:
    /// A line for the page's console view.
    void consoleMessage(const QString &text);
    /// The server's record changed (a server pack can change its port).
    void serverRecordChanged();
    /// A server was created from an imported profile.
    void serverCreated(const QString &serverId);

private:
    void populateItem(QTreeWidgetItem *item);
    void openServerFolder();
    void importServerPack();
    void exportProfile();
    void importProfile();

    ServerManager *m_serverManager = nullptr;
    QString m_serverId;
    QLabel *m_infoLabel = nullptr;
    QTreeWidget *m_filesTree = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_openFolderButton = nullptr;
    QPushButton *m_exportProfileButton = nullptr;
    QPushButton *m_importProfileButton = nullptr;
    QPushButton *m_importPackButton = nullptr;
};
