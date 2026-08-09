/* SPDX-License-Identifier: GPL-3.0-only */

#include "CurseForgeHash.h"

#include "net/ChecksumValidator.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

namespace {

std::optional<Flame::CurseForgeHash> makeHash(const QString &algorithmName,
                                               const QString &value)
{
    const QString algorithm = algorithmName.trimmed().toLower();
    const QString normalizedValue = value.trimmed();
    QCryptographicHash::Algorithm cryptographicAlgorithm;
    int expectedLength = 0;
    if (algorithm == QStringLiteral("sha1")) {
        cryptographicAlgorithm = QCryptographicHash::Sha1;
        expectedLength = 40;
    } else if (algorithm == QStringLiteral("md5")) {
        cryptographicAlgorithm = QCryptographicHash::Md5;
        expectedLength = 32;
    } else {
        return std::nullopt;
    }

    if (normalizedValue.size() != expectedLength
        || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]+$")).match(normalizedValue).hasMatch()) {
        return std::nullopt;
    }

    return Flame::CurseForgeHash{ cryptographicAlgorithm, algorithm, normalizedValue };
}

}  // namespace

namespace Flame {

std::optional<CurseForgeHash> parseCurseForgeHash(const QJsonObject &hashObject)
{
    const QJsonValue algorithmValue = hashObject.value(QStringLiteral("algo"));
    if (!algorithmValue.isDouble()) {
        return std::nullopt;
    }
    const int algorithm = algorithmValue.toInt();
    if (algorithmValue.toDouble() != algorithm) {
        return std::nullopt;
    }

    const QJsonValue value = hashObject.value(QStringLiteral("value"));
    if (!value.isString()) {
        return std::nullopt;
    }

    switch (algorithm) {
        case 1:
            return makeHash(QStringLiteral("sha1"), value.toString());
        case 2:
            return makeHash(QStringLiteral("md5"), value.toString());
        default:
            return std::nullopt;
    }
}

Net::ChecksumValidator *createCurseForgeChecksumValidator(const QString &algorithmName,
                                                           const QString &value)
{
    const auto hash = makeHash(algorithmName, value);
    if (!hash) {
        return nullptr;
    }
    return new Net::ChecksumValidator(hash->algorithm, hash->value);
}

}  // namespace Flame
