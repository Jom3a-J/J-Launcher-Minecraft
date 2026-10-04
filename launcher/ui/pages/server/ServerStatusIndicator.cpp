// SPDX-License-Identifier: GPL-3.0-only

#include "ServerStatusIndicator.h"

#include "server/ServerInstance.h"
#include "server/ServerManager.h"

#include <QAction>
#include <QCoreApplication>
#include <QIcon>

namespace {
/// These texts were written for MainWindow and keep its translation context.
QString mainWindowTr(const char* text)
{
    return QCoreApplication::translate("MainWindow", text);
}
}  // namespace

ServerStatusIndicator::Summary ServerStatusIndicator::summaryFor(int runningCount, int activeCount)
{
    if (activeCount <= 0) {
        return {};
    }
    const bool allRunning = runningCount == activeCount;
    Summary summary;
    summary.allRunning = allRunning;
    summary.text = allRunning ? (runningCount == 1 ? mainWindowTr("1 server running")
                                                   : mainWindowTr("%1 servers running").arg(runningCount))
                              : (activeCount == 1 ? mainWindowTr("1 server process active")
                                                  : mainWindowTr("%1 server processes active").arg(activeCount));
    summary.detail = allRunning
        ? mainWindowTr("Servers keep running when the Server Manager window is closed. Click to manage them.")
        : mainWindowTr("A managed server is starting, stopping, or downloading. Click to open Server Manager.");
    return summary;
}

ServerStatusIndicator::ServerStatusIndicator(ServerManager* manager, QAction* manageServersAction, QWidget* parent)
    : QToolButton(parent), m_manager(manager), m_manageServersAction(manageServersAction)
{
    setObjectName(QStringLiteral("serverStatusButton"));
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setIcon(QIcon::fromTheme(QStringLiteral("status-running")));

    if (manager) {
        for (const auto& server : manager->getAllServers()) {
            watch(server);
        }
        connect(manager, &ServerManager::serverAdded, this, [this](const QString& id) {
            if (m_manager)
                watch(m_manager->getServer(id));
            refresh();
        });
        connect(manager, &ServerManager::serverRemoved, this, &ServerStatusIndicator::refresh);
        connect(manager, &ServerManager::serverChanged, this, &ServerStatusIndicator::refresh);
    }
    refresh();
}

void ServerStatusIndicator::watch(const std::shared_ptr<ServerInstance>& server)
{
    if (!server) {
        return;
    }
    connect(server.get(), &ServerInstance::statusChanged, this, &ServerStatusIndicator::refresh, Qt::UniqueConnection);
}

void ServerStatusIndicator::refresh()
{
    int activeCount = 0;
    int runningCount = 0;
    if (m_manager) {
        for (const auto& server : m_manager->getAllServers()) {
            if (!server) {
                continue;
            }
            switch (server->status()) {
                case ServerStatus::Running:
                    ++runningCount;
                    ++activeCount;
                    break;
                case ServerStatus::Starting:
                case ServerStatus::Stopping:
                case ServerStatus::Downloading:
                    ++activeCount;
                    break;
                case ServerStatus::Stopped:
                case ServerStatus::Error:
                    break;
            }
        }
    }

    const Summary summary = summaryFor(runningCount, activeCount);
    setVisible(activeCount > 0);
    if (activeCount == 0) {
        setText(QString());
        setToolTip(QString());
        setAccessibleName(mainWindowTr("No local servers running"));
        if (m_manageServersAction)
            m_manageServersAction->setToolTip(mainWindowTr("Create and manage local Minecraft servers."));
        return;
    }

    setIcon(QIcon::fromTheme(summary.allRunning ? QStringLiteral("status-running") : QStringLiteral("status-yellow")));
    setText(summary.text);
    setToolTip(summary.text + QStringLiteral("\n") + summary.detail);
    setAccessibleName(summary.text);
    setAccessibleDescription(summary.detail);
    if (m_manageServersAction)
        m_manageServersAction->setToolTip(mainWindowTr("Create and manage local Minecraft servers. %1.").arg(summary.text));
}
