// SPDX-License-Identifier: GPL-3.0-only

#include "ServerPlayersTab.h"

#include <QAction>
#include <QCursor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "server/ServerPlayerAccess.h"

ServerPlayersTab::ServerPlayersTab(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("playersTab"));
    auto *layout = new QVBoxLayout(this);
    m_infoLabel = new QLabel(tr("Known players are read from usercache.json. Stop the server before changing whitelist, operator, or ban entries."), this);
    m_infoLabel->setObjectName(QStringLiteral("playersInfoLabel"));
    m_infoLabel->setWordWrap(true);
    layout->addWidget(m_infoLabel);
    m_playersTree = new QTreeWidget(this);
    m_playersTree->setObjectName(QStringLiteral("playersTree"));
    m_playersTree->setColumnCount(5);
    m_playersTree->setHeaderLabels({tr("Player"), tr("UUID"), tr("Whitelisted"), tr("Operator"), tr("Banned")});
    m_playersTree->setAlternatingRowColors(true);
    m_playersTree->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_playersTree, 1);

    auto *actions = new QGridLayout();
    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_whitelistButton = new QPushButton(tr("Whitelist"), this);
    m_opButton = new QPushButton(tr("Make Operator"), this);
    m_banButton = new QPushButton(tr("Ban"), this);
    m_kickButton = new QPushButton(tr("Kick"), this);
    m_removeAccessButton = new QPushButton(tr("Remove Access"), this);
    m_viewHistoryButton = new QPushButton(tr("Activity History"), this);
    m_exportHistoryButton = new QPushButton(tr("Export History"), this);
    m_refreshButton->setObjectName(QStringLiteral("refreshPlayersButton"));
    m_whitelistButton->setObjectName(QStringLiteral("whitelistPlayerButton"));
    m_opButton->setObjectName(QStringLiteral("opPlayerButton"));
    m_banButton->setObjectName(QStringLiteral("banPlayerButton"));
    m_kickButton->setObjectName(QStringLiteral("kickPlayerButton"));
    m_removeAccessButton->setObjectName(QStringLiteral("removePlayerAccessButton"));
    actions->addWidget(m_refreshButton, 0, 0);
    actions->addWidget(m_whitelistButton, 0, 1);
    actions->addWidget(m_opButton, 0, 2);
    actions->addWidget(m_banButton, 0, 3);
    actions->addWidget(m_kickButton, 1, 0);
    actions->addWidget(m_removeAccessButton, 1, 1);
    actions->addWidget(m_viewHistoryButton, 1, 2);
    actions->addWidget(m_exportHistoryButton, 1, 3);
    for (int column = 0; column < 4; ++column) {
        actions->setColumnStretch(column, 1);
    }
    layout->addLayout(actions);

    connect(m_refreshButton, &QPushButton::clicked, this, &ServerPlayersTab::refresh);
    connect(m_whitelistButton, &QPushButton::clicked, this, &ServerPlayersTab::whitelistPlayer);
    connect(m_opButton, &QPushButton::clicked, this, &ServerPlayersTab::makeOperator);
    connect(m_banButton, &QPushButton::clicked, this, &ServerPlayersTab::banPlayer);
    connect(m_kickButton, &QPushButton::clicked, this, &ServerPlayersTab::kickPlayer);
    connect(m_removeAccessButton, &QPushButton::clicked, this, &ServerPlayersTab::removeAccess);
    connect(m_viewHistoryButton, &QPushButton::clicked, this, &ServerPlayersTab::showHistory);
    connect(m_exportHistoryButton, &QPushButton::clicked, this, &ServerPlayersTab::exportHistory);
    connect(m_playersTree, &QTreeWidget::currentItemChanged, this, &ServerPlayersTab::updateActions);
}

void ServerPlayersTab::setServerManager(ServerManager *manager)
{
    m_serverManager = manager;
}

void ServerPlayersTab::setServerId(const QString &serverId)
{
    m_serverId = serverId;
}

QStringList ServerPlayersTab::playerHistory() const
{
    return m_serverManager
        ? m_serverManager->dataStore().list(m_serverId, ServerDataGroup::PlayerHistory, "events")
        : QStringList();
}

void ServerPlayersTab::refresh()
{
    const QString selectedUuid = m_playersTree->currentItem()
        ? m_playersTree->currentItem()->data(0, Qt::UserRole).toString() : QString();
    const QSignalBlocker selectionBlocker(m_playersTree);
    m_playersTree->clear();
    if (!m_serverManager || m_serverId.isEmpty()) {
        m_infoLabel->setText(tr("Select a server to view known players and manage access."));
        return;
    }
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) return;

    QString error;
    const QList<ServerPlayerInfo> players = ServerPlayerAccess::listPlayers(server, &error);
    if (!error.isEmpty()) {
        m_infoLabel->setText(tr("Could not read player access files: %1").arg(error));
        updateActions();
        return;
    }

    for (const ServerPlayerInfo &player : players) {
        auto *item = new QTreeWidgetItem(m_playersTree);
        item->setText(0, player.name.isEmpty() ? tr("Unknown player") : player.name);
        item->setText(1, player.uuid);
        item->setText(2, player.whitelisted ? tr("Yes") : tr("No"));
        item->setText(3, player.operatorEnabled
                         ? tr("Yes (level %1)").arg(player.operatorLevel) : tr("No"));
        item->setText(4, player.banned ? tr("Yes") : tr("No"));
        item->setData(0, Qt::UserRole, player.uuid);
        if (!selectedUuid.isEmpty() && player.uuid == selectedUuid) {
            m_playersTree->setCurrentItem(item);
        }
    }
    if (!m_playersTree->currentItem() && m_playersTree->topLevelItemCount() > 0) {
        m_playersTree->setCurrentItem(m_playersTree->topLevelItem(0));
    }
    m_playersTree->resizeColumnToContents(0);
    m_playersTree->resizeColumnToContents(2);
    m_playersTree->resizeColumnToContents(3);
    m_playersTree->resizeColumnToContents(4);
    const QStringList history = playerHistory();
    const QString activity = history.isEmpty() ? QString() : tr(" Latest activity: %1").arg(history.last());
    const QString managementMode = server->status() == ServerStatus::Running
        ? tr(" Live actions are sent through the server console; operator level uses server.properties.")
        : tr(" Access files can be edited safely while the server is stopped.");
    m_infoLabel->setText((players.isEmpty()
        ? tr("No known players yet. Players appear after they connect, or when listed in whitelist, ops, or bans.")
        : tr("%1 known player(s).").arg(players.size())) + managementMode + activity);
    updateActions();
}

void ServerPlayersTab::updateActions()
{
    const auto server = m_serverManager && !m_serverId.isEmpty()
        ? m_serverManager->getServer(m_serverId) : nullptr;
    const bool hasSelection = server != nullptr;
    const ServerStatus status = server ? server->status() : ServerStatus::Stopped;
    const bool canEditFiles = status == ServerStatus::Stopped || status == ServerStatus::Error;
    m_refreshButton->setEnabled(hasSelection);
    const bool hasPlayer = m_playersTree->currentItem()
        && !m_playersTree->currentItem()->data(0, Qt::UserRole).toString().isEmpty();
    const bool canManagePlayerAccess = canEditFiles || status == ServerStatus::Running;
    m_whitelistButton->setEnabled(hasPlayer && canManagePlayerAccess);
    m_opButton->setEnabled(hasPlayer && canManagePlayerAccess);
    m_banButton->setEnabled(hasPlayer && canManagePlayerAccess);
    m_kickButton->setEnabled(hasPlayer && status == ServerStatus::Running);
    m_removeAccessButton->setEnabled(hasPlayer && canManagePlayerAccess);
    m_viewHistoryButton->setEnabled(hasSelection);
    m_exportHistoryButton->setEnabled(hasSelection);
}

void ServerPlayersTab::whitelistPlayer()
{
    auto *item = m_playersTree->currentItem();
    if (!item || !m_serverManager) return;
    const auto server = m_serverManager->getServer(m_serverId);
    const QString uuid = item->data(0, Qt::UserRole).toString();
    if (!server || uuid.isEmpty()) return;
    QString error;
    if (server->status() == ServerStatus::Running) {
        if (!server->setPlayerWhitelistedLive(item->text(0), true, &error)) {
            QMessageBox::warning(this, tr("Whitelist"), error);
            return;
        }
        item->setText(2, tr("Yes"));
        m_infoLabel->setText(
            tr("Sent live whitelist command for %1. The Players tab and selection were kept.")
                .arg(item->text(0)));
        updateActions();
        return;
    }
    if (!ServerPlayerAccess::setWhitelisted(server, uuid, item->text(0), true, &error)) {
        QMessageBox::warning(this, tr("Whitelist"), error);
        return;
    }
    refresh();
}

void ServerPlayersTab::makeOperator()
{
    auto *item = m_playersTree->currentItem();
    if (!item || !m_serverManager) return;
    const auto server = m_serverManager->getServer(m_serverId);
    const QString uuid = item->data(0, Qt::UserRole).toString();
    if (!server || uuid.isEmpty()) return;

    if (server->status() == ServerStatus::Running) {
        QString error;
        if (!server->setPlayerOperatorLive(item->text(0), true, &error)) {
            QMessageBox::warning(this, tr("Operators"), error);
            return;
        }
        item->setText(3, tr("Yes (server default level)"));
        m_infoLabel->setText(
            tr("Sent live operator command for %1. The permission level comes from server.properties while running.")
                .arg(item->text(0)));
        updateActions();
        return;
    }

    QMenu levelMenu(this);
    const QList<QPair<int, QString>> levels = {
        {1, tr("Level 1 — bypass spawn protection")},
        {2, tr("Level 2 — use command blocks")},
        {3, tr("Level 3 — manage players")},
        {4, tr("Level 4 — full server control")}
    };
    for (const auto &level : levels) {
        QAction *action = levelMenu.addAction(level.second);
        action->setData(level.first);
    }
    QAction *selected = levelMenu.exec(QCursor::pos());
    if (!selected) return;

    QString error;
    if (!ServerPlayerAccess::setOperator(server, uuid, item->text(0),
                                         selected->data().toInt(), &error)) {
        QMessageBox::warning(this, tr("Operators"), error);
        return;
    }
    refresh();
}

void ServerPlayersTab::banPlayer()
{
    auto *item = m_playersTree->currentItem();
    if (!item || !m_serverManager) return;
    const auto server = m_serverManager->getServer(m_serverId);
    const QString uuid = item->data(0, Qt::UserRole).toString();
    if (!server || uuid.isEmpty()) return;
    if (QMessageBox::question(this, tr("Ban Player"),
                              tr("Ban %1 from this server?").arg(item->text(0)))
        != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (server->status() == ServerStatus::Running) {
        if (!server->setPlayerBannedLive(item->text(0), true,
                                         tr("Banned from J Launcher"), &error)) {
            QMessageBox::warning(this, tr("Ban Player"), error);
            return;
        }
        item->setText(4, tr("Yes"));
        m_infoLabel->setText(
            tr("Sent live ban command for %1. The Players tab and selection were kept.")
                .arg(item->text(0)));
        updateActions();
        return;
    }
    if (!ServerPlayerAccess::setBanned(server, uuid, item->text(0), true,
                                       tr("Banned from J Launcher"), &error)) {
        QMessageBox::warning(this, tr("Ban Player"), error);
        return;
    }
    refresh();
}

void ServerPlayersTab::kickPlayer()
{
    auto *item = m_playersTree->currentItem();
    if (!item || !m_serverManager) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) return;

    bool accepted = false;
    const QString reason = QInputDialog::getText(
        this, tr("Kick Player"), tr("Reason:"), QLineEdit::Normal,
        tr("Removed by server operator"), &accepted);
    if (!accepted) return;

    QString error;
    if (!server->kickPlayer(item->text(0), reason, &error)) {
        QMessageBox::warning(this, tr("Kick Player"), error);
    }
}

void ServerPlayersTab::removeAccess()
{
    auto *item = m_playersTree->currentItem();
    if (!item || !m_serverManager) return;
    const auto server = m_serverManager->getServer(m_serverId);
    const QString uuid = item->data(0, Qt::UserRole).toString();
    if (!server || uuid.isEmpty()) return;
    QString error;
    if (server->status() == ServerStatus::Running) {
        if (!server->clearPlayerAccessLive(item->text(0), &error)) {
            QMessageBox::warning(this, tr("Remove Access"), error);
            return;
        }
        item->setText(2, tr("No"));
        item->setText(3, tr("No"));
        item->setText(4, tr("No"));
        m_infoLabel->setText(
            tr("Sent live whitelist removal, de-op, and pardon commands for %1.")
                .arg(item->text(0)));
        updateActions();
        return;
    }
    if (!ServerPlayerAccess::clearAccess(server, uuid, &error)) {
        QMessageBox::warning(this, tr("Remove Access"), error);
        return;
    }
    refresh();
}

void ServerPlayersTab::showHistory()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) return;
    const QStringList history = playerHistory();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Player Activity - %1").arg(server->name()));
    dialog.setMinimumSize(620, 400);
    auto *layout = new QVBoxLayout(&dialog);
    auto *filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(tr("Search player activity..."));
    auto *list = new QListWidget(&dialog);
    list->setAlternatingRowColors(true);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    const auto populate = [list, history](const QString &term) {
        list->clear();
        for (const QString &event : history) {
            if (term.isEmpty() || event.contains(term, Qt::CaseInsensitive)) list->addItem(event);
        }
        if (list->count() == 0) list->addItem(QObject::tr("No matching activity."));
    };
    populate(QString());
    connect(filter, &QLineEdit::textChanged, &dialog, populate);
    layout->addWidget(filter);
    layout->addWidget(list, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void ServerPlayersTab::exportHistory()
{
    if (!m_serverManager || m_serverId.isEmpty()) return;
    const auto server = m_serverManager->getServer(m_serverId);
    if (!server) return;
    const QStringList history = playerHistory();
    const QString suggested = QDir::home().filePath(server->name().simplified().replace(' ', '-') + "-player-history.json");
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Player Activity"), suggested,
                                                       tr("JSON files (*.json);;CSV files (*.csv)"));
    if (path.isEmpty()) return;

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Export Player Activity"), tr("Could not create the selected export file."));
        return;
    }
    if (path.endsWith(".csv", Qt::CaseInsensitive)) {
        auto quote = [](QString value) {
            value.replace('"', "\"\"");
            return '"' + value + '"';
        };
        QString csv = "event\n";
        for (const QString &event : history) csv += quote(event) + '\n';
        output.write(csv.toUtf8());
    } else {
        QJsonArray events;
        for (const QString &event : history) events.append(event);
        QJsonObject document{{"serverId", server->id()}, {"serverName", server->name()},
                             {"exportedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                             {"events", events}};
        output.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
    }
    if (!output.commit()) {
        QMessageBox::warning(this, tr("Export Player Activity"), tr("Could not finish writing the export file."));
        return;
    }
    QMessageBox::information(this, tr("Player Activity Exported"), tr("Saved player activity to %1.").arg(QDir::toNativeSeparators(path)));
}
