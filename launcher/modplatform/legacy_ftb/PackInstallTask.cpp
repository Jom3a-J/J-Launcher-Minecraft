// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
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
 *      Copyright 2013-2021 MultiMC Contributors
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

#include "PackInstallTask.h"

#include <QtConcurrent>
#include <QFile>
#include <utility>

#include "BaseInstance.h"
#include "FileSystem.h"
#include "MMCZip.h"
#include "minecraft/GradleSpecifier.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "settings/INISettingsObject.h"

#include "Application.h"
#include "BuildConfig.h"
#include "logs/Privacy.h"
#include "modplatform/ServerSupport.h"

#include "net/ApiDownload.h"

namespace LegacyFTB {

PackInstallTask::PackInstallTask(QNetworkAccessManager* network, Modpack pack, QString version)
    : m_network(network), m_pack(std::move(pack)), m_version(std::move(version))
{}

void PackInstallTask::executeTask()
{
    downloadPack();
}

void PackInstallTask::downloadPack()
{
    setStatus(tr("Downloading zip for %1").arg(m_pack.name));
    setProgress(1, 4);
    setAbortable(false);

    const QString versionFolder = ModPlatform::legacyFtbPackVersionFolder(m_version);
    const QString path = QString("%1/%2/%3").arg(m_pack.dir, versionFolder, m_pack.file);
    auto entry = APPLICATION->metacache()->resolveEntry("FTBPacks", path);
    entry->setStale(true);
    m_archivePath = entry->getFullPath();
    m_netJobContainer.reset(new NetJob("Download FTB Pack", m_network));
    const bool privatePack = m_pack.type == PackType::Private;
    const QUrl clientUrl = ModPlatform::legacyFtbPackUrl(BuildConfig.LEGACY_FTB_CDN_BASE_URL, privatePack,
                                                         m_pack.dir, m_version, m_pack.file);
    m_netJobContainer->addNetAction(Net::ApiDownload::makeCached(clientUrl, entry));

    connect(m_netJobContainer.get(), &NetJob::succeeded, this, &PackInstallTask::onClientDownloadSucceeded);
    connect(m_netJobContainer.get(), &NetJob::failed, this, &PackInstallTask::emitFailed);
    connect(m_netJobContainer.get(), &NetJob::stepProgress, this, &PackInstallTask::propagateStepProgress);
    connect(m_netJobContainer.get(), &NetJob::aborted, this, &PackInstallTask::emitAborted);

    m_netJobContainer->start();

    setAbortable(true);
    progress(1, 4);
}

void PackInstallTask::onClientDownloadSucceeded()
{
    m_netJobContainer.reset();
    if (!shouldCreateServerPair() || ModPlatform::legacyFtbServerSupport(m_pack.serverPack) != ModPlatform::ServerSupport::Official) {
        unzip();
        return;
    }

    const bool privatePack = m_pack.type == PackType::Private;
    m_serverPackUrl = ModPlatform::legacyFtbPackUrl(BuildConfig.LEGACY_FTB_CDN_BASE_URL, privatePack,
                                                    m_pack.dir, m_version, m_pack.serverPack);
    m_serverArchivePath = FS::PathCombine(m_stagingPath, "server-pack", "legacy-server-pack.zip");
    FS::ensureFilePathExists(m_serverArchivePath);

    setStatus(tr("Downloading the official legacy FTB server pack"));
    auto job = makeShared<NetJob>(tr("Legacy FTB server pack download"), m_network);
    job->setAskRetry(false);
    job->addNetAction(Net::ApiDownload::makeFile(m_serverPackUrl, m_serverArchivePath));
    connect(job.get(), &NetJob::succeeded, this, &PackInstallTask::onServerPackDownloadSucceeded);
    connect(job.get(), &NetJob::failed, this, &PackInstallTask::onServerPackDownloadFailed);
    connect(job.get(), &NetJob::aborted, this, &PackInstallTask::onServerPackDownloadAborted);
    m_netJobContainer = job;
    setAbortable(true);
    job->start();
}

void PackInstallTask::onServerPackDownloadSucceeded()
{
    m_netJobContainer.reset();
    m_serverPackDownloaded = true;
    unzip();
}

void PackInstallTask::onServerPackDownloadFailed(QString reason)
{
    m_netJobContainer.reset();
    m_serverPackDownloaded = false;
    logWarning(tr("The published legacy FTB server pack could not be downloaded from %1 (%2). J Launcher will build the server from client files instead.")
                   .arg(Privacy::sanitizeUrl(m_serverPackUrl), Privacy::sanitizeText(reason)));
    unzip();
}

void PackInstallTask::onServerPackDownloadAborted()
{
    m_netJobContainer.reset();
    emitAborted();
}

void PackInstallTask::unzip()
{
    setStatus(tr("Extracting modpack"));
    setAbortable(false);
    progress(2, 4);

    QDir extractDir(m_stagingPath);

    const QString clientExtractPath = extractDir.absolutePath() + "/unzip";
    const QString serverArchivePath = m_serverPackDownloaded ? m_serverArchivePath : QString();
    const QString serverFilesPath = FS::PathCombine(m_stagingPath, "server-pack", "server-files");
    m_extractFuture = QtConcurrent::run(QThreadPool::globalInstance(),
        [archivePath = m_archivePath, clientExtractPath, serverArchivePath, serverFilesPath]() {
            ExtractionResult result;
            result.clientFiles = MMCZip::extractDir(archivePath, clientExtractPath);
            if (!result.clientFiles || serverArchivePath.isEmpty()) return result;

            QString failedEntry;
            if (!MMCZip::validateArchive(serverArchivePath, &failedEntry)) {
                qWarning() << "The published legacy FTB server pack failed archive validation at" << failedEntry
                           << "; falling back to client files.";
                return result;
            }
            if (!MMCZip::extractDir(serverArchivePath, serverFilesPath)) {
                qWarning() << "The published legacy FTB server pack could not be extracted; falling back to client files.";
                FS::deletePath(serverFilesPath);
                return result;
            }
            result.publishedServerPackExtracted = true;
            return result;
        });
    connect(&m_extractFutureWatcher, &QFutureWatcher<ExtractionResult>::finished, this, &PackInstallTask::onUnzipFinished);
    connect(&m_extractFutureWatcher, &QFutureWatcher<ExtractionResult>::canceled, this, &PackInstallTask::onUnzipCanceled);
    m_extractFutureWatcher.setFuture(m_extractFuture);
}

void PackInstallTask::onUnzipFinished()
{
    const auto result = m_extractFuture.result();
    if (!m_serverArchivePath.isEmpty()) FS::deletePath(m_serverArchivePath);
    if (!result.clientFiles) {
        emitFailed(tr("Failed to extract the legacy FTB modpack archive."));
        return;
    }
    m_serverPackExtracted = result.publishedServerPackExtracted;
    install();
}

void PackInstallTask::onUnzipCanceled()
{
    emitAborted();
}

void PackInstallTask::install()
{
    setStatus(tr("Installing modpack"));
    progress(3, 4);
    QDir unzipMcDir(m_stagingPath + "/unzip/minecraft");
    if (unzipMcDir.exists()) {
        // ok, found minecraft dir, move contents to instance dir
        if (!FS::move(m_stagingPath + "/unzip/minecraft", m_stagingPath + "/minecraft")) {
            emitFailed(tr("Failed to move unpacked Minecraft!"));
            return;
        }
    }

    QString instanceConfigPath = FS::PathCombine(m_stagingPath, "instance.cfg");
    m_instance =
        std::make_unique<MinecraftInstance>(m_globalSettings, std::make_unique<INISettingsObject>(instanceConfigPath), m_stagingPath);
    {
        SettingsObject::Lock const lock(m_instance->settings());

        auto* components = m_instance->getPackProfile();
        components->buildingFromScratch();
        components->setComponentVersion("net.minecraft", m_pack.mcVersion, true);

        bool fallback = true;

        // handle different versions
        QFile packJson(m_stagingPath + "/minecraft/pack.json");
        QDir jarmodDir = QDir(m_stagingPath + "/unzip/instMods");
        if (packJson.exists()) {
            if (packJson.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QJsonDocument doc = QJsonDocument::fromJson(packJson.readAll());
                packJson.close();

                // we only care about the libs
                QJsonArray libs = doc.object().value("libraries").toArray();

                for (const auto& value : libs) {
                    QString nameValue = value.toObject().value("name").toString();
                    if (!nameValue.startsWith("net.minecraftforge")) {
                        continue;
                    }

                    GradleSpecifier forgeVersion(nameValue);

                    components->setComponentVersion("net.minecraftforge",
                                                    forgeVersion.version().replace(m_pack.mcVersion, "").replace("-", ""));
                    packJson.remove();
                    fallback = false;
                    break;
                }
            } else {
                qWarning() << "Failed to open file" << packJson.fileName() << "for reading:" << packJson.errorString();
            }
        }

        if (jarmodDir.exists()) {
            qDebug() << "Found jarmods, installing...";

            QStringList jarmods;
            for (const auto& info : jarmodDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Files)) {
                qDebug() << "Jarmod:" << info.fileName();
                jarmods.push_back(info.absoluteFilePath());
            }

            components->installJarMods(jarmods);
            fallback = false;
        }

        // just nuke unzip directory, it s not needed anymore
        FS::deletePath(m_stagingPath + "/unzip");

        if (fallback) {
            // TODO: Some fallback mechanism... or just keep failing!
            emitFailed(tr("No installation method found!"));
            return;
        }

        components->saveNow();

        progress(4, 4);

        m_instance->setName(name());
        if (m_instIcon == "default") {
            m_instIcon = "ftb_logo";
        }
        m_instance->setIconKey(m_instIcon);
    }

    if (shouldCreateServerPair()) {
        const QString providerMarkerPath =
            FS::PathCombine(m_stagingPath, "server-pack", "provider.txt");
        FS::ensureFilePathExists(providerMarkerPath);
        QFile providerMarker(providerMarkerPath);
        if (!providerMarker.open(QIODevice::WriteOnly | QIODevice::Text)
            || providerMarker.write("ftb-legacy\n") != 11) {
            emitFailed(tr("Could not record the legacy FTB compatibility metadata."));
            return;
        }
        if (m_serverPackExtracted) {
            QFile publishedMarker(FS::PathCombine(m_stagingPath, "server-pack", "published-server-pack.txt"));
            if (!publishedMarker.open(QIODevice::WriteOnly | QIODevice::Text)
                || publishedMarker.write("ftb-legacy\n") != 11) {
                emitFailed(tr("Could not record the downloaded legacy FTB server pack."));
                return;
            }
        }
    }

    downloadFiles(m_instance.get());
}

bool PackInstallTask::abort()
{
    if (!canAbort()) {
        return false;
    }

    m_netJobContainer->abort();
    return InstanceTask::abort();
}

}  // namespace LegacyFTB
