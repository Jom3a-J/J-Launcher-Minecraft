// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2022 flowln <flowlnlnln@gmail.com>
 *  Copyright (c) 2022 Jamie Mansfield <jmansfield@cadixdev.org>
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2020-2021 Jamie Mansfield <jmansfield@cadixdev.org>
 *      Copyright 2020-2021 Petr Mrazek <peterix@gmail.com>
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "FTBPackInstallTask.h"

#include <utility>

#include "FileSystem.h"
#include "Json.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "modplatform/flame/FileResolvingTask.h"
#include "modplatform/flame/PackManifest.h"
#include "net/ChecksumValidator.h"
#include "settings/INISettingsObject.h"
#include "logs/Privacy.h"
#include "modplatform/ServerPackStaging.h"

#include "Application.h"
#include "BuildConfig.h"
#include "ui/dialogs/BlockedModsDialog.h"

#include <QFile>

namespace FTB {

PackInstallTask::PackInstallTask(Modpack pack, QString version, QWidget* parent)
    : m_pack(std::move(pack)), m_versionName(std::move(version)), m_parent(parent)
{}

PackInstallTask::~PackInstallTask() = default;

bool PackInstallTask::abort()
{
    if (!canAbort()) {
        return false;
    }

    bool aborted = true;

    if (m_net_job) {
        aborted &= m_net_job->abort();
    }
    if (m_modIdResolverTask) {
        aborted &= m_modIdResolverTask->abort();
    }
    cancelDedicatedServerPackRequests(this);
    if (m_serverInstaller) {
        m_serverInstaller->stop();
    }

    return aborted ? InstanceTask::abort() : false;
}

void PackInstallTask::executeTask()
{
    setStatus(tr("Getting the manifest..."));
    setAbortable(false);

    // Find pack version
    auto versionIt = std::find_if(m_pack.versions.constBegin(), m_pack.versions.constEnd(),
                                  [this](const FTB::VersionInfo& a) { return a.name == m_versionName; });

    if (versionIt == m_pack.versions.constEnd()) {
        emitFailed(tr("Failed to find pack version %1").arg(m_versionName));
        return;
    }

    const auto& version = *versionIt;

    auto netJob = makeShared<NetJob>("FTB::VersionFetch", APPLICATION->network());

    auto searchUrl = QString(BuildConfig.FTB_API_BASE_URL + "/modpack/%1/%2").arg(m_pack.id).arg(version.id);

    auto [action, response] = Net::Download::makeByteArray(QUrl(searchUrl));
    netJob->addNetAction(action);

    QObject::connect(netJob.get(), &NetJob::succeeded, this, [this, response] { onManifestDownloadSucceeded(response); });
    QObject::connect(netJob.get(), &NetJob::failed, this, &PackInstallTask::onManifestDownloadFailed);
    QObject::connect(netJob.get(), &NetJob::aborted, this, &PackInstallTask::abort);
    QObject::connect(netJob.get(), &NetJob::progress, this, &PackInstallTask::setProgress);

    m_net_job = netJob;

    setAbortable(true);
    netJob->start();
}

void PackInstallTask::onManifestDownloadSucceeded(QByteArray* responsePtr)
{
    // NOTE(TheKodeToad): moving the response out to avoid it from being destroyed by m_net_job.reset()
    QByteArray response = std::move(*responsePtr);
    m_net_job.reset();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "Error while parsing JSON response from FTB at " << parseError.offset << " reason: " << parseError.errorString();
        qWarning() << "Response body excerpt:"
                   << Privacy::sanitizeResponseBody(response, 2048);
        return;
    }

    FTB::Version version;
    try {
        auto obj = Json::requireObject(doc);
        FTB::loadVersion(version, obj);
    } catch (const JSONValidationError& e) {
        emitFailed(tr("Could not understand pack manifest:\n") + e.cause());
        return;
    }

    m_version = version;

    if (shouldCreateServerPair()) {
        probeDedicatedServerPack();
    } else {
        resolveMods();
    }
}

void PackInstallTask::probeDedicatedServerPack()
{
    setStatus(tr("Checking for the official FTB server installer..."));
    setAbortable(true);
    FTB::probeDedicatedServerPack(
        APPLICATION->network(), m_pack.id, m_version.id, this,
        [this](ModPlatform::ServerSupport support) {
        if (support == ModPlatform::ServerSupport::Unknown) {
            emitFailed(tr("J Launcher could not determine whether this FTB pack has an official server version. Check the connection and try again."));
            return;
        }
        m_hasDedicatedServerPack = support == ModPlatform::ServerSupport::Official;
        resolveMods();
    });
}

void PackInstallTask::resolveMods()
{
    setStatus(tr("Resolving mods..."));
    setAbortable(false);
    setProgress(0, 100);

    m_fileIds.clear();

    Flame::Manifest manifest;
    for (const auto& file : m_version.files) {
        if ((shouldCreateServerPair() || !file.serverOnly) && file.url.isEmpty()) {
            if (file.curseforge.file_id <= 0) {
                emitFailed(tr("Invalid manifest: There's no information available to download the file '%1'!").arg(file.name));
                return;
            }

            Flame::File flameFile;
            flameFile.projectId = file.curseforge.project_id;
            flameFile.fileId = file.curseforge.file_id;

            manifest.files.insert(flameFile.fileId, flameFile);
            m_fileIds.append(flameFile.fileId);
        } else {
            m_fileIds.append(-1);
        }
    }

    m_modIdResolverTask.reset(new Flame::FileResolvingTask(manifest));

    connect(m_modIdResolverTask.get(), &Flame::FileResolvingTask::succeeded, this, &PackInstallTask::onResolveModsSucceeded);
    connect(m_modIdResolverTask.get(), &Flame::FileResolvingTask::failed, this, &PackInstallTask::onResolveModsFailed);
    connect(m_modIdResolverTask.get(), &Flame::FileResolvingTask::aborted, this, &PackInstallTask::abort);
    connect(m_modIdResolverTask.get(), &Flame::FileResolvingTask::progress, this, &PackInstallTask::setProgress);

    setAbortable(true);

    m_modIdResolverTask->start();
}

void PackInstallTask::onResolveModsSucceeded()
{
    auto anyBlocked = false;

    Flame::Manifest results = m_modIdResolverTask->getResults();
    for (int index = 0; index < m_fileIds.size(); index++) {
        const auto fileId = m_fileIds.at(index);
        if (fileId < 0) {
            continue;
        }

        const Flame::File resultsFile = results.files.value(fileId);
        VersionFile& localFile = m_version.files[index];

        // First check for blocked mods
        if (resultsFile.version.downloadUrl.isEmpty()) {
            BlockedMod blockedMod;
            blockedMod.name = resultsFile.version.fileName;
            blockedMod.websiteUrl = QString("%1/download/%2").arg(resultsFile.pack.websiteUrl, QString::number(resultsFile.fileId));
            blockedMod.hash = resultsFile.version.hash;
            blockedMod.matched = false;
            blockedMod.localPath = "";
            blockedMod.targetFolder = resultsFile.targetFolder;

            m_blockedMods.append(blockedMod);
            if (localFile.serverOnly) {
                m_serverOnlyBlockedFiles.insert(blockedMod.name);
            }

            anyBlocked = true;
        } else {
            localFile.url = resultsFile.version.downloadUrl;
        }
    }

    m_modIdResolverTask.reset();

    if (anyBlocked) {
        qDebug() << "Blocked files found, displaying file list";

        BlockedModsDialog messageDialog(m_parent, tr("Blocked files found"),
                                        tr("The following files are not available for download in third party launchers.<br/>"
                                           "You will need to manually download them and add them to the instance."),
                                        m_blockedMods);

        messageDialog.setModal(true);

        if (messageDialog.exec() == QDialog::Accepted) {
            qDebug() << "Post dialog blocked mods list: " << m_blockedMods;
            createInstance();
        } else {
            abort();
        }

    } else {
        createInstance();
    }
}

void PackInstallTask::createInstance()
{
    setAbortable(false);

    setStatus(tr("Creating the instance..."));
    QCoreApplication::processEvents();

    auto instanceConfigPath = FS::PathCombine(m_stagingPath, "instance.cfg");
    auto instanceSettings = std::make_unique<INISettingsObject>(instanceConfigPath);

    m_instance = std::make_unique<MinecraftInstance>(m_globalSettings, std::move(instanceSettings), m_stagingPath);
    auto* components = m_instance->getPackProfile();
    components->buildingFromScratch();

    for (const auto& target : m_version.targets) {
        if (target.type == "game" && target.name == "minecraft") {
            components->setComponentVersion("net.minecraft", target.version, true);
            break;
        }
    }

    for (const auto& target : m_version.targets) {
        if (target.type != "modloader") {
            continue;
        }

        if (target.name == "forge") {
            components->setComponentVersion("net.minecraftforge", target.version);
        } else if (target.name == "fabric") {
            components->setComponentVersion("net.fabricmc.fabric-loader", target.version);
        } else if (target.name == "neoforge") {
            components->setComponentVersion("net.neoforged", target.version);
        } else if (target.name == "quilt") {
            components->setComponentVersion("org.quiltmc.quilt-loader", target.version);
        }
    }

    // install any jar mods
    QDir jarModsDir(FS::PathCombine(m_stagingPath, "minecraft", "jarmods"));
    if (jarModsDir.exists()) {
        QStringList jarMods;

        for (const auto& info : jarModsDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Files)) {
            jarMods.push_back(info.absoluteFilePath());
        }

        components->installJarMods(jarMods);
    }

    components->saveNow();

    m_instance->setName(name());
    m_instance->setIconKey(m_instIcon);
    m_instance->setManagedPack("ftb", QString::number(m_pack.id), m_pack.name, QString::number(m_version.id), m_version.name);

    m_instance->saveNow();

    onCreateInstanceSucceeded();
}

void PackInstallTask::onCreateInstanceSucceeded()
{
    downloadPack();
}

void PackInstallTask::downloadPack()
{
    setStatus(tr("Downloading mods..."));
    setAbortable(false);

    auto jobPtr = makeShared<NetJob>(tr("Mod download"), APPLICATION->network());
    using ServerLists = ModPlatform::ServerPackStaging::FileLists;
    ServerLists serverLists;
    if (shouldCreateServerPair()
        && (!serverLists.open(m_stagingPath, { ServerLists::ClientOnly, ServerLists::Include })
            || !ModPlatform::ServerPackStaging::recordProvider(m_stagingPath, "ftb"))) {
        emitFailed(tr("Could not prepare the FTB server compatibility manifest."));
        return;
    }
    if (m_hasDedicatedServerPack) {
        m_serverInstallerPath = ModPlatform::ServerPackStaging::path(m_stagingPath, "ftb-server-installer.exe");
        jobPtr->addNetAction(Net::Download::makeFile(
            QUrl(dedicatedServerInstallerUrl(m_pack.id, m_version.id)),
            m_serverInstallerPath));
    }
    for (const auto& file : m_version.files) {
        const QString relativePath = FS::PathCombine(file.path, file.name);
        serverLists.add(file.clientOnly ? ServerLists::ClientOnly : ServerLists::Include, relativePath);
        if (file.url.isEmpty()) {
            continue;
        }

        if (!file.serverOnly) {
            auto path = FS::PathCombine(m_stagingPath, ".minecraft", relativePath);
            qDebug() << "Will try to download" << Privacy::sanitizeUrl(file.url)
                     << "to" << Privacy::sanitizePath(path);
            auto dl = Net::Download::makeFile(file.url, path);
            if (!file.sha1.isEmpty()) {
                dl->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, file.sha1));
            }
            jobPtr->addNetAction(dl);
        }
        if (shouldCreateServerPair() && file.serverOnly) {
            auto path = ModPlatform::ServerPackStaging::serverFilesPath(m_stagingPath, relativePath);
            qDebug() << "Will try to download server-only file"
                     << Privacy::sanitizeUrl(file.url)
                     << "to" << Privacy::sanitizePath(path);
            auto dl = Net::Download::makeFile(file.url, path);
            if (!file.sha1.isEmpty()) {
                dl->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, file.sha1));
            }
            jobPtr->addNetAction(dl);
        }
    }
    serverLists.close();

    // HostScheduler pins each FTB host; leave other hosts free to download in parallel.
    connect(jobPtr.get(), &NetJob::succeeded, this, &PackInstallTask::onModDownloadSucceeded);
    connect(jobPtr.get(), &NetJob::failed, this, &PackInstallTask::onModDownloadFailed);
    connect(jobPtr.get(), &NetJob::aborted, this, &PackInstallTask::abort);
    connect(jobPtr.get(), &NetJob::progress, this, &PackInstallTask::setProgress);

    m_net_job = jobPtr;

    setAbortable(true);
    jobPtr->start();
}

void PackInstallTask::onModDownloadSucceeded()
{
    m_net_job.reset();
    if (!m_blockedMods.isEmpty()) {
        copyBlockedMods();
    }
    if (m_hasDedicatedServerPack) {
        installDedicatedServerPack();
        return;
    }
    QString manifestError;
    if (shouldCreateServerPair()
        && !writeServerIncludeList(m_stagingPath, m_version.files, &manifestError)) {
        emitFailed(manifestError);
        return;
    }
    downloadFiles(m_instance.get());
}

void PackInstallTask::installDedicatedServerPack()
{
    m_serverInstaller = std::make_unique<ServerInstallerRun>(m_serverInstallerPath, m_stagingPath, m_pack.id, m_version.id);
    connect(m_serverInstaller.get(), &ServerInstallerRun::failed, this, [this](const QString& reason) { emitFailed(reason); });
    connect(m_serverInstaller.get(), &ServerInstallerRun::succeeded, this, [this] { downloadFiles(m_instance.get()); });
    if (const QString error = m_serverInstaller->prepare(); !error.isEmpty()) {
        emitFailed(error);
        return;
    }
    setStatus(tr("Preparing server files with the official FTB installer..."));
    setAbortable(true);
    m_serverInstaller->start();
}

void PackInstallTask::onManifestDownloadFailed(QString reason)
{
    m_net_job.reset();
    emitFailed(std::move(reason));
}
void PackInstallTask::onResolveModsFailed(QString reason)
{
    m_net_job.reset();
    emitFailed(std::move(reason));
}
void PackInstallTask::onCreateInstanceFailed(QString reason)
{
    emitFailed(std::move(reason));
}
void PackInstallTask::onModDownloadFailed(QString reason)
{
    m_net_job.reset();
    emitFailed(std::move(reason));
}

/// @brief copy the matched blocked mods to the instance staging area
void PackInstallTask::copyBlockedMods()
{
    setStatus(tr("Copying Blocked Mods..."));
    setAbortable(false);
    int i = 0;
    auto total = m_blockedMods.length();
    setProgress(i, total);
    for (const auto& mod : m_blockedMods) {
        if (!mod.matched) {
            qDebug() << mod.name << "was not matched to a local file, skipping copy";
            continue;
        }

        auto destPath = m_serverOnlyBlockedFiles.contains(mod.name)
            ? ModPlatform::ServerPackStaging::serverFilesPath(m_stagingPath, FS::PathCombine(mod.targetFolder, mod.name))
            : FS::PathCombine(m_stagingPath, ".minecraft", mod.targetFolder,
                              mod.name);

        setStatus(tr("Copying Blocked Mods (%1 out of %2 are done)").arg(QString::number(i), QString::number(total)));

        qDebug() << "Will try to copy" << Privacy::sanitizePath(mod.localPath)
                 << "to" << Privacy::sanitizePath(destPath);

        if (!FS::copy(mod.localPath, destPath)()) {
            qDebug() << "Copy of" << Privacy::sanitizePath(mod.localPath)
                     << "to" << Privacy::sanitizePath(destPath) << "Failed";
        }

        i++;
        setProgress(i, total);
    }

    setAbortable(true);
}

}  // namespace FTB
