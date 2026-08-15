/* Copyright 2013-2024 MultiMC Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QMap>
#include <QStack>
#include <QDateTime>
#include <QStringList>
#include <memory>

class ServerInstance;

struct ServerBackupInfo {
    QString name;
    QString path;
    QString serverId;
    QString serverName;
    QString minecraftVersion;
    QString loaderType;
    QString loaderVersion;
    QDateTime createdAt;
    QStringList includedCategories;
    qint64 size = 0;
    bool valid = false;
    QString validationError;
};

class ServerManager : public QObject
{
    Q_OBJECT

public:
    explicit ServerManager(const QString &dataDir, QObject *parent = nullptr);
    ~ServerManager();

    // Server management
    std::shared_ptr<ServerInstance> createServer(const QString &name, const QString &version,
                                                 const QString &loaderType = "vanilla",
                                                 const QString &loaderVersion = QString());
    bool deleteServer(const QString &id);
    bool deleteServerPermanently(const QString &id);
    bool hasDeletedServer() const { return !m_trashHistory.isEmpty(); }
    bool restoreLastDeletedServer(QString *restoredId = nullptr);
    bool createServerBackup(const QString &id, const QString &requestedName,
                            ServerBackupInfo *createdBackup, QString *error);
    QList<ServerBackupInfo> listServerBackups(const QString &id) const;
    bool restoreServerBackup(const QString &id, const QString &backupPath, QString *error,
                             QString *safetyBackupName = nullptr);
    bool deleteServerBackup(const QString &id, const QString &backupPath, QString *error);
    bool enforceServerBackupRetention(const QString &id, const QString &namePrefix,
                                      int maximumBackups, QString *error);

    // Get servers
    std::shared_ptr<ServerInstance> getServer(const QString &id) const;
    QList<std::shared_ptr<ServerInstance>> getAllServers() const;
    int serverCount() const { return m_servers.size(); }

    // Save/Load
    bool save();
    bool load();

signals:
    void serverAdded(const QString &id);
    void serverRemoved(const QString &id);
    void serverChanged(const QString &id);

private:
    QString generateId() const;

    QString m_dataDir;
    QString m_serversFile;
    QMap<QString, std::shared_ptr<ServerInstance>> m_servers;
    struct TrashHistoryItem {
        QString id;
        QString originalPath;
        QString trashPath;
        std::shared_ptr<ServerInstance> server;
    };
    QStack<TrashHistoryItem> m_trashHistory;
};
