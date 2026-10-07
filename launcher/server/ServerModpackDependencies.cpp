// SPDX-License-Identifier: GPL-3.0-only

#include "ServerModpackInstaller.h"
#include "ServerPaths.h"
#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "server/ServerPackCompatibility.h"
#include "server/ServerProperties.h"
#include "HardwareInfo.h"

#include "archive/ArchiveReader.h"
#include "minecraft/mod/MetadataHandler.h"
#include "modplatform/ModIndex.h"
#include "modplatform/helpers/HashUtils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

#include <toml++/toml.h>

#include <algorithm>
#include <utility>

using ServerPaths::isSafeRelativePath;

namespace {
// Definite: complete parsing proved a missing mandatory dependency or a
// definite incompatible version. Blocks derived servers; published packs stay
// authoritative and let the loader validate at startup.
// Inconclusive: parser limits, malformed metadata, invalid/unverifiable
// ranges. Advisory for both published and derived servers.
// Unsafe: traversal/absolute paths or containment violations. Always blocks.
enum class DependencyValidationFailure {
    None,
    Definite,
    Inconclusive,
    Unsafe,
};

struct FabricDependencyRequirement {
    QString modId;
    QString modName;
    QString dependencyId;
};

bool collectFabricMetadata(const QString &jarPath, const QString &temporaryRoot,
                           int depth, int *nestedJarIndex,
                           QSet<QString> *providedIds,
                           QList<FabricDependencyRequirement> *requirements,
                           DependencyValidationFailure *failure,
                           QString *error)
{
    if (depth > 8) {
        if (failure) {
            *failure = DependencyValidationFailure::Inconclusive;
        }
        if (error) {
            *error = QObject::tr(
                "The Fabric pack contains more than eight nested mod levels and cannot be validated safely.");
        }
        return false;
    }
    MMCZip::ArchiveReader archive(jarPath);
    const auto metadataFile = archive.goToFile(QStringLiteral("fabric.mod.json"));
    if (!metadataFile) return true;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        (*metadataFile)->readAll().value_or(QByteArray()), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return true;
    }
    const QJsonObject metadata = document.object();
    const QString modId = metadata.value(QStringLiteral("id"))
                              .toString().trimmed().toLower();
    if (modId.isEmpty()) return true;

    providedIds->insert(modId);
    for (const QJsonValue &provided :
         metadata.value(QStringLiteral("provides")).toArray()) {
        const QString providedId = provided.toString().trimmed().toLower();
        if (!providedId.isEmpty()) providedIds->insert(providedId);
    }

    const QString modName = metadata.value(QStringLiteral("name"))
                                .toString(modId).trimmed();
    const QJsonObject dependencies =
        metadata.value(QStringLiteral("depends")).toObject();
    for (auto it = dependencies.constBegin(); it != dependencies.constEnd(); ++it) {
        const QString dependencyId = it.key().trimmed().toLower();
        if (dependencyId.isEmpty() || dependencyId == QStringLiteral("fabricloader")
            || dependencyId == QStringLiteral("minecraft")
            || dependencyId == QStringLiteral("java")) {
            continue;
        }
        requirements->append({ modId, modName, dependencyId });
    }

    for (const QJsonValue &nestedValue :
         metadata.value(QStringLiteral("jars")).toArray()) {
        const QString nestedPath = nestedValue.toObject()
                                       .value(QStringLiteral("file"))
                                       .toString().trimmed();
        if (!isSafeRelativePath(nestedPath)) {
            if (failure) {
                *failure = DependencyValidationFailure::Unsafe;
            }
            if (error) {
                *error = QObject::tr(
                    "Fabric mod \"%1\" declares an unsafe bundled dependency path: %2")
                             .arg(modName, nestedPath);
            }
            return false;
        }
        MMCZip::ArchiveReader nestedSource(jarPath);
        const auto nestedFile = nestedSource.goToFile(nestedPath);
        if (!nestedFile) {
            if (failure) {
                *failure = DependencyValidationFailure::Definite;
            }
            if (error) {
                *error = QObject::tr(
                             "Fabric mod \"%1\" declares a bundled dependency that is missing: %2")
                             .arg(modName, nestedPath);
            }
            return false;
        }
        const QString extractedPath = QDir(temporaryRoot).filePath(
            QStringLiteral("nested-%1.jar").arg((*nestedJarIndex)++));
        QFile extracted(extractedPath);
        const QByteArray contents = (*nestedFile)->readAll().value_or(QByteArray());
        if (!extracted.open(QIODevice::WriteOnly)
            || extracted.write(contents) != contents.size()) {
            if (error) {
                *error = QObject::tr(
                    "Could not inspect a bundled Fabric dependency safely.");
            }
            return false;
        }
        extracted.close();
        if (!collectFabricMetadata(extractedPath, temporaryRoot, depth + 1,
                                   nestedJarIndex, providedIds, requirements,
                                   failure, error)) {
            return false;
        }
    }
    return true;
}

bool validateFabricDependencyClosure(const QString &serverRoot,
                                     DependencyValidationFailure *failure,
                                     QStringList *missingDependencyIds,
                                     QString *error)
{
    if (failure) {
        *failure = DependencyValidationFailure::Inconclusive;
    }
    QSet<QString> providedIds{
        QStringLiteral("fabricloader"), QStringLiteral("minecraft"),
        QStringLiteral("java")
    };
    QList<FabricDependencyRequirement> requirements;
    QTemporaryDir nestedJars;
    if (!nestedJars.isValid()) {
        if (error) {
            *error = QObject::tr(
                "Could not create temporary storage for Fabric dependency validation.");
        }
        return false;
    }
    int nestedJarIndex = 0;
    const QDir modsDirectory(QDir(serverRoot).filePath(QStringLiteral("mods")));
    for (const QFileInfo &jar : modsDirectory.entryInfoList(
             QStringList() << QStringLiteral("*.jar"), QDir::Files)) {
        if (!collectFabricMetadata(jar.absoluteFilePath(), nestedJars.path(), 0,
                                   &nestedJarIndex, &providedIds, &requirements,
                                   failure, error)) {
            return false;
        }
    }

    std::sort(requirements.begin(), requirements.end(),
              [](const auto &left, const auto &right) {
                  if (left.modId != right.modId) return left.modId < right.modId;
                  return left.dependencyId < right.dependencyId;
              });
    // Report every unsatisfied requirement in one pass. Stopping at the first
    // one forces the user through a separate repair round per missing mod.
    QStringList missingDescriptions;
    for (const auto &requirement : requirements) {
        if (providedIds.contains(requirement.dependencyId)) {
            continue;
        }
        if (missingDependencyIds
            && !missingDependencyIds->contains(requirement.dependencyId)) {
            missingDependencyIds->append(requirement.dependencyId);
        }
        missingDescriptions.append(
            QObject::tr("Mod \"%1\" (%2) requires \"%3\", but that dependency is missing.")
                .arg(requirement.modName, requirement.modId,
                     requirement.dependencyId));
    }
    if (!missingDescriptions.isEmpty()) {
        if (failure) {
            *failure = DependencyValidationFailure::Definite;
        }
        if (error) {
            *error = QObject::tr("The pack does not contain a complete Fabric server. %1")
                         .arg(missingDescriptions.join(QStringLiteral(" ")));
        }
        return false;
    }
    return true;
}

enum class MavenRangeResult {
    Matches,
    DoesNotMatch,
    Invalid,
    Unverifiable,
};

bool parseNumericMavenVersion(const QString &version, QStringList *parts)
{
    static const QRegularExpression numericVersion(
        QStringLiteral(R"(^[0-9]+(?:[._-][0-9]+)*$)"));
    const QString normalized = version.trimmed();
    if (!numericVersion.match(normalized).hasMatch()) {
        return false;
    }

    *parts = normalized.split(QRegularExpression(QStringLiteral(R"([._-])")));
    for (QString &part : *parts) {
        while (part.size() > 1 && part.startsWith(QLatin1Char('0'))) {
            part.remove(0, 1);
        }
    }
    while (parts->size() > 1 && parts->constLast() == QStringLiteral("0")) {
        parts->removeLast();
    }
    return true;
}

int compareNumericMavenVersions(const QStringList &left, const QStringList &right)
{
    const qsizetype count = std::max(left.size(), right.size());
    for (qsizetype index = 0; index < count; ++index) {
        const QString leftPart = index < left.size() ? left.at(index)
                                                     : QStringLiteral("0");
        const QString rightPart = index < right.size() ? right.at(index)
                                                       : QStringLiteral("0");
        if (leftPart.size() != rightPart.size()) {
            return leftPart.size() < rightPart.size() ? -1 : 1;
        }
        const int comparison = QString::compare(leftPart, rightPart);
        if (comparison != 0) {
            return comparison < 0 ? -1 : 1;
        }
    }
    return 0;
}

MavenRangeResult evaluateNumericMavenRange(const QString &range,
                                           const QString &installedVersion)
{
    const QString specification = range.trimmed();
    if (specification.isEmpty()) {
        return MavenRangeResult::Matches;
    }
    if (!specification.startsWith(QLatin1Char('['))
        && !specification.startsWith(QLatin1Char('('))) {
        // Maven treats an unbracketed version as a soft recommendation, not a
        // hard restriction. Any installed version satisfies it.
        return MavenRangeResult::Matches;
    }

    QStringList installedParts;
    const bool installedIsComparable = parseNumericMavenVersion(
        installedVersion, &installedParts);
    bool unverifiable = false;
    bool sawRestriction = false;
    qsizetype position = 0;
    while (position < specification.size()) {
        while (position < specification.size()
               && specification.at(position).isSpace()) {
            ++position;
        }
        if (position == specification.size()) {
            return MavenRangeResult::Invalid;
        }
        const QChar opening = specification.at(position);
        if (opening != QLatin1Char('[') && opening != QLatin1Char('(')) {
            return MavenRangeResult::Invalid;
        }
        qsizetype closingIndex = -1;
        for (qsizetype index = position + 1; index < specification.size(); ++index) {
            const QChar candidate = specification.at(index);
            if (candidate == QLatin1Char(']') || candidate == QLatin1Char(')')) {
                closingIndex = index;
                break;
            }
        }
        if (closingIndex < 0) {
            return MavenRangeResult::Invalid;
        }

        const QChar closing = specification.at(closingIndex);
        const QString body = specification.mid(
            position + 1, closingIndex - position - 1);
        const qsizetype separator = body.indexOf(QLatin1Char(','));
        QString lower;
        QString upper;
        bool lowerInclusive = opening == QLatin1Char('[');
        bool upperInclusive = closing == QLatin1Char(']');
        if (separator < 0) {
            if (!lowerInclusive || !upperInclusive || body.trimmed().isEmpty()) {
                return MavenRangeResult::Invalid;
            }
            lower = body.trimmed();
            upper = lower;
        } else {
            if (body.indexOf(QLatin1Char(','), separator + 1) >= 0) {
                return MavenRangeResult::Invalid;
            }
            lower = body.left(separator).trimmed();
            upper = body.mid(separator + 1).trimmed();
            // Maven ignores inclusivity for a missing bound, so both
            // [1,) and [1,] mean 1 or newer (and likewise for a missing
            // lower bound). Forge uses Maven's VersionRange parser.
            if (lower.isEmpty() && upper.isEmpty()) {
                return MavenRangeResult::Invalid;
            }
        }
        sawRestriction = true;

        QStringList lowerParts;
        QStringList upperParts;
        const bool lowerComparable = lower.isEmpty()
            || parseNumericMavenVersion(lower, &lowerParts);
        const bool upperComparable = upper.isEmpty()
            || parseNumericMavenVersion(upper, &upperParts);
        if (!installedIsComparable || !lowerComparable || !upperComparable) {
            unverifiable = true;
        } else {
            bool matches = true;
            if (!lower.isEmpty()) {
                const int comparison = compareNumericMavenVersions(
                    installedParts, lowerParts);
                matches = comparison > 0 || (comparison == 0 && lowerInclusive);
            }
            if (matches && !upper.isEmpty()) {
                const int comparison = compareNumericMavenVersions(
                    installedParts, upperParts);
                matches = comparison < 0 || (comparison == 0 && upperInclusive);
            }
            if (matches) {
                return MavenRangeResult::Matches;
            }
        }

        position = closingIndex + 1;
        if (position == specification.size()) {
            break;
        }
        if (specification.at(position) != QLatin1Char(',')) {
            return MavenRangeResult::Invalid;
        }
        ++position;
        while (position < specification.size()
               && specification.at(position).isSpace()) {
            ++position;
        }
        if (position == specification.size()) {
            return MavenRangeResult::Invalid;
        }
    }
    if (!sawRestriction) {
        return MavenRangeResult::Invalid;
    }
    return unverifiable ? MavenRangeResult::Unverifiable
                        : MavenRangeResult::DoesNotMatch;
}

bool isNewerPatchInSameMinecraftLine(const QString &range,
                                     const QString &installedVersion)
{
    static const QRegularExpression exactPatchRange(
        QStringLiteral(R"(^\[([0-9]+)\.([0-9]+)\.([0-9]+)\]$)"));
    static const QRegularExpression installedPatchVersion(
        QStringLiteral(R"(^([0-9]+)\.([0-9]+)\.([0-9]+)$)"));
    const auto required = exactPatchRange.match(range.trimmed());
    const auto installed = installedPatchVersion.match(installedVersion.trimmed());
    if (!required.hasMatch() || !installed.hasMatch()
        || required.captured(1) != installed.captured(1)
        || required.captured(2) != installed.captured(2)) {
        return false;
    }
    return installed.captured(3).toLongLong()
        > required.captured(3).toLongLong();
}

QString manifestImplementationVersion(const QString &jarPath)
{
    MMCZip::ArchiveReader archive(jarPath);
    const auto manifest = archive.goToFile(QStringLiteral("META-INF/MANIFEST.MF"));
    if (!manifest) {
        return {};
    }
    const QStringList lines = QString::fromUtf8((*manifest)->readAll().value_or(QByteArray())).split(
        QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r")));
    for (const QString &line : lines) {
        static const QString prefix = QStringLiteral("Implementation-Version:");
        if (line.startsWith(prefix, Qt::CaseInsensitive)) {
            return line.mid(prefix.size()).trimmed();
        }
    }
    return {};
}

QString resolveForgeModVersion(const QString &declaredVersion,
                               const toml::table &document,
                               const QString &manifestVersion)
{
    const QString version = declaredVersion.trimmed();
    static const QRegularExpression propertyReference(
        QStringLiteral(R"(^\$\{file\.([A-Za-z0-9_.-]+)\}$)"));
    const auto match = propertyReference.match(version);
    if (!match.hasMatch()) {
        return version;
    }
    const QString property = match.captured(1);
    if (property == QStringLiteral("jarVersion")) {
        return manifestVersion;
    }
    const auto properties = document["properties"].as_table();
    if (!properties) {
        return {};
    }
    const auto propertyValue = (*properties)[property.toStdString()].as_string();
    return propertyValue
        ? QString::fromStdString(propertyValue->get()).trimmed() : QString();
}

enum class ForgeDependencyKind {
    Required,
    Optional,
    Incompatible,
};

struct ForgeDependencyRequirement {
    QString modId;
    QString modName;
    QString dependencyId;
    QString versionRange;
    ForgeDependencyKind kind = ForgeDependencyKind::Required;
};

bool collectForgeMetadata(const QString &jarPath, const QString &loaderType,
                          QSet<QString> *providedIds,
                          QHash<QString, QString> *providedVersions,
                          QList<ForgeDependencyRequirement> *requirements,
                          QStringList *warnings, QString *error)
{
    const bool isNeoForge = loaderType == QStringLiteral("neoforge");
    const QStringList metadataPaths = isNeoForge
        ? QStringList{ QStringLiteral("META-INF/neoforge.mods.toml"),
                       QStringLiteral("META-INF/mods.toml") }
        : QStringList{ QStringLiteral("META-INF/mods.toml") };

    QByteArray contents;
    QString metadataPath;
    for (const QString &candidate : metadataPaths) {
        MMCZip::ArchiveReader archive(jarPath);
        if (const auto metadata = archive.goToFile(candidate)) {
            contents = (*metadata)->readAll().value_or(QByteArray());
            metadataPath = candidate;
            break;
        }
    }
    if (metadataPath.isEmpty()) {
        return true;
    }

    toml::table document;
#if TOML_EXCEPTIONS
    try {
        document = toml::parse(contents.toStdString());
    } catch ([[maybe_unused]] const toml::parse_error &parseError) {
        if (warnings) {
            warnings->append(
                QObject::tr("Could not strictly parse %1 metadata in %2; "
                            "the loader will verify this mod at server startup.")
                    .arg(isNeoForge ? QObject::tr("NeoForge")
                                   : QObject::tr("Forge"),
                         QFileInfo(jarPath).fileName()));
        }
        return true;
    }
#else
    toml::parse_result parseResult = toml::parse(contents.toStdString());
    if (!parseResult) {
        if (warnings) {
            warnings->append(
                QObject::tr("Could not strictly parse %1 metadata in %2; "
                            "the loader will verify this mod at server startup.")
                    .arg(isNeoForge ? QObject::tr("NeoForge")
                                   : QObject::tr("Forge"),
                         QFileInfo(jarPath).fileName()));
        }
        return true;
    }
    document = std::move(parseResult).table();
#endif

    const auto mods = document["mods"].as_array();
    if (!mods || mods->empty()) {
        if (error) {
            *error = QObject::tr("The %1 metadata in %2 declares no mods.")
                         .arg(isNeoForge ? QObject::tr("NeoForge")
                                        : QObject::tr("Forge"),
                              QFileInfo(jarPath).fileName());
        }
        return false;
    }

    struct DeclaredMod {
        std::string metadataId;
        QString id;
        QString name;
    };
    const QString manifestVersion = manifestImplementationVersion(jarPath);
    QList<DeclaredMod> declaredMods;
    for (const auto &entry : *mods) {
        const auto mod = entry.as_table();
        const auto idValue = mod ? (*mod)["modId"].as_string() : nullptr;
        if (!idValue) {
            if (error) {
                *error = QObject::tr("The %1 metadata in %2 contains a mod without an ID.")
                             .arg(isNeoForge ? QObject::tr("NeoForge")
                                            : QObject::tr("Forge"),
                                  QFileInfo(jarPath).fileName());
            }
            return false;
        }
        const std::string metadataId = idValue->get();
        const QString id = QString::fromStdString(metadataId).trimmed().toLower();
        if (id.isEmpty()) {
            if (error) {
                *error = QObject::tr("The %1 metadata in %2 contains an empty mod ID.")
                             .arg(isNeoForge ? QObject::tr("NeoForge")
                                            : QObject::tr("Forge"),
                                  QFileInfo(jarPath).fileName());
            }
            return false;
        }
        QString name = id;
        if (const auto nameValue = (*mod)["displayName"].as_string()) {
            name = QString::fromStdString(nameValue->get()).trimmed();
            if (name.isEmpty()) {
                name = id;
            }
        }
        QString declaredVersion = isNeoForge ? QStringLiteral("1") : QString();
        if (const auto versionValue = (*mod)["version"].as_string()) {
            declaredVersion = QString::fromStdString(versionValue->get());
        }
        declaredMods.append({ metadataId, id, name });
        providedIds->insert(id);
        providedVersions->insert(
            id, resolveForgeModVersion(declaredVersion, document, manifestVersion));
    }

    const auto dependencyGroups = document["dependencies"].as_table();
    if (!dependencyGroups) {
        return true;
    }
    for (const DeclaredMod &declared : declaredMods) {
        const auto dependencies = (*dependencyGroups)[declared.metadataId].as_array();
        if (!dependencies) {
            continue;
        }
        for (const auto &entry : *dependencies) {
            const auto dependency = entry.as_table();
            const auto dependencyIdValue = dependency
                ? (*dependency)["modId"].as_string() : nullptr;
            if (!dependencyIdValue) {
                if (error) {
                    *error = QObject::tr(
                                 "The %1 metadata for mod \"%2\" contains a dependency without an ID.")
                                 .arg(isNeoForge ? QObject::tr("NeoForge")
                                                : QObject::tr("Forge"),
                                      declared.name);
                }
                return false;
            }
            const QString dependencyId = QString::fromStdString(
                dependencyIdValue->get()).trimmed().toLower();
            if (dependencyId.isEmpty()) {
                continue;
            }

            QString side = QStringLiteral("BOTH");
            if (const auto sideValue = (*dependency)["side"].as_string()) {
                side = QString::fromStdString(sideValue->get()).trimmed().toUpper();
            }
            if (side == QStringLiteral("CLIENT")) {
                continue;
            }
            if (side != QStringLiteral("BOTH") && side != QStringLiteral("SERVER")) {
                if (error) {
                    *error = QObject::tr(
                                 "The %1 metadata for mod \"%2\" declares an unknown dependency side: %3")
                                 .arg(isNeoForge ? QObject::tr("NeoForge")
                                                : QObject::tr("Forge"),
                                      declared.name, side);
                }
                return false;
            }

            // The Java runtime is chosen by the launcher, not installed as a
            // mod, so a requirement on it must never reach the missing list.
            if (dependencyId == QStringLiteral("java")) {
                continue;
            }

            QString versionRange;
            if (const auto rangeValue = (*dependency)["versionRange"].as_string()) {
                versionRange = QString::fromStdString(rangeValue->get()).trimmed();
            }
            if (isNeoForge) {
                QString type = QStringLiteral("required");
                if (const auto typeValue = (*dependency)["type"].as_string()) {
                    type = QString::fromStdString(typeValue->get()).trimmed().toLower();
                } else if (const auto legacyMandatory =
                               (*dependency)["mandatory"].as_boolean()) {
                    type = legacyMandatory->get() ? QStringLiteral("required")
                                                  : QStringLiteral("optional");
                }
                if (type == QStringLiteral("required")) {
                    requirements->append({ declared.id, declared.name, dependencyId,
                                           versionRange, ForgeDependencyKind::Required });
                } else if (type == QStringLiteral("optional")) {
                    requirements->append({ declared.id, declared.name, dependencyId,
                                           versionRange, ForgeDependencyKind::Optional });
                } else if (type == QStringLiteral("incompatible")) {
                    requirements->append({ declared.id, declared.name, dependencyId,
                                           versionRange, ForgeDependencyKind::Incompatible });
                } else if (type != QStringLiteral("discouraged")) {
                    if (error) {
                        *error = QObject::tr(
                                     "The NeoForge metadata for mod \"%1\" declares an unknown dependency type: %2")
                                     .arg(declared.name, type);
                    }
                    return false;
                }
            } else {
                const auto mandatory = (*dependency)["mandatory"].as_boolean();
                requirements->append({
                    declared.id, declared.name, dependencyId, versionRange,
                    mandatory && mandatory->get() ? ForgeDependencyKind::Required
                                                  : ForgeDependencyKind::Optional });
            }
        }
    }
    return true;
}

bool collectForgeBundledMetadata(const QString &jarPath, const QString &loaderType,
                                 const QString &temporaryRoot, int depth, int *count,
                                 QSet<QString> *providedIds,
                                 QHash<QString, QString> *providedVersions,
                                 QList<ForgeDependencyRequirement> *requirements,
                                 QStringList *warnings,
                                 DependencyValidationFailure *failure,
                                 QString *error)
{
    if (depth > 8 || ++*count > 1024) {
        if (failure) *failure = DependencyValidationFailure::Inconclusive;
        if (error) *error = QObject::tr("Bundled mod dependencies exceed the inspection limit.");
        return false;
    }
    if (!collectForgeMetadata(jarPath, loaderType, providedIds, providedVersions,
                              requirements, warnings, error)) return false;

    // Extracts one embedded jar and inspects it under the same rules.
    auto inspectEmbeddedJar = [&](const QString &path) -> bool {
        if (!isSafeRelativePath(path) || !path.endsWith(".jar", Qt::CaseInsensitive)) {
            if (failure) *failure = DependencyValidationFailure::Unsafe;
            if (error) *error = QObject::tr("Unsafe bundled dependency path in %1.").arg(QFileInfo(jarPath).fileName());
            return false;
        }
        MMCZip::ArchiveReader nestedArchive(jarPath);
        const auto entry = nestedArchive.goToFile(path);
        if (!entry || !(*entry)->isFile()) {
            if (failure) *failure = DependencyValidationFailure::Definite;
            if (error) *error = QObject::tr("Bundled dependency %1 is missing from %2.").arg(path, QFileInfo(jarPath).fileName());
            return false;
        }
        // Use a generated destination, never a path supplied by the archive.
        const QString extracted = QDir(temporaryRoot).filePath(QString::number(*count) + ".jar");
        const QByteArray bytes = (*entry)->readAll().value_or(QByteArray());
        QFile file(extracted);
        if (bytes.isEmpty() || !file.open(QIODevice::WriteOnly)
            || file.write(bytes) != bytes.size()) {
            if (error) *error = QObject::tr("Could not inspect bundled dependency %1.").arg(path);
            return false;
        }
        file.close();
        return collectForgeBundledMetadata(extracted, loaderType, temporaryRoot,
                                           depth + 1, count, providedIds, providedVersions,
                                           requirements, warnings, failure, error);
    };

    MMCZip::ArchiveReader archive(jarPath);
    const auto metadata = archive.goToFile("META-INF/jarjar/metadata.json");
    if (!metadata) {
        // Not every mod ships the jarjar index. Connector, for instance, points
        // at its embedded mod through the jar manifest instead, so the mod ids
        // it provides stay invisible unless the folder itself is inspected -
        // and the pack is then reported incomplete however often the user
        // installs the very mod that is already sitting there.
        MMCZip::ArchiveReader listing(jarPath);
        if (!listing.collectFiles()) {
            return true;
        }
        for (const QString &entry : listing.getFiles()) {
            if (entry.startsWith(QStringLiteral("META-INF/jarjar/"), Qt::CaseInsensitive)
                && entry.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive)
                && !inspectEmbeddedJar(entry)) {
                return false;
            }
        }
        return true;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson((*metadata)->readAll().value_or(QByteArray()), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !document.object().value("jars").isArray()) {
        if (error) *error = QObject::tr("Invalid bundled dependency metadata in %1.").arg(QFileInfo(jarPath).fileName());
        return false;
    }
    for (const auto &value : document.object().value("jars").toArray()) {
        if (!inspectEmbeddedJar(value.toObject().value("path").toString())) {
            return false;
        }
    }
    return true;
}

bool validateForgeDependencyClosure(const QString &serverRoot,
                                    const QString &loaderType,
                                    const QString &minecraftVersion,
                                    const QString &loaderVersion,
                                    QStringList *warnings,
                                    DependencyValidationFailure *failure,
                                    QStringList *missingDependencyIds,
                                    QString *error)
{
    if (failure) {
        *failure = DependencyValidationFailure::Inconclusive;
    }
    const bool isNeoForge = loaderType == QStringLiteral("neoforge");
    if (!isNeoForge) {
        bool hasLoaderMajor = false;
        const int loaderMajor = loaderVersion.section(QLatin1Char('.'), 0, 0)
                                    .toInt(&hasLoaderMajor);
        if (hasLoaderMajor && loaderMajor < 25) {
            if (warnings) {
                warnings->append(QObject::tr(
                    "Legacy Forge does not use META-INF/mods.toml dependency metadata; "
                    "the derived server dependency closure remains unverified."));
            }
            return true;
        }
    }
    const QString loaderId = isNeoForge ? QStringLiteral("neoforge")
                                        : QStringLiteral("forge");
    // The running loader satisfies both loader ids: NeoForge 1.20.1 is a Forge
    // fork, and mods on it still declare a "forge" dependency. Reporting the
    // other name as a missing mod sends the user hunting for something that is
    // not a downloadable mod at all.
    const QString otherLoaderId = isNeoForge ? QStringLiteral("forge")
                                             : QStringLiteral("neoforge");
    QSet<QString> providedIds{ QStringLiteral("minecraft"), loaderId, otherLoaderId };
    QHash<QString, QString> providedVersions{
        { QStringLiteral("minecraft"), minecraftVersion },
        { loaderId, loaderVersion },
        { otherLoaderId, loaderVersion },
    };
    QList<ForgeDependencyRequirement> requirements;
    QTemporaryDir bundledFiles;
    if (!bundledFiles.isValid()) {
        if (error) *error = QObject::tr("Could not prepare bundled dependency inspection.");
        return false;
    }
    int inspectedJars = 0;
    const QDir modsDirectory(QDir(serverRoot).filePath(QStringLiteral("mods")));
    for (const QFileInfo &jar : modsDirectory.entryInfoList(
             QStringList() << QStringLiteral("*.jar"), QDir::Files)) {
        if (!collectForgeBundledMetadata(jar.absoluteFilePath(), loaderType,
                                  bundledFiles.path(), 0, &inspectedJars,
                                  &providedIds, &providedVersions,
                                  &requirements, warnings, failure, error)) {
            return false;
        }
    }

    std::sort(requirements.begin(), requirements.end(),
              [](const auto &left, const auto &right) {
                  if (left.modId != right.modId) return left.modId < right.modId;
                  if (left.dependencyId != right.dependencyId) {
                      return left.dependencyId < right.dependencyId;
                  }
                  return left.kind < right.kind;
              });
    const QString loaderName = isNeoForge ? QObject::tr("NeoForge")
                                          : QObject::tr("Forge");
    // Collected across the whole loop so one repair round can cover every gap.
    QStringList missingDescriptions;
    for (const ForgeDependencyRequirement &requirement : requirements) {
        const bool dependencyPresent = providedIds.contains(requirement.dependencyId);
        if (requirement.kind == ForgeDependencyKind::Required && !dependencyPresent) {
            if (missingDependencyIds
                && !missingDependencyIds->contains(requirement.dependencyId)) {
                missingDependencyIds->append(requirement.dependencyId);
            }
            missingDescriptions.append(
                QObject::tr("Mod \"%1\" (%2) requires \"%3\", but that dependency is missing.")
                    .arg(requirement.modName, requirement.modId,
                         requirement.dependencyId));
            continue;
        }
        if (!dependencyPresent) {
            continue;
        }

        const QString installedVersion = providedVersions.value(
            requirement.dependencyId);
        const MavenRangeResult rangeResult = evaluateNumericMavenRange(
            requirement.versionRange, installedVersion);
        if (rangeResult == MavenRangeResult::Invalid) {
            if (failure) {
                *failure = DependencyValidationFailure::Inconclusive;
            }
            if (error) {
                *error = QObject::tr(
                             "The %1 metadata for mod \"%2\" declares an invalid version range for \"%3\": %4")
                             .arg(loaderName, requirement.modName,
                                  requirement.dependencyId, requirement.versionRange);
            }
            return false;
        }
        if (rangeResult == MavenRangeResult::Unverifiable) {
            if (warnings) {
                const QString warning = QObject::tr(
                    "Could not safely compare installed mod \"%1\" version \"%2\" with Maven range %3; the %4 loader will verify it at startup.")
                    .arg(requirement.dependencyId,
                         installedVersion.isEmpty() ? QObject::tr("unknown")
                                                    : installedVersion,
                         requirement.versionRange, loaderName);
                if (!warnings->contains(warning)) {
                    warnings->append(warning);
                }
            }
            continue;
        }

        const bool versionMatches = rangeResult == MavenRangeResult::Matches;
        if ((requirement.kind == ForgeDependencyKind::Required
             || requirement.kind == ForgeDependencyKind::Optional)
            && !versionMatches) {
            if (!isNeoForge
                && requirement.dependencyId == QStringLiteral("minecraft")
                && isNewerPatchInSameMinecraftLine(
                    requirement.versionRange, installedVersion)) {
                if (warnings) {
                    warnings->append(QObject::tr(
                        "Mod \"%1\" (%2) declares Minecraft %3, while the pack uses "
                        "the newer patch %4 in the same release line; Forge will verify "
                        "this provider-published combination at server startup.")
                        .arg(requirement.modName, requirement.modId,
                             requirement.versionRange, installedVersion));
                }
                continue;
            }
            if (failure) {
                *failure = DependencyValidationFailure::Definite;
            }
            if (error) {
                *error = QObject::tr(
                             "The pack does not contain a compatible %1 server. "
                             "Mod \"%2\" (%3) requires \"%4\" version %5, but installed version %6 does not match.")
                             .arg(loaderName, requirement.modName, requirement.modId,
                                  requirement.dependencyId, requirement.versionRange,
                                  installedVersion);
            }
            return false;
        }
        if (requirement.kind == ForgeDependencyKind::Incompatible
            && versionMatches) {
            if (failure) {
                *failure = DependencyValidationFailure::Definite;
            }
            if (error) {
                *error = QObject::tr(
                             "The pack does not contain a compatible %1 server. "
                             "Mod \"%2\" (%3) is incompatible with installed mod \"%4\" version %5.")
                             .arg(loaderName, requirement.modName, requirement.modId,
                                  requirement.dependencyId, installedVersion);
            }
            return false;
        }
    }
    if (!missingDescriptions.isEmpty()) {
        if (failure) {
            *failure = DependencyValidationFailure::Definite;
        }
        if (error) {
            *error = QObject::tr("The pack does not contain a complete %1 server. %2")
                         .arg(loaderName,
                              missingDescriptions.join(QStringLiteral(" ")));
        }
        return false;
    }
    return true;
}
}  // namespace

ServerDependencyCheckResult ServerModpackInstaller::checkServerDependencies(
    const QString &serverRoot, const QString &loaderType,
    const QString &minecraftVersion, const QString &loaderVersion)
{
    ServerDependencyCheckResult result;
    DependencyValidationFailure failure = DependencyValidationFailure::None;
    const QString loader = loaderType.trimmed().toLower();
    bool valid = true;
    if (loader == QStringLiteral("fabric")) {
        valid = validateFabricDependencyClosure(serverRoot, &failure,
                                                &result.missingDependencyIds,
                                                &result.error);
    } else if (loader == QStringLiteral("forge")
               || loader == QStringLiteral("neoforge")) {
        valid = validateForgeDependencyClosure(
            serverRoot, loader, minecraftVersion, loaderVersion,
            &result.warnings, &failure, &result.missingDependencyIds,
            &result.error);
    }
    if (valid) {
        result.state = ServerDependencyCheckState::Compatible;
    } else if (failure == DependencyValidationFailure::Unsafe) {
        result.state = ServerDependencyCheckState::Unsafe;
    } else if (failure == DependencyValidationFailure::Definite) {
        result.state = ServerDependencyCheckState::DefiniteFailure;
    } else {
        result.state = ServerDependencyCheckState::Inconclusive;
    }
    return result;
}
