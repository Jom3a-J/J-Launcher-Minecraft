// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QWidget>

class ServerManager;
class QLabel;
class QPushButton;
class QTreeWidget;

/*! The Server Manager's "Players" tab: known players, whitelist, operators, bans, kicks and
 *  the join/leave history.
 *
 *  Changes go through the server console while it runs, and into the access files while it
 *  is stopped.
 */
class ServerPlayersTab : public QWidget
{
    Q_OBJECT

public:
    explicit ServerPlayersTab(QWidget *parent = nullptr);

    void setServerManager(ServerManager *manager);
    /// The server the tab shows; empty for none. Call refresh() to load it.
    void setServerId(const QString &serverId);

    /// Reloads the player list and the latest activity for the current server.
    void refresh();
    /// Enables the actions that apply to the selected player and the server's state.
    void updateActions();

private:
    void whitelistPlayer();
    void makeOperator();
    void banPlayer();
    void kickPlayer();
    void removeAccess();
    void showHistory();
    void exportHistory();
    QStringList playerHistory() const;

    ServerManager *m_serverManager = nullptr;
    QString m_serverId;
    QLabel *m_infoLabel = nullptr;
    QTreeWidget *m_playersTree = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_whitelistButton = nullptr;
    QPushButton *m_opButton = nullptr;
    QPushButton *m_banButton = nullptr;
    QPushButton *m_kickButton = nullptr;
    QPushButton *m_removeAccessButton = nullptr;
    QPushButton *m_viewHistoryButton = nullptr;
    QPushButton *m_exportHistoryButton = nullptr;
};
