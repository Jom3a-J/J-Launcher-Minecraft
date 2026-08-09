#include "ModrinthInstanceCreationTask.h"

#include "Application.h"
#include "FileSystem.h"
#include "InstanceList.h"
#include "Json.h"

#include "QObjectPtr.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"

#include "minecraft/mod/Mod.h"
#include "modplatform/EnsureMetadataTask.h"
#include "modplatform/helpers/OverrideUtils.h"

#include "net/ChecksumValidator.h"

#include "net/ApiDownload.h"
#include "net/NetJob.h"
#include "logs/Privacy.h"

#include "modplatform/ModIndex.h"
#include "settings/INISettingsObject.h"

#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/UntrustedModsDialog.h"
#include "ui/pages/modplatform/OptionalModDialog.h"

#include <QAbstractButton>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <functional>
#include <memory>
#include <vector>

bool ModrinthCreationTask::abort()
{
    if (!canAbort()) {
        return false;
    }

    if (m_task) {
        m_task->abort();
    }
    return InstanceTask::abort();
}

void ModrinthCreationTask::executeTask()
{
    auto* instanceList = APPLICATION->instances();

    // FIXME: How to handle situations when there's more than one install already for a given modpack?
    BaseInstance* inst = nullptr;
    if (auto originalId = originalInstanceID(); !originalId.isEmpty()) {
        inst = instanceList->getInstanceById(originalId);
        Q_ASSERT(inst);
    } else {
        inst = instanceList->getInstanceByManagedName(originalName());

        if (!inst) {
            inst = instanceList->getInstanceById(originalName());

            if (!inst) {
                createInstance();
                return;
            }
        }
    }

    QString indexPath = FS::PathCombine(m_stagingPath, "modrinth.index.json");
    if (!parseManifest(indexPath, m_files, true, false)) {
        return;
    }

    auto versionName = inst->getManagedPackVersionName();
    m_rootPath = QFileInfo(inst->gameRoot()).fileName();
    auto versionStr = !versionName.isEmpty() ? tr(" (version %1)").arg(versionName) : "";

    if (shouldConfirmUpdate()) {
        auto shouldUpdate = askIfShouldUpdate(m_parent, versionStr);
        if (shouldUpdate == ShouldUpdate::SkipUpdating) {
            createInstance();
            return;
        }
        if (shouldUpdate == ShouldUpdate::Cancel) {
            emitAborted();
            return;
        }
    }

    // Remove repeated files, we don't need to download them!
    QDir oldInstDir(inst->instanceRoot());

    QString oldIndexFolder(FS::PathCombine(oldInstDir.absolutePath(), "mrpack"));

    QString oldIndexPath(FS::PathCombine(oldIndexFolder, "modrinth.index.json"));
    QFileInfo oldIndexFile(oldIndexPath);
    if (oldIndexFile.exists()) {
        std::vector<File> oldFiles;
        parseManifest(oldIndexPath, oldFiles, false, false);

        // Let's remove all duplicated, identical resources!
        auto filesIterator = m_files.begin();
    begin:
        while (filesIterator != m_files.end()) {
            const auto& file = *filesIterator;

            auto oldFilesIterator = oldFiles.begin();
            while (oldFilesIterator != oldFiles.end()) {
                const auto& oldFile = *oldFilesIterator;

                if (oldFile.hash == file.hash) {
                    qDebug() << "Removed file at" << Privacy::sanitizePath(file.path)
                             << "from list of downloads";
                    filesIterator = m_files.erase(filesIterator);
                    oldFilesIterator = oldFiles.erase(oldFilesIterator);
                    goto begin;  // Sorry :c
                }

                oldFilesIterator++;
            }

            filesIterator++;
        }

        QDir oldMinecraftDir(inst->gameRoot());

        // Some files were removed from the old version, and some will be downloaded in an updated version,
        // so we're fine removing them!
        if (!oldFiles.empty()) {
            for (const auto& file : oldFiles) {
                scheduleToDelete(m_parent, oldMinecraftDir, file.path, true);
            }
        }

        // We will remove all the previous overrides, to prevent duplicate files!
        // TODO: Currently 'overrides' will always override the stuff on update. How do we preserve unchanged overrides?
        // FIXME: We may want to do something about disabled mods.
        auto oldOverrides = Override::readOverrides("overrides", oldIndexFolder);
        for (const auto& entry : oldOverrides) {
            scheduleToDelete(m_parent, oldMinecraftDir, entry);
        }

        auto oldClientOverrides = Override::readOverrides("client-overrides", oldIndexFolder);
        for (const auto& entry : oldClientOverrides) {
            scheduleToDelete(m_parent, oldMinecraftDir, entry);
        }
    } else {
        // We don't have an old index file, so we may duplicate stuff!
        auto* dialog = CustomMessageBox::selectable(m_parent, tr("No index file."),
                                                    tr("We couldn't find a suitable index file for the older version. This may cause some "
                                                       "of the files to be duplicated. Do you want to continue?"),
                                                    QMessageBox::Warning, QMessageBox::Ok | QMessageBox::Cancel);

        if (dialog->exec() == QDialog::DialogCode::Rejected) {
            emitAborted();
            return;
        }
    }

    setOverride(true, inst->id());
    qDebug() << "Will override instance!";

    m_oldInstance = inst;

    // We let it go through the createInstance() stage, just with a couple modifications for updating
    createInstance();
}

// https://docs.modrinth.com/docs/modpacks/format_definition/
void ModrinthCreationTask::createInstance()
{
    QString parentFolder(FS::PathCombine(m_stagingPath, "mrpack"));

    QString indexPath = FS::PathCombine(m_stagingPath, "modrinth.index.json");
    if (m_files.empty() && !parseManifest(indexPath, m_files, true, true)) {
        return;
    }

    // Keep index file in case we need it some other time (like when changing versions)
    QString newIndexPlace(FS::PathCombine(parentFolder, "modrinth.index.json"));
    FS::ensureFilePathExists(newIndexPlace);
    FS::move(indexPath, newIndexPlace);
    if (shouldCreateServerPair()) {
        QFile providerMarker(FS::PathCombine(parentFolder, "provider.txt"));
        if (!providerMarker.open(QIODevice::WriteOnly | QIODevice::Text)
            || providerMarker.write("modrinth\n") != 9) {
            emitFailed(tr("Could not record the Modrinth compatibility metadata."));
            return;
        }
    }

    auto mcPath = FS::PathCombine(m_stagingPath, m_rootPath);

    auto overridePath = FS::PathCombine(m_stagingPath, "overrides");
    if (QFile::exists(overridePath)) {
        // Create a list of overrides in "overrides.txt" inside mrpack/
        Override::createOverrides("overrides", parentFolder, overridePath);

        // Apply the overrides
        if (!FS::move(overridePath, mcPath)) {
            emitFailed(tr("Could not rename the overrides folder:\n") + "overrides");
            return;
        }
    }

    // Do client overrides
    auto clientOverridePath = FS::PathCombine(m_stagingPath, "client-overrides");
    if (QFile::exists(clientOverridePath)) {
        // Create a list of overrides in "client-overrides.txt" inside mrpack/
        Override::createOverrides("client-overrides", parentFolder, clientOverridePath);

        // Apply the overrides
        if (!FS::overrideFolder(mcPath, clientOverridePath)) {
            emitFailed(tr("Could not rename the client overrides folder:\n") + "client overrides");
            return;
        }
    }

    if (!promptForUntrustedMods()) {
        emitAborted();
        return;
    }

    QString configPath = FS::PathCombine(m_stagingPath, "instance.cfg");
    auto instanceSettings = std::make_unique<INISettingsObject>(configPath);
    m_newInstance = std::make_unique<MinecraftInstance>(m_globalSettings, std::move(instanceSettings), m_stagingPath);

    auto* components = m_newInstance->getPackProfile();
    components->buildingFromScratch();
    components->setComponentVersion("net.minecraft", m_minecraftVersion, true);

    QString loader;
    if (!m_fabricVersion.isEmpty()) {
        components->setComponentVersion("net.fabricmc.fabric-loader", m_fabricVersion);
        loader = ModPlatform::getModLoaderAsString(ModPlatform::ModLoaderType::Fabric);
    }
    if (!m_quiltVersion.isEmpty()) {
        components->setComponentVersion("org.quiltmc.quilt-loader", m_quiltVersion);
        loader = ModPlatform::getModLoaderAsString(ModPlatform::ModLoaderType::Quilt);
    }
    if (!m_forgeVersion.isEmpty()) {
        components->setComponentVersion("net.minecraftforge", m_forgeVersion);
        loader = ModPlatform::getModLoaderAsString(ModPlatform::ModLoaderType::Forge);
    }
    if (!m_neoForgeVersion.isEmpty()) {
        components->setComponentVersion("net.neoforged", m_neoForgeVersion);
        loader = ModPlatform::getModLoaderAsString(ModPlatform::ModLoaderType::NeoForge);
    }

    if (m_instIcon != "default") {
        m_newInstance->setIconKey(m_instIcon);
    } else if (!m_managedId.isEmpty()) {
        m_newInstance->setIconKey("modrinth");
    }

    setManagedPack(m_newInstance.get());

    m_newInstance->setName(name());
    m_newInstance->saveNow();

    auto downloadMods = makeShared<NetJob>(tr("Mod Download Modrinth"), APPLICATION->network());

    auto rootModpackPath = FS::PathCombine(m_stagingPath, m_rootPath);
    auto rootModpackUrl = QUrl::fromLocalFile(rootModpackPath);
    // TODO make this work with other sorts of resource
    for (auto& file : m_files) {
        auto fileName = file.path;
        fileName = FS::RemoveInvalidPathChars(fileName);
        auto filePath = FS::PathCombine(rootModpackPath, fileName);
        if (!rootModpackUrl.isParentOf(QUrl::fromLocalFile(filePath))) {
            // This means we somehow got out of the root folder, so abort here to prevent exploits
            emitFailed(
                tr("One of the files has a path that leads to an arbitrary location (%1). This is a security risk and isn't allowed.")
                    .arg(fileName));
            return;
        }
        if (fileName.startsWith("mods/")) {
            auto* mod = new Mod(filePath);
            ModDetails d;
            d.mod_id = filePath;
            mod->setDetails(d);
            m_resources.insert(file.hash.toHex(), mod);
        }
        if (file.downloads.empty()) {
            emitFailed(tr("The file '%1' is missing a download link. This is invalid in the pack format.").arg(fileName));
            return;
        }
        qDebug() << "Will try to download"
                 << Privacy::sanitizeUrl(file.downloads.front())
                 << "to" << Privacy::sanitizePath(filePath);

        Net::ModrinthDownloadMeta meta{ .reason = m_oldInstance.has_value() ? "update" : "modpack",
                                        .gameVersion = m_minecraftVersion,
                                        .loader = loader,
                                        .dependentOn = !m_managedId.isEmpty() ? m_managedVersionId : "" };

        QUrl downloadUrl = file.downloads.dequeue();
        auto dl = Net::ApiDownload::makeFile(downloadUrl, filePath, Net::Download::Option::NoOptions, meta);
        dl->addValidator(new Net::ChecksumValidator(file.hashAlgorithm, file.hash));
        downloadMods->addNetAction(dl);
        if (!file.downloads.empty()) {
            // FIXME: This really needs to be put into a ConcurrentTask of
            // MultipleOptionsTask's , once those exist :)
            auto param = dl.toWeakRef();
            connect(dl.get(), &Task::failed, dl.get(), [&file, filePath, param, downloadMods, meta] {
                QUrl fallbackUrl = file.downloads.dequeue();
                auto ndl = Net::ApiDownload::makeFile(fallbackUrl, filePath, Net::Download::Option::NoOptions, meta);
                ndl->addValidator(new Net::ChecksumValidator(file.hashAlgorithm, file.hash));
                downloadMods->addNetAction(ndl);
                if (auto shared = param.lock()) {
                    shared->succeeded();
                }
            });
        }
    }

    if (shouldCreateServerPair()
        && !addServerOnlyDownloads(newIndexPlace,
                                   FS::PathCombine(parentFolder, "server-files"),
                                   downloadMods.get())) {
        return;
    }

    connect(downloadMods.get(), &NetJob::succeeded, this, &ModrinthCreationTask::ensureMetaLoop);
    connect(downloadMods.get(), &NetJob::failed, this, &ModrinthCreationTask::emitFailed);
    connect(downloadMods.get(), &NetJob::aborted, this, &ModrinthCreationTask::emitAborted);
    connect(downloadMods.get(), &NetJob::progress, this, [this](qint64 current, qint64 total) {
        setDetails(tr("%1 out of %2 complete").arg(current).arg(total));
        setProgress(current, total);
    });
    connect(downloadMods.get(), &NetJob::stepProgress, this, &ModrinthCreationTask::propagateStepProgress);

    setStatus(tr("Downloading mods..."));
    downloadMods->start();
    m_task = downloadMods;
}

bool ModrinthCreationTask::addServerOnlyDownloads(const QString& indexPath,
                                                  const QString& cacheRoot,
                                                  NetJob* downloads)
{
    QJsonDocument document;
    try {
        document = Json::requireDocument(indexPath, "modrinth.index.json");
    } catch (const JSONValidationError& e) {
        emitFailed(tr("Could not read the Modrinth server file manifest:\n%1").arg(e.cause()));
        return false;
    }

    if (!downloads || !document.isObject()) {
        emitFailed(tr("The Modrinth server file manifest is malformed."));
        return false;
    }

    const QJsonArray files = document.object().value(QStringLiteral("files")).toArray();
    for (const QJsonValue& value : files) {
        if (!value.isObject()) {
            emitFailed(tr("The Modrinth server file manifest contains an invalid entry."));
            return false;
        }
        const QJsonObject file = value.toObject();
        const QJsonValue envValue = file.value(QStringLiteral("env"));
        const QString filePath = file.value(QStringLiteral("path")).toString();
        if (!envValue.isUndefined() && !envValue.isObject()) {
            emitFailed(tr("The Modrinth environment metadata for %1 is malformed.").arg(filePath));
            return false;
        }
        const QJsonObject environment = envValue.toObject();
        QString clientSupport = QStringLiteral("required");
        QString serverSupport = QStringLiteral("required");
        const auto readSupport = [this, &environment, &filePath](const QString& key,
                                                                  QString& support) {
            if (!environment.contains(key)) {
                return true;
            }
            const QJsonValue value = environment.value(key);
            if (!value.isString()) {
                emitFailed(tr("The Modrinth environment value for %1 is malformed.").arg(filePath));
                return false;
            }
            support = value.toString();
            if (support != QStringLiteral("required")
                && support != QStringLiteral("optional")
                && support != QStringLiteral("unsupported")) {
                emitFailed(tr("The Modrinth environment value for %1 is unsupported.").arg(filePath));
                return false;
            }
            return true;
        };
        if (!readSupport(QStringLiteral("client"), clientSupport)
            || !readSupport(QStringLiteral("server"), serverSupport)) {
            return false;
        }
        if (clientSupport == QStringLiteral("required")) {
            continue;
        }
        if (serverSupport == QStringLiteral("unsupported")) {
            continue;
        }

        const QString relativePath = QDir::cleanPath(filePath).replace('\\', '/');
        if (relativePath.isEmpty() || relativePath == QStringLiteral("..")
            || relativePath.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relativePath)) {
            emitFailed(tr("The Modrinth server manifest contains an unsafe path: %1").arg(relativePath));
            return false;
        }

        const QJsonValue downloadsValue = file.value(QStringLiteral("downloads"));
        if (!downloadsValue.isArray()) {
            emitFailed(tr("The Modrinth server file %1 has invalid download metadata.").arg(relativePath));
            return false;
        }
        const QJsonArray downloadValues = downloadsValue.toArray();
        QList<QUrl> downloadUrls;
        for (const QJsonValue& downloadValue : downloadValues) {
            if (!downloadValue.isString()) {
                emitFailed(tr("The Modrinth server file %1 has an invalid download URL.").arg(relativePath));
                return false;
            }
            const QString urlText = downloadValue.toString().trimmed();
            const QUrl url(urlText);
            if (urlText.isEmpty() || !url.isValid()
                || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
                || url.host().isEmpty()) {
                emitFailed(tr("The Modrinth server file %1 has an invalid HTTPS download URL.").arg(relativePath));
                return false;
            }
            downloadUrls.append(url);
        }
        const QString sha512 = file.value(QStringLiteral("hashes")).toObject().value(QStringLiteral("sha512")).toString().trimmed();
        if (downloadUrls.isEmpty() || sha512.size() != 128
            || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]{128}$")).match(sha512).hasMatch()) {
            emitFailed(tr("The required Modrinth server file %1 has incomplete download or checksum metadata.").arg(relativePath));
            return false;
        }

        const QString destination = FS::PathCombine(cacheRoot, relativePath);
        FS::ensureFilePathExists(destination);
        const QByteArray hash = QByteArray::fromHex(sha512.toLatin1());
        auto enqueueDownload = [downloads, destination, hash,
                                urls = std::move(downloadUrls)]() mutable {
            struct DownloadFallbackState {
                QList<QUrl> remaining;
                std::function<void()> enqueue;
            };

            auto state = std::make_shared<DownloadFallbackState>();
            state->remaining = std::move(urls);
            const std::weak_ptr<DownloadFallbackState> weakState = state;
            state->enqueue = [downloads, destination, hash, weakState]() {
                auto state = weakState.lock();
                if (!state || state->remaining.isEmpty()) {
                    return;
                }

                auto download = Net::ApiDownload::makeFile(state->remaining.takeFirst(), destination);
                download->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha512, hash));
                if (!state->remaining.isEmpty()) {
                    const auto previous = download.toWeakRef();
                    QObject::connect(download.get(), &Task::failed, download.get(),
                                     [state, previous] {
                                         state->enqueue();
                                         if (auto shared = previous.lock()) {
                                             shared->succeeded();
                                         }
                                     });
                }
                downloads->addNetAction(download);
            };
            state->enqueue();
        };
        enqueueDownload();
    }
    return true;
}

bool ModrinthCreationTask::parseManifest(const QString& indexPath, std::vector<File>& files, bool setInternalData, bool showOptionalDialog)
{
    try {
        auto doc = Json::requireDocument(indexPath);
        auto obj = Json::requireObject(doc, "modrinth.index.json");
        int formatVersion = Json::requireInteger(obj, "formatVersion", "modrinth.index.json");
        if (formatVersion == 1) {
            auto game = Json::requireString(obj, "game", "modrinth.index.json");
            if (game != "minecraft") {
                throw JSONValidationError("Unknown game: " + game);
            }

            if (setInternalData) {
                if (m_managedVersionId.isEmpty()) {
                    m_managedVersionId = obj.value("versionId").toString();
                }
                m_managedName = obj.value("name").toString();
            }

            auto jsonFiles = Json::requireIsArrayOf<QJsonObject>(obj, "files", "modrinth.index.json");
            std::vector<File> optionalFiles;
            for (const auto& modInfo : jsonFiles) {
                File file;
                file.path = Json::requireString(modInfo, "path").replace("\\", "/");

                auto envValue = modInfo["env"];
                // 'env' field is optional. Explicit values must be valid so malformed
                // provider metadata cannot silently change the projection.
                if (!envValue.isUndefined() && !envValue.isObject()) {
                    throw JSONValidationError("'env' must be an object");
                }
                auto env = envValue.toObject();
                if (!env.isEmpty()) {
                    const auto readSupport = [&env](const QString& key, const QString& fallback) {
                        if (!env.contains(key)) {
                            return fallback;
                        }
                        const auto value = env.value(key);
                        if (!value.isString()) {
                            throw JSONValidationError("'env." + key + "' must be a string");
                        }
                        const auto support = value.toString();
                        if (support != "required" && support != "optional" && support != "unsupported") {
                            throw JSONValidationError("Unsupported Modrinth environment value: " + support);
                        }
                        return support;
                    };
                    QString support = readSupport("client", "required");
                    if (support == "unsupported") {
                        continue;
                    }
                    if (support == "optional") {
                        file.required = false;
                    }
                }

                QJsonObject hashes = Json::requireObject(modInfo, "hashes");
                file.hash = QByteArray::fromHex(Json::requireString(hashes, "sha512").toLatin1());
                file.hashAlgorithm = QCryptographicHash::Sha512;

                // Do not use requireUrl, which uses StrictMode, instead use QUrl's default TolerantMode
                // (as Modrinth seems to incorrectly handle spaces)

                auto downloadArr = modInfo["downloads"].toArray();
                for (auto download : downloadArr) {
                    qWarning() << Privacy::sanitizeUrl(download.toString());
                    bool isLast = download.toString() == downloadArr.last().toString();

                    auto downloadUrl = QUrl(download.toString());

                    if (!downloadUrl.isValid()) {
                        qDebug()
                            << QString("Download URL (%1) for %2 is not a correctly formatted URL").arg(downloadUrl.toString(), file.path);
                        if (isLast && file.downloads.isEmpty()) {
                            throw JSONValidationError(tr("Download URL for %1 is not a correctly formatted URL").arg(file.path));
                        }
                    } else {
                        file.downloads.push_back(downloadUrl);
                    }
                }

                (file.required ? files : optionalFiles).push_back(file);
            }

            if (!optionalFiles.empty()) {
                if (showOptionalDialog) {
                    QStringList oFiles;
                    for (const auto& file : optionalFiles) {
                        oFiles.push_back(file.path);
                    }
                    OptionalModDialog optionalModDialog(m_parent, oFiles);
                    if (optionalModDialog.exec() == QDialog::Rejected) {
                        emitAborted();
                        return false;
                    }

                    auto selectedMods = optionalModDialog.getResult();
                    for (auto file : optionalFiles) {
                        if (selectedMods.contains(file.path)) {
                            file.required = true;
                        } else {
                            file.path += ".disabled";
                        }
                        files.push_back(file);
                    }
                } else {
                    for (auto file : optionalFiles) {
                        file.path += ".disabled";
                        files.push_back(file);
                    }
                }
            }
            if (setInternalData) {
                auto dependencies = Json::requireObject(obj, "dependencies", "modrinth.index.json");
                for (auto it = dependencies.begin(), end = dependencies.end(); it != end; ++it) {
                    QString name = it.key();
                    if (name == "minecraft") {
                        m_minecraftVersion = Json::requireString(*it, "Minecraft version");
                    } else if (name == "fabric-loader") {
                        m_fabricVersion = Json::requireString(*it, "Fabric Loader version");
                    } else if (name == "quilt-loader") {
                        m_quiltVersion = Json::requireString(*it, "Quilt Loader version");
                    } else if (name == "forge") {
                        m_forgeVersion = Json::requireString(*it, "Forge version");
                    } else if (name == "neoforge") {
                        m_neoForgeVersion = Json::requireString(*it, "NeoForge version");
                    } else {
                        throw JSONValidationError("Unknown dependency type: " + name);
                    }
                }
            }
        } else {
            throw JSONValidationError(QStringLiteral("Unknown format version: %s").arg(formatVersion));
        }

    } catch (const JSONValidationError& e) {
        emitFailed(tr("Could not understand pack index:\n") + e.cause());
        return false;
    }

    return true;
}

void ModrinthCreationTask::ensureMetaLoop()
{
    const QDir folder = FS::PathCombine(m_stagingPath, "minecraft", "jarmods");
    auto ensureMetadataTask = makeShared<EnsureMetadataTask>(m_resources, folder, ModPlatform::ResourceProvider::MODRINTH);
    connect(ensureMetadataTask.get(), &Task::succeeded, this, &ModrinthCreationTask::finishInstall);
    connect(ensureMetadataTask.get(), &Task::failed, this, &ModrinthCreationTask::emitFailed);
    connect(ensureMetadataTask.get(), &Task::aborted, this, &ModrinthCreationTask::emitAborted);
    connect(ensureMetadataTask.get(), &Task::progress, this, [this](qint64 current, qint64 total) {
        setDetails(tr("%1 out of %2 complete").arg(current).arg(total));
        setProgress(current, total);
    });
    connect(ensureMetadataTask.get(), &Task::stepProgress, this, &ModrinthCreationTask::propagateStepProgress);

    ensureMetadataTask->start();
    m_task = ensureMetadataTask;
}

bool ModrinthCreationTask::promptForUntrustedMods()
{
    if (m_trustedSource) {
        return true;
    }

    QStringList untrustedMods;

    for (const auto& file : m_files) {
        for (const auto& url : file.downloads) {
            if (url.scheme() != "https" || url.host() != BuildConfig.MODRINTH_DOWNLOAD_HOST) {
                untrustedMods.append(file.path);
                break;
            }
        }
    }

    const QDir mcDir{ FS::PathCombine(m_stagingPath, m_rootPath) };
    const QString modsPath{ FS::PathCombine(m_stagingPath, m_rootPath, "mods") };
    if (QDir(modsPath).exists()) {
        QDirIterator iter{ modsPath, QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks };
        while (iter.hasNext()) {
            untrustedMods.append(mcDir.relativeFilePath(iter.next()));
        }
    }

    if (untrustedMods.empty()) {
        return true;
    }

    UntrustedModsDialog dialog{ untrustedMods, m_parent };
    return dialog.exec() == QDialog::Accepted;
}

ModrinthCreationTask::~ModrinthCreationTask()
{
    for (auto* resource : m_resources) {
        delete resource;
    }
    m_resources.clear();
}

void ModrinthCreationTask::setManagedPack(BaseInstance* instance)
{
    // Don't add managed info to packs without an ID (most likely imported from ZIP)
    if (!m_managedId.isEmpty()) {
        instance->setManagedPack("modrinth", m_managedId, m_managedName, m_managedVersionId, version());
    } else {
        instance->setManagedPack("modrinth", "", name(), "", "");
    }
}

void ModrinthCreationTask::finishInstall()
{
    // Update information of the already installed instance, if any.
    if (m_oldInstance) {
        setAbortable(false);
        auto* inst = *m_oldInstance;

        // Only change the name if it didn't use a custom name, so that the previous custom name
        // is preserved, but if we're using the original one, we update the version string.
        // NOTE: This needs to come before the setManagedPack call!
        if (inst->name().contains(inst->getManagedPackVersionName()) && inst->name() != name()) {
            if (askForChangingInstanceName(m_parent, inst->name(), name()) == InstanceNameChange::ShouldChange) {
                inst->setName(name());
            }
        }

        setManagedPack(*m_oldInstance);
    }

    if (shouldOverride()) {
        bool deleteFailed = false;

        setAbortable(false);
        setStatus(tr("Removing old conflicting files..."));
        qDebug() << "Removing old files";

        for (const QString& path : m_filesToRemove) {
            if (!QFile::exists(path)) {
                continue;
            }

            qDebug() << "Removing" << path;

            if (!QFile::remove(path)) {
                qCritical() << "Could not remove" << path;
                deleteFailed = true;
            }
        }

        if (deleteFailed) {
            emitFailed(tr("Failed to remove old conflicting files."));
            return;
        }
    }
    downloadFiles(m_newInstance.get());
}
