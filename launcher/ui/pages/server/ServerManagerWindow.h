// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>
#include <QPointer>

class ServerManager;

/// The separate Server Manager window the main window opens.
namespace ServerManagerWindow {

/// Opens the Server Manager window for manager, or brings the already open one forward.
void show(ServerManager* manager, QWidget* parent, QPointer<QDialog>& window);

/*! Asks before the launcher quits while servers are starting, running, stopping or downloading.
 *  True when it may quit: nothing is active, or the user chose to stop the servers. */
bool confirmQuit(ServerManager* manager, QWidget* parent);

}  // namespace ServerManagerWindow
