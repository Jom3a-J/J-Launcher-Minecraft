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

#include "Application.h"
#include "BuildConfig.h"
#include "ui/dialogs/BlockedModsDialog.h"

#include <QFile>
#include <QDirIterator>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#endif

namespace FTB {

namespace {
QString dedicatedServerInstallerUrl(int packId, int versionId)
{
    return QString(BuildConfig.FTB_API_BASE_URL
                   + "/modpack/%1/%2/server/windows")
        .arg(packId)
        .arg(versionId);
}

bool verifyTrustedWindowsExecutable(const QString& path, QString* error)
{
#ifdef Q_OS_WIN
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    const std::wstring nativePath =
        QDir::toNativeSeparators(path).toStdWString();
    fileInfo.pcwszFilePath = nativePath.c_str();

    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policy, &trustData);
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trustData);
    if (status == ERROR_SUCCESS) {
        return true;
    }
    if (error) {
        *error = QObject::tr(
            "The official FTB server installer did not pass Windows signature "
            "verification (error 0x%1), so it was not run.")
                     .arg(static_cast<qulonglong>(
                              static_cast<unsigned long>(status)),
                          8, 16, QLatin1Char('0'));
    }
    return false;
#else
    Q_UNUSED(path)
    if (error) {
        *error = QObject::tr(
            "Automatic FTB server-package installation is currently supported "
            "on Windows only.");
    }
    return false;
#endif
}
}
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
    if (m_serverPackProbe) {
        disconnect(m_serverPackProbe, nullptr, this, nullptr);
        m_serverPackProbe->abort();
        m_serverPackProbe->deleteLater();
        m_serverPackProbe = nullptr;
    }
    if (m_serverInstallerProcess
        && m_serverInstallerProcess->state() != QProcess::NotRunning) {
        disconnect(m_serverInstallerProcess.get(), nullptr, this, nullptr);
        m_serverInstallerProcess->kill();
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
#ifdef Q_OS_WIN
    setStatus(tr("Checking for the official FTB server installer..."));
    setAbortable(true);
    QNetworkRequest request(
        QUrl(dedicatedServerInstallerUrl(m_pack.id, m_version.id)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    m_serverPackProbe = APPLICATION->network()->head(request);
    connect(m_serverPackProbe, &QNetworkReply::finished, this, [this]() {
        if (!m_serverPackProbe) {
            return;
        }
        const int status = m_serverPackProbe
                               ->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                               .toInt();
        const auto networkError = m_serverPackProbe->error();
        m_serverPackProbe->deleteLater();
        m_serverPackProbe = nullptr;
        if (status == 200) {
            m_hasDedicatedServerPack = true;
        } else if (status == 404) {
            m_hasDedicatedServerPack = false;
        } else if (networkError != QNetworkReply::NoError) {
            emitFailed(tr(
                "J Launcher could not determine whether this FTB pack has an official "
                "server version. Check the connection and try again."));
            return;
        } else {
            emitFailed(tr(
                "The FTB service returned an unexpected response (%1) while checking "
                "for a server version.")
                           .arg(status));
            return;
        }
        resolveMods();
    });
#else
    m_hasDedicatedServerPack = false;
    resolveMods();
#endif
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
    QFile clientOnlyFile;
    QFile serverIncludeFile;
    QFile providerMarker;
    if (shouldCreateServerPair()) {
        const QString clientOnlyPath =
            FS::PathCombine(m_stagingPath, "server-pack", "client-only.txt");
        const QString serverIncludePath =
            FS::PathCombine(m_stagingPath, "server-pack", "include.txt");
        FS::ensureFilePathExists(clientOnlyPath);
        FS::ensureFilePathExists(serverIncludePath);
        clientOnlyFile.setFileName(clientOnlyPath);
        serverIncludeFile.setFileName(serverIncludePath);
        providerMarker.setFileName(
            FS::PathCombine(m_stagingPath, "server-pack", "provider.txt"));
        if (!clientOnlyFile.open(QIODevice::WriteOnly | QIODevice::Text)
            || !serverIncludeFile.open(QIODevice::WriteOnly | QIODevice::Text)
            || !providerMarker.open(QIODevice::WriteOnly | QIODevice::Text)
            || providerMarker.write("ftb\n") != 4) {
            emitFailed(tr("Could not prepare the FTB server compatibility manifest."));
            return;
        }
    }
    if (m_hasDedicatedServerPack) {
        m_serverInstallerPath = FS::PathCombine(
            m_stagingPath, "server-pack", "ftb-server-installer.exe");
        jobPtr->addNetAction(Net::Download::makeFile(
            QUrl(dedicatedServerInstallerUrl(m_pack.id, m_version.id)),
            m_serverInstallerPath));
    }
    for (const auto& file : m_version.files) {
        const QString relativePath = FS::PathCombine(file.path, file.name);
        if (shouldCreateServerPair() && file.clientOnly) {
            clientOnlyFile.write(QDir::fromNativeSeparators(relativePath).toUtf8());
            clientOnlyFile.write("\n");
        }
        if (shouldCreateServerPair() && !file.clientOnly) {
            serverIncludeFile.write(QDir::fromNativeSeparators(relativePath).toUtf8());
            serverIncludeFile.write("\n");
        }
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
            auto path = FS::PathCombine(m_stagingPath, "server-pack", "server-files",
                                        relativePath);
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
    if (clientOnlyFile.isOpen()) {
        clientOnlyFile.close();
    }
    if (serverIncludeFile.isOpen()) {
        serverIncludeFile.close();
    }
    if (providerMarker.isOpen()) {
        providerMarker.close();
    }

    jobPtr->setMaxConcurrent(1);  // FTB blocks multiple requests at a time
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
        && !finalizeServerCompatibilityManifest(&manifestError)) {
        emitFailed(manifestError);
        return;
    }
    downloadFiles(m_instance.get());
}

bool PackInstallTask::finalizeServerCompatibilityManifest(QString* error)
{
    QFile includeFile(FS::PathCombine(
        m_stagingPath, "server-pack", "include.txt"));
    if (!includeFile.open(QIODevice::WriteOnly | QIODevice::Text
                          | QIODevice::Truncate)) {
        if (error) {
            *error = tr("Could not finalize the FTB server compatibility manifest.");
        }
        return false;
    }
    for (const auto& file : m_version.files) {
        if (file.clientOnly) {
            continue;
        }
        const QString relativePath = FS::PathCombine(file.path, file.name);
        const QString downloadedPath = file.serverOnly
            ? FS::PathCombine(m_stagingPath, "server-pack", "server-files",
                              relativePath)
            : FS::PathCombine(m_stagingPath, ".minecraft", relativePath);
        if (!QFileInfo(downloadedPath).isFile()) {
            if (file.optional) {
                continue;
            }
            if (error) {
                *error = tr(
                    "The FTB pack download is incomplete. A required server file "
                    "was not downloaded: %1")
                             .arg(QDir::fromNativeSeparators(relativePath));
            }
            return false;
        }
        includeFile.write(QDir::fromNativeSeparators(relativePath).toUtf8());
        includeFile.write("\n");
    }
    return true;
}

void PackInstallTask::installDedicatedServerPack()
{
    QString verificationError;
    if (!verifyTrustedWindowsExecutable(m_serverInstallerPath,
                                        &verificationError)) {
        emitFailed(verificationError);
        return;
    }

    const QString serverRoot = FS::PathCombine(
        m_stagingPath, "server-pack", "server-files");
    if (!QDir().mkpath(serverRoot)) {
        emitFailed(tr("Could not create the official FTB server-pack folder."));
        return;
    }

    setStatus(tr("Preparing server files with the official FTB installer..."));
    setAbortable(true);
    m_serverInstallerProcess = std::make_unique<QProcess>(this);
    m_serverInstallerProcess->setWorkingDirectory(serverRoot);
    m_serverInstallerProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_serverInstallerProcess.get(), &QProcess::readyReadStandardOutput,
            this, [this]() {
                const QString output = QString::fromUtf8(
                    m_serverInstallerProcess->readAllStandardOutput()).trimmed();
                if (!output.isEmpty()) {
                    qDebug() << "FTB server installer:"
                             << Privacy::sanitizeText(output);
                }
            });
    connect(m_serverInstallerProcess.get(), &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError processError) {
                if (processError == QProcess::FailedToStart) {
                    emitFailed(tr(
                        "The verified FTB server installer could not be started."));
                }
            });
    connect(m_serverInstallerProcess.get(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, serverRoot](int exitCode,
                                    QProcess::ExitStatus status) {
                const QByteArray output = m_serverInstallerProcess->readAll();
                QProcess* completedProcess = m_serverInstallerProcess.release();
                completedProcess->deleteLater();
                if (status != QProcess::NormalExit || exitCode != 0) {
                    qWarning() << "FTB server installer failed:"
                               << Privacy::sanitizeText(QString::fromUtf8(output));
                    emitFailed(tr(
                        "The official FTB server installer could not prepare the server files "
                        "(exit code %1).")
                                   .arg(exitCode));
                    return;
                }
                QFile marker(FS::PathCombine(
                    m_stagingPath, "server-pack",
                    "published-server-pack.txt"));
                QDirIterator installedFiles(
                    serverRoot,
                    QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
                if (!installedFiles.hasNext()
                    || !marker.open(QIODevice::WriteOnly | QIODevice::Text)
                    || marker.write("ftb\n") != 4) {
                    emitFailed(tr(
                        "The FTB server installer finished without producing usable "
                        "server files."));
                    return;
                }
                QFile::remove(m_serverInstallerPath);
                downloadFiles(m_instance.get());
            });
    m_serverInstallerProcess->start(
        m_serverInstallerPath,
        { QStringLiteral("-pack"), QString::number(m_pack.id),
          QStringLiteral("-version"), QString::number(m_version.id),
          QStringLiteral("-dir"), serverRoot,
          QStringLiteral("-auto"), QStringLiteral("-force"),
          QStringLiteral("-just-files"), QStringLiteral("-validate"),
          QStringLiteral("-no-colours") });
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
            ? FS::PathCombine(m_stagingPath, "server-pack", "server-files",
                              FS::PathCombine(mod.targetFolder, mod.name))
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
