// SPDX-License-Identifier: GPL-3.0-only

#include "ServerManagerWindow.h"

#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "ui/ModelessWindow.h"
#include "ui/pages/server/ServerListPage.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace ServerManagerWindow {

namespace {
/// These texts were written for MainWindow and keep its translation context.
QString mainWindowTr(const char* text, int n = -1)
{
    return QCoreApplication::translate("MainWindow", text, nullptr, n);
}
}  // namespace

void show(ServerManager* manager, QWidget* parent, QPointer<QDialog>& window)
{
    if (!manager) {
        return;
    }

    if (window) {
        UI::Modeless::activate(window.data());
        return;
    }

    auto* dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("serverManagerWindow"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(mainWindowTr("Server Manager"));
    dialog->setMinimumSize(980, 640);
    dialog->resize(980, 640);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* serverPage = new ServerListPage(dialog);
    serverPage->setServerManager(manager);
    layout->addWidget(serverPage);
    UI::Modeless::showOrActivate(window, [dialog] { return dialog; });
}

bool confirmQuit(ServerManager* manager, QWidget* parent)
{
    if (!manager) {
        return true;
    }
    int runningServerCount = 0;
    for (const auto& server : manager->getAllServers()) {
        const ServerStatus status = server->status();
        if (status == ServerStatus::Running || status == ServerStatus::Starting || status == ServerStatus::Stopping
            || status == ServerStatus::Downloading) {
            ++runningServerCount;
        }
    }
    if (runningServerCount == 0) {
        return true;
    }

    QMessageBox warning(QMessageBox::Warning, mainWindowTr("Servers Are Running"),
                        mainWindowTr("%n server(s) are still running. Closing J Launcher will stop them safely; each world is saved first.",
                                     runningServerCount),
                        QMessageBox::NoButton, parent);
    QAbstractButton* stopButton = warning.addButton(mainWindowTr("Stop Servers and Quit"), QMessageBox::AcceptRole);
    warning.addButton(QMessageBox::Cancel);
    warning.setDefaultButton(QMessageBox::Cancel);
    warning.exec();
    return warning.clickedButton() == stopButton;
}

}  // namespace ServerManagerWindow
