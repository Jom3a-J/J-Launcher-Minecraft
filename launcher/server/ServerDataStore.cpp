// SPDX-License-Identifier: GPL-3.0-only

#include "ServerDataStore.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <utility>

namespace {
constexpr int FormatVersion = 1;
const QString FormatVersionKey = QStringLiteral("formatVersion");

/// Where earlier versions kept each group in the application settings.
const QList<QPair<QString, QString>> &legacyGroups()
{
    static const QList<QPair<QString, QString>> groups{
        { QStringLiteral("ServerAutomation"), ServerDataGroup::Automation },
        { QStringLiteral("ServerMonitoring"), ServerDataGroup::Monitoring },
        { QStringLiteral("ServerDiagnostics"), ServerDataGroup::Diagnostics },
        { QStringLiteral("ServerPlayerHistory"), ServerDataGroup::PlayerHistory },
        { QStringLiteral("ServerContentSources"), ServerDataGroup::ContentSources },
        { QStringLiteral("ServerUpdates"), ServerDataGroup::Updates },
    };
    return groups;
}

/// Values that were written as string lists; a registry may hand a one-item list back as text.
bool isListKey(const QString &group, const QString &key)
{
    return (group == ServerDataGroup::Automation && key == QStringLiteral("history"))
        || (group == ServerDataGroup::PlayerHistory && key == QStringLiteral("events"))
        || (group == ServerDataGroup::Updates && key == QStringLiteral("history"));
}

QJsonValue legacyValue(const QString &group, const QString &key, const QVariant &value)
{
    if (isListKey(group, key)) {
        return QJsonArray::fromStringList(value.toStringList());
    }
    return QJsonValue::fromVariant(value);
}

/// Server ids become file names, so anything that could leave the folder is refused.
bool isSafeServerId(const QString &serverId)
{
    return !serverId.isEmpty() && serverId != QStringLiteral(".") && serverId != QStringLiteral("..")
        && !serverId.contains(QLatin1Char('/')) && !serverId.contains(QLatin1Char('\\'))
        && !serverId.contains(QLatin1Char(':'));
}
}  // namespace

ServerDataStore::ServerDataStore(QString directory) : m_directory(std::move(directory)) {}

QString ServerDataStore::filePath(const QString &serverId) const
{
    return isSafeServerId(serverId) ? QDir(m_directory).filePath(serverId + QStringLiteral(".json")) : QString();
}

QJsonObject &ServerDataStore::document(const QString &serverId) const
{
    auto existing = m_documents.find(serverId);
    if (existing != m_documents.end()) {
        return *existing;
    }
    QJsonObject loaded;
    const QString path = filePath(serverId);
    QFile file(path);
    if (!path.isEmpty() && file.open(QIODevice::ReadOnly)) {
        const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll());
        if (parsed.isObject()) {
            loaded = parsed.object();
        } else {
            qWarning() << "Ignoring unreadable server records" << QFileInfo(path).fileName();
        }
    }
    return *m_documents.insert(serverId, loaded);
}

bool ServerDataStore::save(const QString &serverId) const
{
    const QString path = filePath(serverId);
    if (path.isEmpty() || !QDir().mkpath(m_directory)) {
        return false;
    }
    QJsonObject contents = document(serverId);
    contents.insert(FormatVersionKey, FormatVersion);
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(QJsonDocument(contents).toJson(QJsonDocument::Indented)) >= 0
        && file.commit();
}

QVariant ServerDataStore::value(const QString &serverId, const QString &group, const QString &key,
                                const QVariant &defaultValue) const
{
    const QJsonValue stored = document(serverId).value(group).toObject().value(key);
    return stored.isUndefined() || stored.isNull() ? defaultValue : stored.toVariant();
}

bool ServerDataStore::setValue(const QString &serverId, const QString &group, const QString &key,
                               const QVariant &value)
{
    QJsonObject &contents = document(serverId);
    QJsonObject groupObject = contents.value(group).toObject();
    groupObject.insert(key, QJsonValue::fromVariant(value));
    contents.insert(group, groupObject);
    return save(serverId);
}

bool ServerDataStore::remove(const QString &serverId, const QString &group, const QString &key)
{
    QJsonObject &contents = document(serverId);
    QJsonObject groupObject = contents.value(group).toObject();
    if (!groupObject.contains(key)) {
        return true;
    }
    groupObject.remove(key);
    contents.insert(group, groupObject);
    return save(serverId);
}

QStringList ServerDataStore::list(const QString &serverId, const QString &group, const QString &key) const
{
    return value(serverId, group, key).toStringList();
}

bool ServerDataStore::addToList(const QString &serverId, const QString &group, const QString &key,
                                const QString &entry, int maxEntries, Order order)
{
    QStringList entries = list(serverId, group, key);
    if (order == Order::NewestFirst) {
        entries.prepend(entry);
        while (entries.size() > maxEntries) entries.removeLast();
    } else {
        entries.append(entry);
        while (entries.size() > maxEntries) entries.removeFirst();
    }
    return setValue(serverId, group, key, entries);
}

bool ServerDataStore::importLegacySettings(const QString &serverId)
{
    const QString path = filePath(serverId);
    if (path.isEmpty() || QFileInfo::exists(path)) {
        return false;
    }

    QSettings settings;
    QJsonObject &contents = document(serverId);
    bool found = false;
    for (const auto &[legacyGroup, group] : legacyGroups()) {
        settings.beginGroup(legacyGroup + QLatin1Char('/') + serverId);
        QJsonObject groupObject = contents.value(group).toObject();
        for (const QString &key : settings.childKeys()) {
            groupObject.insert(key, legacyValue(group, key, settings.value(key)));
            found = true;
        }
        settings.endGroup();
        if (!groupObject.isEmpty()) {
            contents.insert(group, groupObject);
        }
    }

    // Each updated mod had its own settings group of details.
    settings.beginGroup(QStringLiteral("ServerContentMetadata/") + serverId);
    QJsonObject metadata = contents.value(ServerDataGroup::ContentMetadata).toObject();
    for (const QString &fileName : settings.childGroups()) {
        settings.beginGroup(fileName);
        QJsonObject details;
        for (const QString &key : settings.childKeys()) {
            details.insert(key, QJsonValue::fromVariant(settings.value(key)));
        }
        settings.endGroup();
        if (!details.isEmpty()) {
            metadata.insert(fileName, details);
            found = true;
        }
    }
    settings.endGroup();
    if (!metadata.isEmpty()) {
        contents.insert(ServerDataGroup::ContentMetadata, metadata);
    }

    return found && save(serverId);
}
