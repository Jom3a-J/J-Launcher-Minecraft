#include "ServerModpackInstaller.h"

#include "server/ServerInstance.h"
#include "server/ServerManager.h"
#include "server/ServerMemory.h"
#include "server/ServerPackCompatibility.h"
#include "server/ServerProperties.h"
#include "HardwareInfo.h"

#include "archive/ArchiveReader.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/MetadataHandler.h"
#include "logs/Privacy.h"
#include "modplatform/ModIndex.h"
#include "modplatform/helpers/HashUtils.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>

#include <toml++/toml.h>

#include <algorithm>
#include <utility>

QString serverModpackFailureCategoryName(ServerModpackFailureCategory category)
{
    switch (category) {
        case ServerModpackFailureCategory::None:
            return QObject::tr("None");
        case ServerModpackFailureCategory::InvalidProfile:
            return QObject::tr("Invalid modpack profile");
        case ServerModpackFailureCategory::CompatibilityMetadata:
            return QObject::tr("Provider compatibility metadata");
        case ServerModpackFailureCategory::ContentProjection:
            return QObject::tr("Server content projection");
        case ServerModpackFailureCategory::DependencyIncompatibility:
            return QObject::tr("Mod dependency incompatibility");
        case ServerModpackFailureCategory::LauncherInternal:
            return QObject::tr("Launcher failure");
    }
    return QObject::tr("Unknown");
}

QString serverModpackFailureStageName(ServerModpackFailureStage stage)
{
    switch (stage) {
        case ServerModpackFailureStage::None:
            return QObject::tr("None");
        case ServerModpackFailureStage::ProfileInspection:
            return QObject::tr("Profile inspection");
        case ServerModpackFailureStage::CompatibilityCheck:
            return QObject::tr("Compatibility check");
        case ServerModpackFailureStage::ContentPreparation:
            return QObject::tr("Server content preparation");
        case ServerModpackFailureStage::DependencyValidation:
            return QObject::tr("Dependency validation");
        case ServerModpackFailureStage::ServerCreation:
            return QObject::tr("Server creation");
        case ServerModpackFailureStage::ContentInstallation:
            return QObject::tr("Server content installation");
    }
    return QObject::tr("Unknown");
}

namespace {
void recordServerModpackFailure(ServerModpackInstallResult *result,
                                ServerModpackFailureCategory category,
                                ServerModpackFailureStage stage,
                                QString error)
{
    result->failureCategory = category;
    result->failureStage = stage;
    result->error = std::move(error);
    qWarning().noquote()
        << "Modpack server creation failed. Category:"
        << serverModpackFailureCategoryName(category)
        << "Stage:" << serverModpackFailureStageName(stage)
        << "Reason:" << Privacy::sanitizeText(result->error);
}

QString normalizedRelativePath(QString path)
{
    path = QDir::fromNativeSeparators(QDir::cleanPath(path.trimmed()));
    while (path.startsWith("./")) {
        path.remove(0, 2);
    }
    return path;
}

bool isSafeRelativePath(const QString &path)
{
    const QString normalized = normalizedRelativePath(path);
    return !normalized.isEmpty() && normalized != ".." && !normalized.startsWith("../")
        && !QDir::isAbsolutePath(normalized);
}

const QStringList &serverContentRoots()
{
    static const QStringList roots{
        QStringLiteral("mods"),
        QStringLiteral("config"),
        QStringLiteral("configureddefaults"),
        QStringLiteral("datapacks"),
        QStringLiteral("defaultconfigs"),
        QStringLiteral("ftbteambases"),
        QStringLiteral("global_data_packs"),
        QStringLiteral("global_packs"),
        QStringLiteral("kubejs"),
        QStringLiteral("openloader"),
        QStringLiteral("patchouli_books"),
        QStringLiteral("resources"),
        QStringLiteral("scripts"),
        QStringLiteral("structures"),
    };
    return roots;
}

bool hasServerContentRoot(const QString &path)
{
    const QDir directory(path);
    for (const QString &root : serverContentRoots()) {
        if (QFileInfo(directory.filePath(root)).isDir()) {
            return true;
        }
    }
    return false;
}

QString publishedServerPackRoot(const QString &instanceRoot, QString *error)
{
    const QString extractedRoot = QDir(instanceRoot).filePath(
        QStringLiteral("server-pack/server-files"));
    if (!QFileInfo(extractedRoot).isDir()) {
        if (error) {
            *error = QObject::tr("The published server pack was not extracted.");
        }
        return {};
    }
    if (hasServerContentRoot(extractedRoot)) {
        return extractedRoot;
    }

    const QDir base(extractedRoot);
    QList<QPair<int, QString>> candidates;
    QDirIterator iterator(extractedRoot, QDir::Dirs | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (!hasServerContentRoot(path)) {
            continue;
        }
        const QString relative = normalizedRelativePath(base.relativeFilePath(path));
        candidates.append({ relative.count(QLatin1Char('/')), path });
    }

    if (candidates.isEmpty()) {
        if (error) {
            *error = QObject::tr(
                "The published server pack contains no supported server content folders.");
        }
        return {};
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto &left, const auto &right) {
                  return left.first < right.first;
              });
    if (candidates.size() > 1 && candidates.at(0).first == candidates.at(1).first) {
        if (error) {
            *error = QObject::tr(
                "The published server pack has more than one possible content root.");
        }
        return {};
    }
    return candidates.constFirst().second;
}

bool copyFile(const QString &source, const QString &destination, QString *error)
{
    if (!QDir().mkpath(QFileInfo(destination).dir().absolutePath())) {
        if (error) {
            *error = QObject::tr("Could not create the destination folder for %1.")
                         .arg(QFileInfo(destination).fileName());
        }
        return false;
    }
    if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
        if (error) {
            *error = QObject::tr("Could not replace %1.").arg(QFileInfo(destination).fileName());
        }
        return false;
    }
    if (!QFile::copy(source, destination)) {
        if (error) {
            *error = QObject::tr("Could not copy %1.").arg(QFileInfo(source).fileName());
        }
        return false;
    }
    return true;
}

bool copyDirectory(const QString &sourceRoot, const QString &destinationRoot,
                   const QSet<QString> &excludedPaths, QString *error)
{
    const QDir source(sourceRoot);
    if (!source.exists()) {
        return true;
    }
    QDirIterator iterator(sourceRoot,
                          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo entry = iterator.fileInfo();
        const QString relative = normalizedRelativePath(source.relativeFilePath(entry.absoluteFilePath()));
        const QString serverRelative = normalizedRelativePath(
            QFileInfo(sourceRoot).fileName() + QLatin1Char('/') + relative);
        if (entry.isSymLink()) {
            if (error) {
                *error = QObject::tr("The modpack contains an unsafe symbolic link: %1.")
                             .arg(serverRelative);
            }
            return false;
        }
        if (excludedPaths.contains(serverRelative.toLower())
            || serverRelative.compare(QStringLiteral("mods/.index"), Qt::CaseInsensitive) == 0
            || serverRelative.startsWith(QStringLiteral("mods/.index/"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString destination = QDir(destinationRoot).filePath(serverRelative);
        if (entry.isDir()) {
            if (!QDir().mkpath(destination)) {
                if (error) {
                    *error = QObject::tr("Could not create the server folder %1.").arg(serverRelative);
                }
                return false;
            }
        } else if (entry.isFile() && !copyFile(entry.absoluteFilePath(), destination, error)) {
            return false;
        }
    }
    return true;
}

bool copyDirectoryContents(const QString &sourceRoot, const QString &destinationRoot,
                           QString *error)
{
    const QDir source(sourceRoot);
    if (!source.exists()) {
        return true;
    }
    QDirIterator iterator(sourceRoot,
                          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                              | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo entry = iterator.fileInfo();
        const QString relative = normalizedRelativePath(
            source.relativeFilePath(entry.absoluteFilePath()));
        if (!isSafeRelativePath(relative) || entry.isSymLink()) {
            if (error) {
                *error = QObject::tr("The server pack contains an unsafe path: %1.")
                             .arg(relative);
            }
            return false;
        }
        const QString destinationRelative =
            relative.compare(QStringLiteral("default-server.properties"),
                             Qt::CaseInsensitive) == 0
            ? QStringLiteral("server.properties")
            : relative;
        const QString destination = QDir(destinationRoot).filePath(destinationRelative);
        if (entry.isDir()) {
            if (!QDir().mkpath(destination)) {
                if (error) {
                    *error = QObject::tr("Could not create the server folder %1.")
                                 .arg(relative);
                }
                return false;
            }
        } else if (entry.isFile()
                   && !copyFile(entry.absoluteFilePath(), destination, error)) {
            return false;
        }
    }
    return true;
}

bool copyServerRootFiles(const QString &sourceRoot, const QString &destinationRoot,
                         QString *error)
{
    const QDir source(sourceRoot);
    const QDir destination(destinationRoot);

    QString propertiesSource = source.filePath(QStringLiteral("server.properties"));
    if (!QFileInfo(propertiesSource).isFile()) {
        propertiesSource = source.filePath(QStringLiteral("default-server.properties"));
    }
    if (QFileInfo(propertiesSource).isFile()
        && !copyFile(propertiesSource,
                     destination.filePath(QStringLiteral("server.properties")), error)) {
        return false;
    }

    const QString iconSource = source.filePath(QStringLiteral("server-icon.png"));
    return !QFileInfo(iconSource).isFile()
        || copyFile(iconSource,
                    destination.filePath(QStringLiteral("server-icon.png")), error);
}

bool applyServerPropertyOverrides(const QString &instanceRoot,
                                  const std::shared_ptr<ServerInstance> &server,
                                  QString *error)
{
    const QString requirements = QDir(instanceRoot).filePath(
        QStringLiteral("server-pack/server-setup-required.txt"));
    if (QFileInfo(requirements).exists()
        && (QFileInfo(requirements).isSymLink()
            || !copyFile(requirements,
                         QDir(server->serverDirectory()).filePath("server-setup-required.txt"), error))) {
        if (error && error->isEmpty()) {
            *error = QObject::tr("The server setup manifest is an unsafe symbolic link.");
        }
        return false;
    }
    const QString overridePath = QDir(instanceRoot).filePath(
        QStringLiteral("server-pack/server-properties.txt"));
    if (!QFileInfo(overridePath).isFile()) {
        return true;
    }

    QString loadError;
    const QMap<QString, QString> overrides =
        ServerProperties::load(overridePath, &loadError);
    if (!loadError.isEmpty()) {
        if (error) {
            *error = loadError;
        }
        return false;
    }
    QMap<QString, QString> properties =
        ServerProperties::load(server->serverPropertiesPath(), &loadError);
    if (!loadError.isEmpty()) {
        if (error) {
            *error = loadError;
        }
        return false;
    }
    for (auto iterator = overrides.cbegin(); iterator != overrides.cend(); ++iterator) {
        if (!properties.contains(iterator.key())) {
            properties.insert(iterator.key(), iterator.value());
        }
    }
    properties.insert(QStringLiteral("server-port"),
                      QString::number(server->port()));
    return ServerProperties::save(server->serverPropertiesPath(), properties,
                                  error);
}

bool readPathList(const QString &path, QSet<QString> *paths, QString *error,
                QStringList *warnings = nullptr)
{
    QFile file(path);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // Optional compatibility lists: ignore with a warning and fall back
        // to conservative local content. Unsafe paths below still block.
        const QString warning = QObject::tr(
            "Could not read the server compatibility manifest %1; it was ignored.")
                                      .arg(QFileInfo(path).fileName());
        if (warnings && !warnings->contains(warning)) {
            warnings->append(warning);
        } else {
            qWarning().noquote() << warning;
        }
        return true;
    }
    while (!file.atEnd()) {
        const QString entry = normalizedRelativePath(
            QString::fromUtf8(file.readLine()));
        if (entry.isEmpty()) {
            continue;
        }
        if (!isSafeRelativePath(entry)) {
            if (error) {
                *error = QObject::tr(
                             "The server compatibility manifest contains an unsafe path: %1")
                             .arg(entry);
            }
            return false;
        }
        paths->insert(entry);
    }
    return true;
}

bool readModrinthRules(const QString &instanceRoot, const QString &gameRoot,
                       QSet<QString> *includedPaths, QSet<QString> *excludedPaths,
                       QStringList *skippedClientFiles, QString *error,
                       QStringList *warnings = nullptr)
{
    const QString mrpackRoot = QDir(instanceRoot).filePath(QStringLiteral("mrpack"));
    QFile indexFile(QDir(mrpackRoot).filePath(QStringLiteral("modrinth.index.json")));
    if (!indexFile.exists()) {
        return true;
    }
    if (!indexFile.open(QIODevice::ReadOnly)) {
        // Optional provider index: ignore with a warning when the installed
        // profile remains usable. Compatibility already reports this.
        const QString warning = QObject::tr(
            "Could not read the installed Modrinth pack index; it was ignored and local content will be used.");
        if (warnings && !warnings->contains(warning)) {
            warnings->append(warning);
        }
        return true;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(indexFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        const QString warning = QObject::tr(
            "The installed Modrinth pack index is malformed; it was ignored and local content will be used conservatively.");
        if (warnings && !warnings->contains(warning)) {
            warnings->append(warning);
        }
        return true;
    }

    const QJsonValue filesValue = document.object().value(QStringLiteral("files"));
    if (!filesValue.isArray()) {
        const QString warning = QObject::tr(
            "The installed Modrinth pack has no valid file list; server content will be projected conservatively from local files.");
        if (warnings && !warnings->contains(warning)) {
            warnings->append(warning);
        }
        return true;
    }
    for (const QJsonValue &value : filesValue.toArray()) {
        if (!value.isObject()) {
            const QString warning = QObject::tr(
                "The installed Modrinth pack contains an invalid file entry; it was skipped.");
            if (warnings && !warnings->contains(warning)) {
                warnings->append(warning);
            }
            continue;
        }
        const QJsonObject file = value.toObject();
        const QString rawPath = file.value(QStringLiteral("path")).toString();
        const QString path = normalizedRelativePath(rawPath);
        if (path.isEmpty()) {
            const QString warning = QObject::tr(
                "The installed Modrinth pack contains a file entry without a path; it was skipped.");
            if (warnings && !warnings->contains(warning)) {
                warnings->append(warning);
            }
            continue;
        }
        if (!isSafeRelativePath(path)) {
            if (error) {
                *error = QObject::tr("The installed Modrinth pack contains an unsafe path: %1")
                             .arg(path);
            }
            return false;
        }
        const QString serverSupport = file.value(QStringLiteral("env")).toObject()
                                           .value(QStringLiteral("server"))
                                           .toString();
        if (serverSupport == QStringLiteral("unsupported")) {
            // Side conflicts are advisory and order-independent: retain the
            // most server-capable label (include wins). Compatibility already
            // warns about the conflict.
            bool alreadyIncluded = false;
            for (const QString &included : std::as_const(*includedPaths)) {
                if (included.compare(path, Qt::CaseInsensitive) == 0) {
                    alreadyIncluded = true;
                    break;
                }
            }
            if (alreadyIncluded) {
                continue;
            }
            excludedPaths->insert(path.toLower());
            if (skippedClientFiles) {
                skippedClientFiles->append(path);
            }
            continue;
        }
        if (excludedPaths->remove(path.toLower()) && skippedClientFiles) {
            skippedClientFiles->erase(
                std::remove_if(skippedClientFiles->begin(), skippedClientFiles->end(),
                               [&path](const QString &skipped) {
                                   return skipped.compare(path, Qt::CaseInsensitive) == 0;
                               }),
                skippedClientFiles->end());
        }
        includedPaths->insert(path);
        if (serverSupport == QStringLiteral("required")
            && !QFileInfo::exists(QDir(gameRoot).filePath(path))) {
            const QString cachedPath =
                QDir(mrpackRoot).filePath(QStringLiteral("server-files/") + path);
            if (QFileInfo::exists(cachedPath)) {
                continue;
            }
            if (error) {
                *error = QObject::tr(
                             "This pack has a required server-only file that the client download "
                             "does not contain: %1")
                             .arg(path);
            }
            return false;
        }
    }

    if (!readPathList(QDir(mrpackRoot).filePath(QStringLiteral("overrides.txt")),
                      includedPaths, error, warnings)) {
        return false;
    }

    QFile clientOverrides(QDir(mrpackRoot).filePath(QStringLiteral("client-overrides.txt")));
    if (clientOverrides.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!clientOverrides.atEnd()) {
            const QString path = normalizedRelativePath(
                QString::fromUtf8(clientOverrides.readLine()));
            if (!isSafeRelativePath(path)) {
                continue;
            }
            excludedPaths->insert(path.toLower());
            if (skippedClientFiles) {
                skippedClientFiles->append(path);
            }
        }
    }
    return true;
}

bool jarDeclaresClientOnly(const QString &path)
{
    MMCZip::ArchiveReader archive(path);
    if (const auto fabricMetadata = archive.goToFile(QStringLiteral("fabric.mod.json"))) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            fabricMetadata->readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            const QString environment = document.object()
                                            .value(QStringLiteral("environment"))
                                            .toString()
                                            .trimmed();
            if (environment.compare(QStringLiteral("client"), Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
    }

    // Older Forge metadata has an explicit clientSideOnly flag. Modern
    // displayTest values describe network-version checks, not physical side,
    // so they must not be treated as proof that a mod is client-only.
    for (const QString &metadataPath : {
             QStringLiteral("META-INF/mods.toml"),
             QStringLiteral("META-INF/neoforge.mods.toml") }) {
        MMCZip::ArchiveReader forgeArchive(path);
        if (const auto forgeMetadata = forgeArchive.goToFile(metadataPath)) {
            const QString contents = QString::fromUtf8(forgeMetadata->readAll());
            static const QRegularExpression clientOnlyExpression(
                QStringLiteral(R"((?im)^\s*clientSideOnly\s*=\s*true\s*(?:#.*)?$)"));
            if (clientOnlyExpression.match(contents).hasMatch()) {
                return true;
            }
        }
    }
    return false;
}

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
        metadataFile->readAll(), &parseError);
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
        const QByteArray contents = nestedFile->readAll();
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
    for (const auto &requirement : requirements) {
        if (providedIds.contains(requirement.dependencyId)) {
            continue;
        }
        if (failure) {
            *failure = DependencyValidationFailure::Definite;
        }
        if (error) {
            *error = QObject::tr(
                         "The pack does not contain a complete Fabric server. "
                         "Mod \"%1\" (%2) requires \"%3\", but that dependency is missing.")
                         .arg(requirement.modName, requirement.modId,
                              requirement.dependencyId);
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
    const QStringList lines = QString::fromUtf8(manifest->readAll()).split(
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
            contents = metadata->readAll();
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

    MMCZip::ArchiveReader archive(jarPath);
    const auto metadata = archive.goToFile("META-INF/jarjar/metadata.json");
    if (!metadata) return true;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(metadata->readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !document.object().value("jars").isArray()) {
        if (error) *error = QObject::tr("Invalid bundled dependency metadata in %1.").arg(QFileInfo(jarPath).fileName());
        return false;
    }
    for (const auto &value : document.object().value("jars").toArray()) {
        const QString path = value.toObject().value("path").toString();
        if (!isSafeRelativePath(path) || !path.endsWith(".jar", Qt::CaseInsensitive)) {
            if (failure) *failure = DependencyValidationFailure::Unsafe;
            if (error) *error = QObject::tr("Unsafe bundled dependency path in %1.").arg(QFileInfo(jarPath).fileName());
            return false;
        }
        MMCZip::ArchiveReader nestedArchive(jarPath);
        const auto entry = nestedArchive.goToFile(path);
        if (!entry || !entry->isFile()) {
            if (failure) *failure = DependencyValidationFailure::Definite;
            if (error) *error = QObject::tr("Bundled dependency %1 is missing from %2.").arg(path, QFileInfo(jarPath).fileName());
            return false;
        }
        // Use a generated destination, never a path supplied by the archive.
        const QString extracted = QDir(temporaryRoot).filePath(QString::number(*count) + ".jar");
        const QByteArray bytes = entry->readAll();
        QFile file(extracted);
        if (bytes.isEmpty() || !file.open(QIODevice::WriteOnly)
            || file.write(bytes) != bytes.size()) {
            if (error) *error = QObject::tr("Could not inspect bundled dependency %1.").arg(path);
            return false;
        }
        file.close();
        if (!collectForgeBundledMetadata(extracted, loaderType, temporaryRoot,
                                         depth + 1, count, providedIds, providedVersions,
                                         requirements, warnings, failure, error)) return false;
    }
    return true;
}

bool validateForgeDependencyClosure(const QString &serverRoot,
                                    const QString &loaderType,
                                    const QString &minecraftVersion,
                                    const QString &loaderVersion,
                                    QStringList *warnings,
                                    DependencyValidationFailure *failure,
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
    QSet<QString> providedIds{ QStringLiteral("minecraft"), loaderId };
    QHash<QString, QString> providedVersions{
        { QStringLiteral("minecraft"), minecraftVersion },
        { loaderId, loaderVersion },
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
    for (const ForgeDependencyRequirement &requirement : requirements) {
        const bool dependencyPresent = providedIds.contains(requirement.dependencyId);
        if (requirement.kind == ForgeDependencyKind::Required && !dependencyPresent) {
            if (failure) {
                *failure = DependencyValidationFailure::Definite;
            }
            if (error) {
                *error = QObject::tr(
                             "The pack does not contain a complete %1 server. "
                             "Mod \"%2\" (%3) requires \"%4\", but that dependency is missing.")
                             .arg(loaderName, requirement.modName, requirement.modId,
                                  requirement.dependencyId);
            }
            return false;
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
    return true;
}

void excludeClientOnlyMod(const QString &filename, QSet<QString> *excludedPaths,
                          QStringList *skippedClientFiles)
{
    if (filename.isEmpty()) return;
    const QString relativePath = normalizedRelativePath(
        QStringLiteral("mods/") + filename);
    excludedPaths->insert(relativePath.toLower());
    if (skippedClientFiles) {
        skippedClientFiles->append(relativePath);
    }
}

void readDeclaredClientOnlyMods(const QString &gameRoot, QSet<QString> *excludedPaths,
                                QStringList *skippedClientFiles)
{
    // New downloads use mods/.index. Prism-compatible legacy instances use
    // jarmods. Both describe files installed in the same minecraft/mods folder.
    const QList<QDir> metadataDirectories{
        QDir(QDir(gameRoot).filePath(QStringLiteral("mods/.index"))),
        QDir(QDir(gameRoot).filePath(QStringLiteral("jarmods"))),
    };
    for (const QDir &indexDirectory : metadataDirectories) {
        for (const QString &entry : indexDirectory.entryList(
                 QStringList() << QStringLiteral("*.pw.toml"), QDir::Files)) {
            const auto metadata = Metadata::get(indexDirectory, entry);
            if (!metadata.isValid()
                || metadata.side != ModPlatform::SideType::ClientSide) {
                continue;
            }
            excludeClientOnlyMod(metadata.filename, excludedPaths,
                                 skippedClientFiles);
        }
    }

    const QDir modsDirectory(QDir(gameRoot).filePath(QStringLiteral("mods")));
    for (const QFileInfo &jar : modsDirectory.entryInfoList(
             QStringList() << QStringLiteral("*.jar"), QDir::Files)) {
        const QString relativePath = normalizedRelativePath(
            QStringLiteral("mods/") + jar.fileName());
        if (!excludedPaths->contains(relativePath.toLower())
            && jarDeclaresClientOnly(jar.absoluteFilePath())) {
            excludeClientOnlyMod(jar.fileName(), excludedPaths,
                                 skippedClientFiles);
        }
    }
}

void readServerPairClientOnlyFiles(const QString &instanceRoot,
                                   QSet<QString> *excludedPaths,
                                   QStringList *skippedClientFiles)
{
    QFile file(QDir(instanceRoot).filePath(
        QStringLiteral("server-pack/client-only.txt")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    while (!file.atEnd()) {
        const QString path =
            normalizedRelativePath(QString::fromUtf8(file.readLine()));
        if (!isSafeRelativePath(path)) {
            continue;
        }
        excludedPaths->insert(path.toLower());
        if (skippedClientFiles) {
            skippedClientFiles->append(path);
        }
    }
}

bool copyIncludedServerFiles(const QString &instanceRoot, const QString &gameRoot,
                             const QSet<QString> &includedPaths,
                             const QSet<QString> &excludedPaths,
                             const QString &destination, QString *error)
{
    QStringList sortedPaths(includedPaths.cbegin(), includedPaths.cend());
    sortedPaths.sort(Qt::CaseInsensitive);
    for (const QString &relativePath : sortedPaths) {
        if (excludedPaths.contains(relativePath.toLower())) {
            continue;
        }
        const QStringList candidates{
            QDir(instanceRoot).filePath(
                QStringLiteral("server-pack/server-files/") + relativePath),
            QDir(instanceRoot).filePath(
                QStringLiteral("mrpack/server-files/") + relativePath),
            QDir(gameRoot).filePath(relativePath),
        };
        QString source;
        for (const QString &candidate : candidates) {
            const QFileInfo info(candidate);
            if (info.isSymLink()) {
                if (error) {
                    *error = QObject::tr("The modpack contains an unsafe symbolic link: %1.")
                                 .arg(relativePath);
                }
                return false;
            }
            if (info.isFile()) {
                source = candidate;
                break;
            }
        }
        if (source.isEmpty()) {
            if (error) {
                *error = QObject::tr(
                             "A provider-declared server file is missing after download: %1")
                             .arg(relativePath);
            }
            return false;
        }
        const QString destinationPath =
            relativePath.compare(QStringLiteral("default-server.properties"),
                                 Qt::CaseInsensitive) == 0
            ? QStringLiteral("server.properties")
            : relativePath;
        if (!copyFile(source, QDir(destination).filePath(destinationPath), error)) {
            return false;
        }
    }
    return true;
}

void importContentTracking(const QString &gameRoot,
                           const std::shared_ptr<ServerInstance> &server)
{
    if (!server) return;
    QSettings settings;
    const QString sourcePrefix = QStringLiteral("ServerContentSources/%1/").arg(server->id());
    const QFileInfoList installedFiles = QDir(server->modsDirectory()).entryInfoList(
        QStringList() << QStringLiteral("*.jar"), QDir::Files);
    for (const QFileInfo &installed : installedFiles) {
        const QString source = ServerModpackInstaller::contentTrackingSource(
            gameRoot, installed.absoluteFilePath());
        if (!source.isEmpty()) {
            settings.setValue(sourcePrefix + installed.fileName(), source);
        }
    }
}
}

ServerModpackProfile ServerModpackInstaller::profileForVersions(
    const QString &minecraftVersion, const QString &fabricVersion,
    const QString &forgeVersion, const QString &neoForgeVersion,
    const QString &quiltVersion)
{
    ServerModpackProfile result;
    result.minecraftVersion = minecraftVersion.trimmed();
    if (result.minecraftVersion.isEmpty()) {
        result.error = QObject::tr("The modpack does not declare a Minecraft version.");
        return result;
    }

    const QList<QPair<QString, QString>> loaders{
        { QStringLiteral("fabric"), fabricVersion.trimmed() },
        { QStringLiteral("forge"), forgeVersion.trimmed() },
        { QStringLiteral("neoforge"), neoForgeVersion.trimmed() },
    };
    for (const auto &loader : loaders) {
        if (loader.second.isEmpty()) {
            continue;
        }
        if (!result.loaderType.isEmpty()) {
            result.error = QObject::tr("The modpack declares more than one server loader.");
            return result;
        }
        result.loaderType = loader.first;
        result.loaderVersion = loader.second;
    }
    if (!quiltVersion.trimmed().isEmpty()) {
        result.error = QObject::tr(
            "Quilt modpack servers are not supported by the current server runtime.");
        return result;
    }
    if (result.loaderType.isEmpty()) {
        result.error = QObject::tr(
            "The selected pack is not a supported Fabric, Forge, or NeoForge modpack.");
    }
    return result;
}

QString ServerModpackInstaller::contentTrackingSource(
    const QString &gameRoot, const QString &installedFilePath)
{
    const QFileInfo installed(installedFilePath);
    if (!installed.isFile()) return {};

    const QList<QDir> metadataDirectories{
        QDir(QDir(gameRoot).filePath(QStringLiteral("mods/.index"))),
        QDir(QDir(gameRoot).filePath(QStringLiteral("jarmods"))),
    };
    for (const QDir &indexDirectory : metadataDirectories) {
        for (const QString &entry : indexDirectory.entryList(
                 QStringList() << QStringLiteral("*.pw.toml"), QDir::Files)) {
            const auto metadata = Metadata::get(indexDirectory, entry);
            if (!metadata.isValid()
                || metadata.filename.compare(installed.fileName(), Qt::CaseInsensitive) != 0) {
                continue;
            }

            bool matches = false;
            const Hashing::Algorithm algorithm =
                Hashing::algorithmFromString(metadata.hash_format.toLower());
            if (!metadata.hash.isEmpty() && algorithm != Hashing::Algorithm::Unknown) {
                const QString actualHash = Hashing::hash(installed.absoluteFilePath(), algorithm);
                matches = !actualHash.isEmpty()
                    && actualHash.compare(metadata.hash, Qt::CaseInsensitive) == 0;
            } else {
                const QFileInfo sourceFile(
                    QDir(gameRoot).filePath(QStringLiteral("mods/") + metadata.filename));
                if (sourceFile.isFile() && sourceFile.size() == installed.size()) {
                    const QString installedHash = Hashing::hash(
                        installed.absoluteFilePath(), Hashing::Algorithm::Sha1);
                    const QString sourceHash = Hashing::hash(
                        sourceFile.absoluteFilePath(), Hashing::Algorithm::Sha1);
                    matches = !installedHash.isEmpty() && installedHash == sourceHash;
                }
            }
            if (!matches) continue;

            const QString provider =
                metadata.provider == ModPlatform::ResourceProvider::MODRINTH
                ? QStringLiteral("modrinth") : QStringLiteral("curseforge");
            return QStringLiteral("%1:%2:%3")
                .arg(provider, metadata.project_id.toString(),
                     metadata.file_id.toString());
        }
    }
    return {};
}

ServerModpackProfile ServerModpackInstaller::inspect(const MinecraftInstance &instance)
{
    const auto *profile = instance.getPackProfile();
    const auto resolvedProfile = profileForVersions(
        profile->getComponentVersion(QStringLiteral("net.minecraft")),
        profile->getComponentVersion(QStringLiteral("net.fabricmc.fabric-loader")),
        profile->getComponentVersion(QStringLiteral("net.minecraftforge")),
        profile->getComponentVersion(QStringLiteral("net.neoforged")),
        profile->getComponentVersion(QStringLiteral("org.quiltmc.quilt-loader")));
    if (resolvedProfile.isValid()) {
        return resolvedProfile;
    }

    // Provider installation can finish before the remote metadata resolver has
    // refreshed every component. The local component document is already
    // transactional and contains the exact versions selected by the provider,
    // so it is the reliable fallback for server pairing.
    const auto savedProfile = profileFromInstanceRoot(instance.instanceRoot());
    return savedProfile.isValid() ? savedProfile : resolvedProfile;
}

ServerModpackProfile ServerModpackInstaller::profileFromInstanceRoot(
    const QString &instanceRoot)
{
    QFile file(QDir(instanceRoot).filePath(QStringLiteral("mmc-pack.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        ServerModpackProfile result;
        result.error = QObject::tr("The installed modpack component file could not be read.");
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        ServerModpackProfile result;
        result.error = QObject::tr("The installed modpack component file is invalid.");
        return result;
    }

    QHash<QString, QString> versions;
    const QJsonArray components =
        document.object().value(QStringLiteral("components")).toArray();
    for (const QJsonValue &value : components) {
        const QJsonObject component = value.toObject();
        const QString uid = component.value(QStringLiteral("uid")).toString();
        QString version = component.value(QStringLiteral("version")).toString();
        if (version.isEmpty()) {
            version = component.value(QStringLiteral("cachedVersion")).toString();
        }
        if (!uid.isEmpty() && !version.isEmpty()) {
            versions.insert(uid, version);
        }
    }

    return profileForVersions(
        versions.value(QStringLiteral("net.minecraft")),
        versions.value(QStringLiteral("net.fabricmc.fabric-loader")),
        versions.value(QStringLiteral("net.minecraftforge")),
        versions.value(QStringLiteral("net.neoforged")),
        versions.value(QStringLiteral("org.quiltmc.quilt-loader")));
}

bool ServerModpackInstaller::prepareContent(const QString &instanceRoot,
                                            const QString &gameRoot,
                                            const QString &destination,
                                            QStringList *skippedClientFiles,
                                            QString *error, QStringList *warnings)
{
    if (!QFileInfo(gameRoot).isDir()) {
        if (error) {
            *error = QObject::tr("The downloaded instance has no game directory.");
        }
        return false;
    }
    const auto compatibility = inspectServerPack(instanceRoot);
    if (compatibility.isIncompatible()) {
        if (error) {
            *error = serverPackCompatibilityDescription(compatibility);
        }
        return false;
    }
    if (!QDir().mkpath(destination)) {
        if (error) {
            *error = QObject::tr("Could not create temporary server content.");
        }
        return false;
    }

    if (compatibility.hasDedicatedServerPack) {
        const QString serverPackRoot = publishedServerPackRoot(instanceRoot, error);
        return !serverPackRoot.isEmpty()
            && copyDirectoryContents(serverPackRoot, destination, error);
    }

    QSet<QString> includedPaths;
    QSet<QString> supplementalServerPaths;
    QSet<QString> excludedPaths;
    for (const auto &file : compatibility.files) {
        if (file.side == ServerPackFileSide::ClientOnly) {
            excludedPaths.insert(file.path.toLower());
            if (skippedClientFiles) {
                skippedClientFiles->append(file.path);
            }
        } else if (file.side == ServerPackFileSide::ServerOnly) {
            supplementalServerPaths.insert(file.path);
        } else if (file.side == ServerPackFileSide::Universal
                   || file.side == ServerPackFileSide::Unknown) {
            includedPaths.insert(file.path);
        }
    }
    if (!readModrinthRules(instanceRoot, gameRoot, &includedPaths, &excludedPaths,
                           skippedClientFiles, error, warnings)) {
        return false;
    }
    if (!readPathList(
            QDir(instanceRoot).filePath(QStringLiteral("server-pack/include.txt")),
            &includedPaths, error, warnings)) {
        return false;
    }
    if (!readPathList(
            QDir(instanceRoot).filePath(QStringLiteral("flame/overrides.txt")),
            &includedPaths, error, warnings)) {
        return false;
    }
    readDeclaredClientOnlyMods(gameRoot, &excludedPaths, skippedClientFiles);
    readServerPairClientOnlyFiles(instanceRoot, &excludedPaths,
                                  skippedClientFiles);

    if (!includedPaths.isEmpty()) {
        includedPaths.unite(supplementalServerPaths);
        const bool copied = copyIncludedServerFiles(
            instanceRoot, gameRoot, includedPaths, excludedPaths, destination, error);
        if (copied && skippedClientFiles) {
            skippedClientFiles->removeDuplicates();
            skippedClientFiles->sort(Qt::CaseInsensitive);
        }
        return copied;
    }

    for (const QString &root : serverContentRoots()) {
        if (!copyDirectory(QDir(gameRoot).filePath(root), destination,
                           excludedPaths, error)) {
            return false;
        }
        if (!copyDirectory(
                QDir(instanceRoot).filePath(
                    QStringLiteral("mrpack/server-files/") + root),
                destination, excludedPaths, error)) {
            return false;
        }
        if (!copyDirectory(
                QDir(instanceRoot).filePath(
                    QStringLiteral("server-pack/server-files/") + root),
                destination, excludedPaths, error)) {
            return false;
        }
    }
    if (!copyServerRootFiles(gameRoot, destination, error)
        || !copyServerRootFiles(
            QDir(instanceRoot).filePath(QStringLiteral("mrpack/server-files")),
            destination, error)
        || !copyServerRootFiles(
            QDir(instanceRoot).filePath(QStringLiteral("server-pack/server-files")),
            destination, error)) {
        return false;
    }
    if (!supplementalServerPaths.isEmpty()
        && !copyIncludedServerFiles(instanceRoot, gameRoot,
                                    supplementalServerPaths, excludedPaths,
                                    destination, error)) {
        return false;
    }
    if (skippedClientFiles) {
        skippedClientFiles->removeDuplicates();
        skippedClientFiles->sort(Qt::CaseInsensitive);
    }
    return true;
}

ServerModpackInstallResult ServerModpackInstaller::createMatchingServer(
    ServerManager *manager, const MinecraftInstance &instance,
    const QString &serverName)
{
    // Trust an explicit pack recommendation only when the source instance
    // truly overrides memory or carries an exported recommendation. The
    // global default must not count. Never parse startup scripts or command
    // text for memory flags.
    int providerRecommendation = 0;
    if (auto *settings = const_cast<MinecraftInstance &>(instance).settings()) {
        providerRecommendation = ServerMemory::resolveProviderRecommendation(
            settings->get("OverrideMemory").toBool(),
            settings->get("MaxMemAlloc").toInt(),
            settings->get("ExportRecommendedRAM").toInt());
    }
    return createMatchingServer(manager, inspect(instance), instance.instanceRoot(),
                                instance.gameRoot(),
                                serverName.trimmed().isEmpty()
                                    ? instance.name() + QObject::tr(" Server")
                                    : serverName,
                                providerRecommendation, 0);
}

ServerModpackInstallResult ServerModpackInstaller::createMatchingServer(
    ServerManager *manager, const ServerModpackProfile &profile,
    const QString &instanceRoot, const QString &gameRoot,
    const QString &serverName, int providerRecommendationMiB,
    quint64 totalRamMiB)
{
    ServerModpackInstallResult result;
    if (!manager) {
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::LauncherInternal,
            ServerModpackFailureStage::ServerCreation,
            QObject::tr("The server manager is not available."));
        return result;
    }
    if (!profile.isValid()) {
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::InvalidProfile,
            ServerModpackFailureStage::ProfileInspection, profile.error);
        return result;
    }

    const auto compatibility = evaluateServerPack(
        instanceRoot, profile.minecraftVersion, profile.loaderType,
        profile.loaderVersion);
    result.provider = compatibility.provider;
    result.hasDedicatedServerPack = compatibility.hasDedicatedServerPack;
    if (compatibility.isIncompatible()) {
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::CompatibilityMetadata,
            ServerModpackFailureStage::CompatibilityCheck,
            serverPackCompatibilityDescription(compatibility));
        return result;
    }

    const bool hasPublishedServerPack = compatibility.hasDedicatedServerPack;
    if (!hasPublishedServerPack) {
        result.warnings.append(
            compatibility.sideMetadataPresent
                ? QObject::tr("The provider did not publish a dedicated server pack. "
                              "The server was derived from the client pack using the "
                              "available compatibility metadata.")
                : QObject::tr("The provider did not publish a dedicated server pack and "
                              "did not supply complete compatibility metadata. "
                              "The server projection is unverified and may need manual review."));
    }
    if (compatibility.state == ServerPackCompatibilityState::Unknown) {
        result.warnings.append(QObject::tr(
            "Server compatibility is unknown; client-only content could not be proven safe "
            "for a dedicated server."));
    }
    // Surface advisory provider diagnostics without string matching: the
    // installed profile already won, these only explain what was ignored.
    // Integrity is aggregated by count below to avoid one warning per file.
    // Published packs are authoritative byte-identical content; their missing
    // hashes are expected and stay silent (the loader validates at startup).
    for (const ServerPackIssue &issue : compatibility.issues) {
        if (issue.severity != ServerPackIssueSeverity::Advisory) {
            continue;
        }
        if (issue.kind == ServerPackIssueKind::UnverifiedIntegrity) {
            continue;
        }
        if (!result.warnings.contains(issue.message)) {
            result.warnings.append(issue.message);
        }
    }
    if (!hasPublishedServerPack && compatibility.unverifiedFileCount > 0) {
        const QString integritySummary = QObject::tr(
            "%1 server file(s) have no supported provider hash and remain unverified.")
                                             .arg(compatibility.unverifiedFileCount);
        if (!result.warnings.contains(integritySummary)) {
            result.warnings.append(integritySummary);
        }
    }
    for (const QString &warning : compatibility.projectionWarnings) {
        if (!result.warnings.contains(warning)) {
            result.warnings.append(warning);
        }
    }
    if (compatibility.hasAnyAdvisory() && compatibility.providerMetadataPresent) {
        const QString installedSummary = QObject::tr(
            "Using installed Minecraft %1 with %2 %3; provider values that disagreed were ignored. "
            "The server was created stopped and may still fail to start if third-party content is broken.")
                                             .arg(profile.minecraftVersion, profile.loaderType,
                                                  profile.loaderVersion);
        if (!result.warnings.contains(installedSummary)) {
            result.warnings.append(installedSummary);
        }
    }

    QTemporaryDir staging;
    QString preparationError;
    if (!staging.isValid()
        || !prepareContent(instanceRoot, gameRoot, staging.path(),
                           &result.skippedClientFiles, &preparationError,
                           &result.warnings)) {
        if (preparationError.isEmpty()) {
            preparationError = QObject::tr("Could not prepare the modpack for the server.");
        }
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::ContentProjection,
            ServerModpackFailureStage::ContentPreparation,
            preparationError);
        return result;
    }
    bool dependencyClosureValid = true;
    DependencyValidationFailure dependencyFailure =
        DependencyValidationFailure::None;
    QString dependencyError;
    if (profile.loaderType == QStringLiteral("fabric")) {
        dependencyClosureValid = validateFabricDependencyClosure(
            staging.path(), &dependencyFailure, &dependencyError);
    } else if (profile.loaderType == QStringLiteral("forge")
               || profile.loaderType == QStringLiteral("neoforge")) {
        dependencyClosureValid = validateForgeDependencyClosure(
            staging.path(), profile.loaderType, profile.minecraftVersion,
            profile.loaderVersion, &result.warnings, &dependencyFailure,
            &dependencyError);
    }
    QString dependencyWarning;
    if (!dependencyClosureValid) {
        const bool unsafeDependency =
            dependencyFailure == DependencyValidationFailure::Unsafe;
        if (hasPublishedServerPack && !unsafeDependency) {
            // Published packs are authoritative; the loader validates at startup.
            dependencyWarning = QObject::tr(
                "The launcher could not confirm all dependencies in the published server pack. "
                "The server was created; the %1 loader will check them at startup.\n\nDetails: %2")
                                    .arg(profile.loaderType, dependencyError);
        } else if (!hasPublishedServerPack
                   && dependencyFailure == DependencyValidationFailure::Inconclusive) {
            // Inconclusive parser/range findings stay advisory for derived
            // servers. Do not promise startup for broken third-party content.
            dependencyWarning = QObject::tr(
                "The launcher could not fully verify dependencies in the derived server copy. "
                "The server was created stopped, but it may fail to start; the %1 loader will check them at startup.\n\nDetails: %2")
                                    .arg(profile.loaderType, dependencyError);
        } else {
            if (!hasPublishedServerPack) {
                dependencyError = QObject::tr(
                                      "No dedicated server version was supplied by %1, and the "
                                      "derived server copy is incomplete.\n\n%2")
                                      .arg(compatibility.provider, dependencyError);
            }
            recordServerModpackFailure(
                &result, ServerModpackFailureCategory::DependencyIncompatibility,
                ServerModpackFailureStage::DependencyValidation,
                dependencyError);
            return result;
        }
    }

    const auto server = manager->createServer(
        serverName.trimmed(),
        profile.minecraftVersion, profile.loaderType, profile.loaderVersion);
    if (!server) {
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::LauncherInternal,
            ServerModpackFailureStage::ServerCreation,
            QObject::tr("Could not create the matching server."));
        return result;
    }

    QString installationError;
    if (!copyDirectoryContents(staging.path(), server->serverDirectory(),
                               &installationError)) {
        manager->deleteServer(server->id());
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::ContentProjection,
            ServerModpackFailureStage::ContentInstallation,
            installationError);
        return result;
    }
    if (!applyServerPropertyOverrides(instanceRoot, server,
                                      &installationError)) {
        manager->deleteServer(server->id());
        recordServerModpackFailure(
            &result, ServerModpackFailureCategory::ContentProjection,
            ServerModpackFailureStage::ContentInstallation,
            installationError);
        return result;
    }
    importContentTracking(gameRoot, server);
    // Size memory only now that the prepared server content is known, and
    // only for this newly created server. Existing servers are never
    // touched. Persist through the manager so save/reload keeps the values.
    {
        const quint64 hostRamMiB =
            totalRamMiB > 0 ? totalRamMiB : HardwareInfo::totalRamMiB();
        const int deployedJars =
            ServerMemory::countDeployedServerJars(server->serverDirectory(),
                                                  profile.loaderType);
        const ServerMemoryRecommendation memory = ServerMemory::recommend(
            profile.loaderType, deployedJars, hostRamMiB,
            providerRecommendationMiB);
        server->setMinMemory(memory.minMemoryMiB);
        server->setMaxMemory(memory.maxMemoryMiB);
        if (!manager->save()) {
            const QString saveError =
                QObject::tr("Could not save the new server's memory settings.");
            manager->deleteServer(server->id());
            recordServerModpackFailure(
                &result, ServerModpackFailureCategory::LauncherInternal,
                ServerModpackFailureStage::ServerCreation, saveError);
            return result;
        }
    }
    const QString setupIssue = ServerProperties::worldSetupIssue(server->serverDirectory());
    if (!setupIssue.isEmpty()) {
        result.warnings.append(setupIssue);
    }
    if (!dependencyWarning.isEmpty()) {
        result.warnings.append(dependencyWarning);
    }
    result.serverId = server->id();
    return result;
}
