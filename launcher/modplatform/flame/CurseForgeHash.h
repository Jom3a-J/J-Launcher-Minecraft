/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <QCryptographicHash>
#include <QString>

#include <optional>

class QJsonObject;

namespace Net {
class ChecksumValidator;
}

namespace Flame {

struct CurseForgeHash {
    QCryptographicHash::Algorithm algorithm;
    QString algorithmName;
    QString value;
};

std::optional<CurseForgeHash> parseCurseForgeHash(const QJsonObject &hashObject);
Net::ChecksumValidator *createCurseForgeChecksumValidator(const QString &algorithmName,
                                                           const QString &value);

}  // namespace Flame
