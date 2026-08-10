// SPDX-License-Identifier: GPL-3.0-only

#include "ServerPlayerAccess.h"

#include "ServerInstance.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

namespace {
QString normalizedUuid(QString uuid)
{
    uuid.remove('-');
    return uuid.toLower();
}

bool validUuid(const QString& uuid, QString* error)
{
    static const QRegularExpression uuidPattern(
        QStringLiteral("^(?:[0-9a-fA-F]{32}|[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-"
                       "[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$"));
    if (uuidPattern.match(uuid.trimmed()).hasMatch()) {
        return true;
    }
    if (error) {
        *error = QObject::tr("The selected player UUID is invalid.");
    }
    return false;
}

bool validIdentity(const QString& uuid, const QString& name, QString* error)
{
    static const QRegularExpression namePattern(QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
    if (!validUuid(uuid, error)) {
        return false;
    }
    if (!namePattern.match(name.trimmed()).hasMatch()) {
        if (error) {
            *error = QObject::tr("The selected player name is invalid.");
        }
        return false;
    }
    return true;
}

bool ensureEditable(const std::shared_ptr<ServerInstance>& server, QString* error)
{
    if (!server
        || (server->status() != ServerStatus::Stopped && server->status() != ServerStatus::Error)) {
        if (error) {
            *error = QObject::tr("Stop the selected server before changing player access files.");
        }
        return false;
    }
    return true;
}

bool readArray(const QString& path, QJsonArray* array, QString* error)
{
    if (!array) {
        return false;
    }
    if (!QFileInfo::exists(path)) {
        *array = {};
        return true;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QObject::tr("Could not read %1.").arg(QFileInfo(path).fileName());
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (error) {
            *error = QObject::tr("%1 contains invalid JSON and was not changed.")
                         .arg(QFileInfo(path).fileName());
        }
        return false;
    }
    *array = document.array();
    return true;
}

bool writeArray(const QString& path, const QJsonArray& array, QString* error)
{
    if (!QDir().mkpath(QFileInfo(path).dir().absolutePath())) {
        if (error) {
            *error = QObject::tr("Could not prepare %1.").arg(QFileInfo(path).fileName());
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(array).toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        file.cancelWriting();
        if (error) {
            *error = QObject::tr("Could not save %1.").arg(QFileInfo(path).fileName());
        }
        return false;
    }
    return true;
}

int firstPlayerIndex(const QJsonArray& players, const QString& uuid)
{
    const QString target = normalizedUuid(uuid);
    for (int index = 0; index < players.size(); ++index) {
        if (normalizedUuid(players.at(index).toObject().value("uuid").toString()) == target) {
            return index;
        }
    }
    return -1;
}

void removePlayerEntries(QJsonArray* players, const QString& uuid, int exceptIndex = -1)
{
    const QString target = normalizedUuid(uuid);
    for (int index = players->size() - 1; index >= 0; --index) {
        if (index != exceptIndex
            && normalizedUuid(players->at(index).toObject().value("uuid").toString()) == target) {
            players->removeAt(index);
        }
    }
}

bool updateEntry(const std::shared_ptr<ServerInstance>& server, const QString& fileName,
                 const QString& uuid, const QString& name, bool enabled,
                 const QJsonObject& replacement, QString* error)
{
    if (!ensureEditable(server, error) || !validIdentity(uuid, name, error)) {
        return false;
    }
    const QString path = QDir(server->serverDirectory()).filePath(fileName);
    QJsonArray players;
    if (!readArray(path, &players, error)) {
        return false;
    }
    int index = firstPlayerIndex(players, uuid);
    if (!enabled) {
        removePlayerEntries(&players, uuid);
    } else if (index < 0) {
        players.append(replacement);
    } else {
        players[index] = replacement;
        removePlayerEntries(&players, uuid, index);
    }
    return writeArray(path, players, error);
}
}

QList<ServerPlayerInfo> ServerPlayerAccess::listPlayers(
    const std::shared_ptr<ServerInstance>& server, QString* error)
{
    QList<ServerPlayerInfo> result;
    if (!server) {
        if (error) {
            *error = QObject::tr("The selected server is unavailable.");
        }
        return result;
    }

    QMap<QString, ServerPlayerInfo> players;
    const auto merge = [&players, error](const QString& path, int kind) {
        QJsonArray entries;
        if (!readArray(path, &entries, error)) {
            return false;
        }
        for (const QJsonValue& value : entries) {
            const QJsonObject object = value.toObject();
            const QString uuid = object.value("uuid").toString().trimmed();
            if (uuid.isEmpty()) {
                continue;
            }
            ServerPlayerInfo& player = players[normalizedUuid(uuid)];
            player.uuid = uuid;
            const QString name = object.value("name").toString().trimmed();
            if (!name.isEmpty()) {
                player.name = name;
            }
            if (kind == 1) {
                player.whitelisted = true;
            } else if (kind == 2) {
                player.operatorEnabled = true;
                player.operatorLevel = object.value("level").toInt();
            } else if (kind == 3) {
                player.banned = true;
            }
        }
        return true;
    };

    const QDir directory(server->serverDirectory());
    if (!merge(directory.filePath("usercache.json"), 0)
        || !merge(directory.filePath("whitelist.json"), 1)
        || !merge(directory.filePath("ops.json"), 2)
        || !merge(directory.filePath("banned-players.json"), 3)) {
        return {};
    }
    result = players.values();
    std::sort(result.begin(), result.end(), [](const ServerPlayerInfo& left,
                                               const ServerPlayerInfo& right) {
        const int nameOrder = left.name.compare(right.name, Qt::CaseInsensitive);
        return nameOrder == 0 ? left.uuid < right.uuid : nameOrder < 0;
    });
    return result;
}

bool ServerPlayerAccess::setWhitelisted(const std::shared_ptr<ServerInstance>& server,
                                        const QString& uuid, const QString& name,
                                        bool enabled, QString* error)
{
    return updateEntry(server, QStringLiteral("whitelist.json"), uuid, name, enabled,
                       QJsonObject{ { "uuid", uuid.trimmed() }, { "name", name.trimmed() } },
                       error);
}

bool ServerPlayerAccess::setOperator(const std::shared_ptr<ServerInstance>& server,
                                     const QString& uuid, const QString& name, int level,
                                     QString* error)
{
    if (level < 1 || level > 4) {
        if (error) {
            *error = QObject::tr("Choose an operator level from 1 to 4.");
        }
        return false;
    }
    return updateEntry(server, QStringLiteral("ops.json"), uuid, name, true,
                       QJsonObject{ { "uuid", uuid.trimmed() }, { "name", name.trimmed() },
                                    { "level", level }, { "bypassesPlayerLimit", false } },
                       error);
}

bool ServerPlayerAccess::setBanned(const std::shared_ptr<ServerInstance>& server,
                                   const QString& uuid, const QString& name, bool enabled,
                                   const QString& reason, QString* error)
{
    QString safeReason = reason.simplified();
    if (safeReason.isEmpty()) {
        safeReason = QObject::tr("Banned from J Launcher");
    }
    if (safeReason.size() > 256) {
        if (error) {
            *error = QObject::tr("The ban reason is too long.");
        }
        return false;
    }
    return updateEntry(
        server, QStringLiteral("banned-players.json"), uuid, name, enabled,
        QJsonObject{ { "uuid", uuid.trimmed() }, { "name", name.trimmed() },
                     { "created", QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss +0000") },
                     { "source", "J Launcher" }, { "expires", "forever" },
                     { "reason", safeReason } },
        error);
}

bool ServerPlayerAccess::clearAccess(const std::shared_ptr<ServerInstance>& server,
                                     const QString& uuid, QString* error)
{
    if (!ensureEditable(server, error) || !validUuid(uuid, error)) {
        return false;
    }
    static const QStringList fileNames{
        QStringLiteral("whitelist.json"), QStringLiteral("ops.json"),
        QStringLiteral("banned-players.json")
    };
    struct FileState {
        QString path;
        QJsonArray original;
        QJsonArray updated;
        bool existed = false;
    };
    QList<FileState> states;
    for (const QString& fileName : fileNames) {
        FileState state;
        state.path = QDir(server->serverDirectory()).filePath(fileName);
        state.existed = QFileInfo::exists(state.path);
        if (!readArray(state.path, &state.original, error)) {
            return false;
        }
        state.updated = state.original;
        removePlayerEntries(&state.updated, uuid);
        states.append(state);
    }

    int written = 0;
    for (; written < states.size(); ++written) {
        if (!writeArray(states[written].path, states[written].updated, error)) {
            for (int rollback = 0; rollback < written; ++rollback) {
                if (states[rollback].existed) {
                    writeArray(states[rollback].path, states[rollback].original, nullptr);
                } else {
                    QFile::remove(states[rollback].path);
                }
            }
            return false;
        }
    }
    return true;
}
