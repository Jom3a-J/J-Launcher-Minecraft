/* Copyright 2013-2021 MultiMC Contributors
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

#include "SingleZipPackInstallTask.h"

#include <QFile>
#include <QtConcurrent>

#include "FileSystem.h"
#include "MMCZip.h"
#include "TechnicPackProcessor.h"

#include "Application.h"

#include "net/ApiDownload.h"
#include "logs/Privacy.h"

Technic::SingleZipPackInstallTask::SingleZipPackInstallTask(const QUrl& sourceUrl, const QString& minecraftVersion,
                                                            const QUrl& serverPackUrl)
{
    m_sourceUrl = sourceUrl;
    m_minecraftVersion = minecraftVersion;
    m_serverPackUrl = serverPackUrl;
}

bool Technic::SingleZipPackInstallTask::abort()
{
    if (m_abortable) {
        return m_filesNetJob->abort();
    }
    return false;
}

void Technic::SingleZipPackInstallTask::executeTask()
{
    if (shouldCreateServerPair()) {
        const QString markerPath = FS::PathCombine(m_stagingPath, "server-pack", "provider.txt");
        FS::ensureFilePathExists(markerPath);
        QFile marker(markerPath);
        if (!marker.open(QIODevice::WriteOnly | QIODevice::Text) || marker.write("technic\n") != 8) {
            emitFailed(tr("Could not record the Technic compatibility metadata."));
            return;
        }
    }
    if (shouldCreateServerPair() && (m_serverPackUrl.isEmpty() || !m_serverPackUrl.isValid())) {
        logWarning(tr("Technic does not publish a dedicated server pack for this modpack. The launcher will derive server content from the client pack."));
    }
    setStatus(tr("Downloading modpack:\n%1")
                  .arg(Privacy::sanitizeUrl(m_sourceUrl)));

    const QString path = m_sourceUrl.host() + '/' + m_sourceUrl.path();
    auto entry = APPLICATION->metacache()->resolveEntry("general", path);
    entry->setStale(true);
    m_filesNetJob.reset(new NetJob(tr("Modpack download"), APPLICATION->network()));
    m_filesNetJob->addNetAction(Net::ApiDownload::makeCached(m_sourceUrl, entry));
    m_archivePath = entry->getFullPath();
    if (shouldCreateServerPair() && m_serverPackUrl.isValid() && !m_serverPackUrl.isEmpty()) {
        const QString serverPath = m_serverPackUrl.host() + '/' + m_serverPackUrl.path();
        auto serverEntry = APPLICATION->metacache()->resolveEntry("general", serverPath);
        serverEntry->setStale(true);
        m_filesNetJob->addNetAction(Net::ApiDownload::makeCached(m_serverPackUrl, serverEntry));
        m_serverArchivePath = serverEntry->getFullPath();
    }
    auto job = m_filesNetJob.get();
    connect(job, &NetJob::succeeded, this, &Technic::SingleZipPackInstallTask::downloadSucceeded);
    connect(job, &NetJob::progress, this, &Technic::SingleZipPackInstallTask::downloadProgressChanged);
    connect(job, &NetJob::stepProgress, this, &Technic::SingleZipPackInstallTask::propagateStepProgress);
    connect(job, &NetJob::failed, this, &Technic::SingleZipPackInstallTask::downloadFailed);
    m_filesNetJob->start();
}

void Technic::SingleZipPackInstallTask::downloadSucceeded()
{
    m_abortable = false;

    setStatus(tr("Extracting modpack"));
    qDebug() << "Attempting to create instance from"
             << Privacy::sanitizePath(m_archivePath);

    const QString archivePath = m_archivePath;
    const QString serverArchivePath = m_serverArchivePath;
    const QString stagingPath = m_stagingPath;
    m_extractFuture = QtConcurrent::run(QThreadPool::globalInstance(), [archivePath, serverArchivePath, stagingPath]() -> QString {
        QString failedEntry;
        if (!MMCZip::validateArchive(archivePath, &failedEntry)) {
            return QObject::tr("The Technic provider archive is corrupt (failed integrity check at %1).")
                .arg(failedEntry.isEmpty() ? QObject::tr("an unknown file") : failedEntry);
        }
        if (!MMCZip::extractDir(archivePath, FS::PathCombine(stagingPath, "minecraft"))) {
            return QObject::tr("Failed to extract the Technic modpack archive.");
        }
        if (!serverArchivePath.isEmpty()) {
            failedEntry.clear();
            if (!MMCZip::validateArchive(serverArchivePath, &failedEntry)) {
                return QObject::tr("The Technic server-pack archive is corrupt (failed integrity check at %1).")
                    .arg(failedEntry.isEmpty() ? QObject::tr("an unknown file") : failedEntry);
            }
            const QString serverRoot = FS::PathCombine(stagingPath, "server-pack", "server-files");
            if (!MMCZip::extractDir(serverArchivePath, serverRoot)) {
                return QObject::tr("Failed to extract the Technic server-pack archive.");
            }
            QFile marker(FS::PathCombine(stagingPath, "server-pack", "published-server-pack.txt"));
            if (!marker.open(QIODevice::WriteOnly | QIODevice::Text) || marker.write("technic\n") != 8) {
                return QObject::tr("Could not record the downloaded Technic server pack.");
            }
        }
        return {};
    });
    connect(&m_extractFutureWatcher, &QFutureWatcher<QString>::finished, this, &Technic::SingleZipPackInstallTask::extractFinished);
    connect(&m_extractFutureWatcher, &QFutureWatcher<QString>::canceled, this, &Technic::SingleZipPackInstallTask::extractAborted);
    m_extractFutureWatcher.setFuture(m_extractFuture);
    m_filesNetJob.reset();
}

void Technic::SingleZipPackInstallTask::downloadFailed(QString reason)
{
    m_abortable = false;
    m_filesNetJob.reset();
    emitFailed(reason);
}

void Technic::SingleZipPackInstallTask::downloadProgressChanged(qint64 current, qint64 total)
{
    m_abortable = true;
    setProgress(current / 2, total);
}

void Technic::SingleZipPackInstallTask::extractFinished()
{
    const QString error = m_extractFuture.result();
    if (!error.isEmpty()) {
        emitFailed(error);
        return;
    }
    QDir extractDir(m_stagingPath);

    qDebug() << "Fixing permissions for extracted pack files...";
    QDirIterator it(extractDir, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        auto filepath = it.next();
        QFileInfo file(filepath);
        auto permissions = QFile::permissions(filepath);
        auto origPermissions = permissions;
        if (file.isDir()) {
            // Folder +rwx for current user
            permissions |= QFileDevice::Permission::ReadUser | QFileDevice::Permission::WriteUser | QFileDevice::Permission::ExeUser;
        } else {
            // File +rw for current user
            permissions |= QFileDevice::Permission::ReadUser | QFileDevice::Permission::WriteUser;
        }
        if (origPermissions != permissions) {
            if (!QFile::setPermissions(filepath, permissions)) {
                logWarning(tr("Could not fix permissions for %1").arg(filepath));
            } else {
                qDebug() << "Fixed" << Privacy::sanitizePath(filepath);
            }
        }
    }

    auto packProcessor = makeShared<Technic::TechnicPackProcessor>();
    connect(packProcessor.get(), &Technic::TechnicPackProcessor::succeeded, this, &Technic::SingleZipPackInstallTask::emitSucceeded);
    connect(packProcessor.get(), &Technic::TechnicPackProcessor::failed, this, &Technic::SingleZipPackInstallTask::emitFailed);
    packProcessor->run(m_globalSettings, name(), m_instIcon, m_stagingPath, m_minecraftVersion);
}

void Technic::SingleZipPackInstallTask::extractAborted()
{
    emitFailed(tr("Instance import has been aborted."));
}
