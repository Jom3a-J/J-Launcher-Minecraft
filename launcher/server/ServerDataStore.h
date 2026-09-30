// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariant>

/// Names of the groups a server's records are kept under.
namespace ServerDataGroup {
/// Daily schedule (enabled, action, time, retention, lastRun) and its history.
inline const QString Automation = QStringLiteral("automation");
/// Warning levels for the live CPU, RAM and disk figures.
inline const QString Monitoring = QStringLiteral("monitoring");
/// The latest crash: lastCrash (summary) and details.
inline const QString Diagnostics = QStringLiteral("diagnostics");
/// Join and leave events, under "events".
inline const QString PlayerHistory = QStringLiteral("playerHistory");
/// Where each installed mod came from: file name -> "provider:project:file".
inline const QString ContentSources = QStringLiteral("contentSources");
/// Details of mods installed by update: file name -> {versionId, url, hash, ...}.
inline const QString ContentMetadata = QStringLiteral("contentMetadata");
/// Software update history and the latest rollback backup.
inline const QString Updates = QStringLiteral("updates");
}  // namespace ServerDataGroup

/*! What the launcher records about each server beyond its settings in servers.json.
 *
 *  Kept as one JSON file per server, <folder>/<server id>.json, loaded on first use.
 *  Earlier versions kept these records in the application settings (the Windows registry);
 *  importLegacySettings copies them over once and leaves the old values where they were.
 */
class ServerDataStore
{
public:
    explicit ServerDataStore(QString directory);

    QString directory() const { return m_directory; }

    QVariant value(const QString &serverId, const QString &group, const QString &key,
                   const QVariant &defaultValue = {}) const;
    /// Stores a value and saves the server's file. False when the file could not be written.
    bool setValue(const QString &serverId, const QString &group, const QString &key,
                  const QVariant &value);
    bool remove(const QString &serverId, const QString &group, const QString &key);

    QStringList list(const QString &serverId, const QString &group, const QString &key) const;
    /// Adds an entry to a list, keeping at most maxEntries by dropping the oldest ones.
    enum class Order { NewestFirst, NewestLast };
    bool addToList(const QString &serverId, const QString &group, const QString &key,
                   const QString &entry, int maxEntries, Order order);

    /*! Copies a server's records from the application settings used by earlier versions.
     *
     *  Does nothing when the server already has a file, so it only ever runs once per server.
     *  Returns true when records were found and saved.
     */
    bool importLegacySettings(const QString &serverId);

private:
    QJsonObject &document(const QString &serverId) const;
    bool save(const QString &serverId) const;
    QString filePath(const QString &serverId) const;

    QString m_directory;
    mutable QHash<QString, QJsonObject> m_documents;
};
