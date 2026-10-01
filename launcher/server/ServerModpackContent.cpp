// SPDX-License-Identifier: GPL-3.0-only

#include "ServerModpackContent.h"
#include "ServerPaths.h"

#include "ServerModpackInstaller.h"
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
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <toml++/toml.h>

#include <algorithm>
#include <utility>

namespace ServerModpackContent {

// Paths from the pack are checked with the shared rules.
using ServerPaths::isSafeRelativePath;
using ServerPaths::normalizedRelativePath;


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

bool hasServerRootEvidence(const QString &path)
{
    if (hasServerContentRoot(path)) {
        return true;
    }
    const QDir directory(path);
    const QStringList rootFiles{
        QStringLiteral("server.jar"),
        QStringLiteral("run.bat"), QStringLiteral("run.sh"),
        QStringLiteral("start.bat"), QStringLiteral("start.sh"),
        QStringLiteral("server.properties"),
        QStringLiteral("default-server.properties"),
    };
    for (const QString &name : rootFiles) {
        if (QFileInfo(directory.filePath(name)).isFile()) {
            return true;
        }
    }
    return false;
}

QStringList publishedServerPackRootChoicesInternal(const QString &instanceRoot)
{
    const QString extractedRoot = QDir(instanceRoot).filePath(
        QStringLiteral("server-pack/server-files"));
    if (!QFileInfo(extractedRoot).isDir()) {
        return {};
    }
    if (hasServerRootEvidence(extractedRoot)) {
        return { QStringLiteral(".") };
    }

    const QDir base(extractedRoot);
    QList<QPair<int, QString>> candidates;
    QDirIterator iterator(extractedRoot, QDir::Dirs | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (!hasServerRootEvidence(path)) {
            continue;
        }
        const QString relative = normalizedRelativePath(base.relativeFilePath(path));
        candidates.append({ relative.count(QLatin1Char('/')), relative });
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto &left, const auto &right) {
                  if (left.first != right.first) return left.first < right.first;
                  return left.second.compare(right.second, Qt::CaseInsensitive) < 0;
              });
    QStringList choices;
    if (!candidates.isEmpty()) {
        const int shallowest = candidates.constFirst().first;
        for (const auto &candidate : std::as_const(candidates)) {
            if (candidate.first != shallowest) break;
            choices.append(candidate.second);
        }
    }
    return choices;
}

QString publishedServerPackRoot(const QString &instanceRoot,
                                const QString &selectedRoot, QString *error)
{
    const QString extractedRoot = QDir(instanceRoot).filePath(
        QStringLiteral("server-pack/server-files"));
    if (!QFileInfo(extractedRoot).isDir()) {
        if (error) {
            *error = QObject::tr(
                "The published server pack was not extracted.");
        }
        return {};
    }
    const QStringList choices = publishedServerPackRootChoicesInternal(instanceRoot);
    if (choices.isEmpty()) {
        if (error) {
            *error = QObject::tr(
                "The published server pack contains no usable server files.");
        }
        return {};
    }
    QString choice = normalizedRelativePath(selectedRoot);
    if (selectedRoot.trimmed().isEmpty()) {
        if (choices.size() > 1) {
            if (error) {
                *error = QObject::tr(
                    "The published server pack has more than one possible content root. Choose one and retry.");
            }
            return {};
        }
        choice = choices.constFirst();
    }
    if (!choices.contains(choice, Qt::CaseInsensitive)) {
        if (error) {
            *error = QObject::tr("The selected server-pack root is not available: %1")
                         .arg(selectedRoot);
        }
        return {};
    }
    return choice == QStringLiteral(".")
        ? extractedRoot : QDir(extractedRoot).filePath(choice);
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
    // Adapter-provided values describe the required setup for this newly
    // created server. They must replace blank or incorrect pack defaults.
    for (auto iterator = overrides.cbegin(); iterator != overrides.cend(); ++iterator) {
        properties.insert(iterator.key(), iterator.value());
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
                       QStringList *warnings = nullptr,
                       QStringList *missingRequiredFiles = nullptr)
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
            if (missingRequiredFiles && !missingRequiredFiles->contains(path)) {
                missingRequiredFiles->append(path);
            }
            if (warnings) {
                const QString warning = QObject::tr(
                    "Required server file is missing: %1. Add it before starting the server.")
                                            .arg(path);
                if (!warnings->contains(warning)) warnings->append(warning);
            }
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
                                QStringList *skippedClientFiles,
                                const QSet<QString> &knownClientOnlyHashes)
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
            && (jarDeclaresClientOnly(jar.absoluteFilePath())
                || knownClientOnlyHashes.contains(Hashing::hash(
                       jar.absoluteFilePath(), Hashing::Algorithm::Sha256).toLower()))) {
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
                             const QString &destination,
                             QStringList *missingRequiredFiles,
                             QStringList *warnings, QString *error)
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
            if (missingRequiredFiles && !missingRequiredFiles->contains(relativePath)) {
                missingRequiredFiles->append(relativePath);
            }
            if (warnings) {
                const QString warning = QObject::tr(
                    "Required server file is missing: %1. Add it before starting the server.")
                                            .arg(relativePath);
                if (!warnings->contains(warning)) warnings->append(warning);
            }
            continue;
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

bool writeRequiredFilesManifest(const QString &destination,
                                QStringList missingPaths, QString *error)
{
    if (missingPaths.isEmpty()) return true;
    missingPaths.removeDuplicates();
    missingPaths.sort(Qt::CaseInsensitive);
    QSaveFile file(QDir(destination).filePath(
        QStringLiteral("jlauncher_required_server_files.txt")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QObject::tr("Could not save the missing server-file requirements.");
        return false;
    }
    QByteArray contents;
    for (const QString &path : std::as_const(missingPaths)) {
        contents += path.toUtf8() + '\n';
    }
    if (file.write(contents) != contents.size() || !file.commit()) {
        file.cancelWriting();
        if (error) *error = QObject::tr("Could not save the missing server-file requirements.");
        return false;
    }
    return true;
}

bool writeDerivedServerMarker(const QString &destination, QString *error)
{
    QSaveFile file(QDir(destination).filePath(
        QStringLiteral("jlauncher_derived_server.txt")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)
        || file.write("Dependencies are checked before startup.\n") < 0
        || !file.commit()) {
        file.cancelWriting();
        if (error) *error = QObject::tr("Could not save the derived-server readiness marker.");
        return false;
    }
    return true;
}

bool hasUsablePreparedContent(const QString &destination)
{
    QDirIterator iterator(destination, QDir::Files | QDir::NoSymLinks,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString name = QFileInfo(iterator.next()).fileName();
        if (name != QStringLiteral("jlauncher_required_server_files.txt")
            && name != QStringLiteral("jlauncher_derived_server.txt")) {
            return true;
        }
    }
    return false;
}

}  // namespace ServerModpackContent

using namespace ServerModpackContent;

bool ServerModpackInstaller::prepareContent(const QString &instanceRoot,
                                            const QString &gameRoot,
                                            const QString &destination,
                                            QStringList *skippedClientFiles,
                                            QString *error, QStringList *warnings,
                                            const QString &publishedServerRoot,
                                            QStringList *missingRequiredFiles,
                                            const QStringList &knownClientOnlyHashes,
                                            bool knownClientOnlyHashesProvided)
{
    if (missingRequiredFiles) missingRequiredFiles->clear();
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
        const QString serverPackRoot = publishedServerPackRoot(
            instanceRoot, publishedServerRoot, error);
        return !serverPackRoot.isEmpty()
            && copyDirectoryContents(serverPackRoot, destination, error);
    }

    QStringList missingFiles;
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
                           skippedClientFiles, error, warnings,
                           &missingFiles)) {
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
    const QStringList effectiveKnownClientOnlyHashes = knownClientOnlyHashesProvided
        ? knownClientOnlyHashes : ServerModpackInstaller::knownClientOnlyHashes();
    QSet<QString> knownClientOnlyHashSet;
    for (const QString &hash : effectiveKnownClientOnlyHashes) {
        knownClientOnlyHashSet.insert(hash.toLower());
    }
    readDeclaredClientOnlyMods(gameRoot, &excludedPaths, skippedClientFiles,
                               knownClientOnlyHashSet);
    readServerPairClientOnlyFiles(instanceRoot, &excludedPaths,
                                  skippedClientFiles);

    if (!includedPaths.isEmpty()) {
        includedPaths.unite(supplementalServerPaths);
        const bool copied = copyIncludedServerFiles(
            instanceRoot, gameRoot, includedPaths, excludedPaths, destination,
            &missingFiles, warnings, error);
        if (copied && skippedClientFiles) {
            skippedClientFiles->removeDuplicates();
            skippedClientFiles->sort(Qt::CaseInsensitive);
        }
        if (missingRequiredFiles) *missingRequiredFiles = missingFiles;
        return copied && writeRequiredFilesManifest(destination, missingFiles, error);
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
                                    destination, &missingFiles,
                                    warnings, error)) {
        return false;
    }
    if (skippedClientFiles) {
        skippedClientFiles->removeDuplicates();
        skippedClientFiles->sort(Qt::CaseInsensitive);
    }
    if (missingRequiredFiles) *missingRequiredFiles = missingFiles;
    return writeRequiredFilesManifest(destination, missingFiles, error);
}
