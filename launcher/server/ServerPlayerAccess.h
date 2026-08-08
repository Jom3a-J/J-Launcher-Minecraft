// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QList>
#include <QString>
#include <memory>

class ServerInstance;

struct ServerPlayerInfo {
    QString name;
    QString uuid;
    bool whitelisted = false;
    bool operatorEnabled = false;
    int operatorLevel = 0;
    bool banned = false;
};

class ServerPlayerAccess final
{
public:
    static QList<ServerPlayerInfo> listPlayers(
        const std::shared_ptr<ServerInstance>& server, QString* error = nullptr);
    static bool setWhitelisted(const std::shared_ptr<ServerInstance>& server,
                               const QString& uuid, const QString& name, bool enabled,
                               QString* error = nullptr);
    static bool setOperator(const std::shared_ptr<ServerInstance>& server,
                            const QString& uuid, const QString& name, int level,
                            QString* error = nullptr);
    static bool setBanned(const std::shared_ptr<ServerInstance>& server,
                          const QString& uuid, const QString& name, bool enabled,
                          const QString& reason = QString(), QString* error = nullptr);
    static bool clearAccess(const std::shared_ptr<ServerInstance>& server,
                            const QString& uuid, QString* error = nullptr);
};
