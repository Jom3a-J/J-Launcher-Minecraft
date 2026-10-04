// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

class QNetworkAccessManager;
class QObject;

/*! Technic's per-pack details, which say whether a pack has a server pack. The modpack list asks
 *  for every pack it shows, so requests share one queue that limits how many run at once and
 *  remembers the answers. */
namespace Technic {

inline constexpr int PackDetailsConcurrency = 4;
using PackDetailsCallback = std::function<void(std::optional<QJsonObject>)>;
/// Asks for a pack's details for owner; the callback gets them, or nothing when the request failed.
void requestPackDetails(QNetworkAccessManager* network, const QString& slug, QObject* owner, PackDetailsCallback callback);
/// Remembers details that already arrived with a search, so they are not asked for again.
void cachePackDetails(const QString& slug, const QJsonObject& details);
/// Drops owner's waiting requests and stops its running ones.
void cancelPackDetailsRequests(QObject* owner);

}  // namespace Technic
