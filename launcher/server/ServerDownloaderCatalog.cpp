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

// Reading the version and build lists that server software providers publish.

#include "ServerDownloader.h"
#include "ServerDownloaderShared.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QVersionNumber>
#include <QXmlStreamReader>
#include <algorithm>

using namespace ServerDownloaderDetail;

namespace {
void sortBuildVersions(QStringList &builds)
{
    builds.removeAll(QString());
    QSet<QString> unique(builds.begin(), builds.end());
    builds = QStringList(unique.begin(), unique.end());
    std::sort(builds.begin(), builds.end(), [](const QString &left, const QString &right) {
        bool leftNumber = false;
        bool rightNumber = false;
        const qlonglong leftValue = left.toLongLong(&leftNumber);
        const qlonglong rightValue = right.toLongLong(&rightNumber);
        if (leftNumber && rightNumber) return leftValue > rightValue;
        const int comparison = QVersionNumber::compare(QVersionNumber::fromString(left),
                                                        QVersionNumber::fromString(right));
        return comparison == 0 ? left > right : comparison > 0;
    });
}
}  // namespace

QStringList ServerDownloader::parseAvailableVersions(const QString &type, const QByteArray &data,
                                                     QString *errorMessage)
{
    const QString provider = type.trimmed().toLower();
    if (provider == QStringLiteral("forge") && data.trimmed().startsWith('<')) {
        QStringList versions;
        QXmlStreamReader xml(data);
        const QRegularExpression coordinatePattern(
            QStringLiteral("^((?:1\\.)?\\d+(?:\\.\\d+){0,2})-(.+)$"));
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QStringLiteral("version")) {
                const QRegularExpressionMatch match = coordinatePattern.match(
                    xml.readElementText().trimmed());
                if (match.hasMatch()) {
                    versions.append(match.captured(1));
                }
            }
        }
        if (xml.hasError()) {
            if (errorMessage) {
                *errorMessage = tr("The Forge version response was not valid Maven metadata.");
            }
            return {};
        }
        versions.removeDuplicates();
        std::sort(versions.begin(), versions.end(), [](const QString &left,
                                                        const QString &right) {
            const int comparison = QVersionNumber::compare(
                QVersionNumber::fromString(left), QVersionNumber::fromString(right));
            return comparison == 0 ? left > right : comparison > 0;
        });
        if (versions.isEmpty() && errorMessage) {
            *errorMessage = tr("The Forge service returned no supported Minecraft versions.");
        }
        return versions;
    }

    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (document.isNull()) {
        if (errorMessage) {
            *errorMessage = tr("The %1 version response was not valid JSON.").arg(type);
        }
        return {};
    }

    QStringList versions;
    if (provider == "vanilla") {
        for (const QJsonValue &value : document.object()["versions"].toArray()) {
            const QJsonObject version = value.toObject();
            versions.append(version["id"].toString());
        }
    } else if (provider == "paper") {
        const QJsonObject groupedVersions = document.object()["versions"].toObject();
        for (const QJsonValue &group : groupedVersions) {
            for (const QJsonValue &version : group.toArray()) {
                versions.append(version.toString());
            }
        }
    } else if (provider == "fabric") {
        for (const QJsonValue &value : document.array()) {
            const QJsonObject version = value.toObject();
            versions.append(version["version"].toString());
        }
    } else if (provider == "purpur") {
        for (const QJsonValue &version : document.object()["versions"].toArray()) {
            versions.append(version.toString());
        }
    } else if (provider == "forge") {
        const QJsonObject promotions = document.object()["promos"].toObject();
        for (auto iterator = promotions.constBegin(); iterator != promotions.constEnd(); ++iterator) {
            QString key = iterator.key();
            if (key.endsWith("-recommended")) {
                key.chop(QString("-recommended").size());
                versions.append(key);
            } else if (key.endsWith("-latest")) {
                key.chop(QString("-latest").size());
                versions.append(key);
            }
        }
    } else if (provider == "neoforge") {
        for (const QJsonValue &value : document.object()["versions"].toArray()) {
            const QStringList parts = value.toString().split(QRegularExpression("[.-]"), Qt::SkipEmptyParts);
            if (parts.size() < 2) {
                continue;
            }
            bool majorValid = false;
            const int major = parts.at(0).toInt(&majorValid);
            if (!majorValid) {
                continue;
            }
            versions.append(major >= 26
                                ? QString("%1.%2").arg(parts.at(0), parts.at(1))
                                : QString("1.%1.%2").arg(parts.at(0), parts.at(1)));
        }
        // NeoForge 1.20.1 was published under the legacy
        // net.neoforged:forge coordinate and is therefore absent from the
        // modern net.neoforged:neoforge version endpoint. Its builds are
        // resolved from the legacy Maven metadata when selected.
        versions.append(QStringLiteral("1.20.1"));
    } else {
        if (errorMessage) {
            *errorMessage = tr("Unsupported server type: %1").arg(type);
        }
        return {};
    }

    versions.removeAll(QString());
    QSet<QString> uniqueVersions(versions.begin(), versions.end());
    versions = QStringList(uniqueVersions.begin(), uniqueVersions.end());
    std::sort(versions.begin(), versions.end(), [](const QString &left, const QString &right) {
        const int comparison = QVersionNumber::compare(QVersionNumber::fromString(left),
                                                        QVersionNumber::fromString(right));
        return comparison == 0 ? left > right : comparison > 0;
    });
    if (versions.isEmpty() && errorMessage) {
        *errorMessage = tr("The %1 service returned no supported Minecraft versions.").arg(type);
    }
    return versions;
}

QStringList ServerDownloader::parseAvailableBuilds(const QString &type, const QString &version,
                                                    const QByteArray &data, QString *errorMessage)
{
    const QString provider = type.trimmed().toLower();
    if ((provider == "forge" || provider == "neoforge")
        && data.trimmed().startsWith('<')) {
        QStringList builds;
        QXmlStreamReader xml(data);
        const QString prefix = version + '-';
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QStringLiteral("version")) {
                const QString publishedVersion = xml.readElementText().trimmed();
                if (publishedVersion.startsWith(prefix)) {
                    builds.append(publishedVersion.mid(prefix.size()));
                }
            }
        }
        if (xml.hasError()) {
            if (errorMessage) {
                *errorMessage = tr("The %1 build response was not valid Maven metadata.")
                                    .arg(type);
            }
            return {};
        }
        sortBuildVersions(builds);
        if (builds.isEmpty() && errorMessage) {
            *errorMessage = tr("The %1 service returned no builds for Minecraft %2.")
                                .arg(type, version);
        }
        return builds;
    }

    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (document.isNull()) {
        if (errorMessage) *errorMessage = tr("The %1 build response was not valid JSON.").arg(type);
        return {};
    }

    QStringList builds;
    if (provider == "paper") {
        const QJsonArray entries = document.isArray()
            ? document.array() : document.object()["builds"].toArray();
        for (const QJsonValue &value : entries) {
            const QJsonObject build = value.toObject();
            const int number = build["id"].toInt(build["build"].toInt());
            if (number > 0) builds.append(QString::number(number));
        }
    } else if (provider == "fabric") {
        for (const QJsonValue &value : document.array()) {
            builds.append(value.toObject()["loader"].toObject()["version"].toString());
        }
    } else if (provider == "purpur") {
        const QJsonArray entries = document.object()["builds"].toObject()["all"].toArray();
        for (const QJsonValue &value : entries) {
            builds.append(value.isString() ? value.toString() : QString::number(value.toInt()));
        }
    } else if (provider == "forge") {
        const QJsonObject promotions = document.object()["promos"].toObject();
        builds.append(promotions[version + "-recommended"].toString());
        builds.append(promotions[version + "-latest"].toString());
    } else if (provider == "neoforge") {
        QString minecraftPrefix = version;
        if (minecraftPrefix.startsWith("1.")) minecraftPrefix = minecraftPrefix.mid(2);
        for (const QJsonValue &value : document.object()["versions"].toArray()) {
            const QString candidate = value.toString();
            if (candidate.startsWith(minecraftPrefix + '.')
                || candidate.startsWith(minecraftPrefix + '-')) {
                builds.append(candidate);
            }
        }
    } else {
        if (errorMessage) *errorMessage = tr("Unsupported server type: %1").arg(type);
        return {};
    }

    sortBuildVersions(builds);
    if (builds.isEmpty() && errorMessage) {
        *errorMessage = tr("The %1 service returned no builds for Minecraft %2.").arg(type, version);
    }
    return builds;
}
