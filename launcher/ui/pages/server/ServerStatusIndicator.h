// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QPointer>
#include <QString>
#include <QToolButton>

#include <memory>

class QAction;
class ServerInstance;
class ServerManager;

/*! The main window's status-bar button that says how many local servers are running. It hides
 *  while none are, and also keeps the Manage Servers action's tooltip up to date. */
class ServerStatusIndicator : public QToolButton {
    Q_OBJECT

   public:
    struct Summary {
        QString text;    //!< Empty when no server is active.
        QString detail;
        bool allRunning = false;
    };
    /// What the button says for this many running servers out of this many active ones.
    static Summary summaryFor(int runningCount, int activeCount);

    ServerStatusIndicator(ServerManager* manager, QAction* manageServersAction, QWidget* parent = nullptr);

    /// Counts the servers again and updates the button, after a language change for instance.
    void refresh();

   private:
    void watch(const std::shared_ptr<ServerInstance>& server);

    QPointer<ServerManager> m_manager;
    QPointer<QAction> m_manageServersAction;
};
