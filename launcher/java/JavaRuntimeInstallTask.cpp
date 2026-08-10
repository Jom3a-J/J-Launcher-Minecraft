// SPDX-License-Identifier: GPL-3.0-only

#include "JavaRuntimeInstallTask.h"

#include <QDir>
#include <QFileInfo>


#include "Application.h"
#include "FileSystem.h"
#include "QObjectPtr.h"
#include "SysInfo.h"
#include "java/JavaMetadata.h"
#include "java/JavaUtils.h"
#include "java/download/ArchiveDownloadTask.h"
#include "java/download/ManifestDownloadTask.h"
#include "meta/Index.h"
#include "meta/Version.h"
#include "net/Mode.h"

namespace Java {

JavaRuntimeInstallTask::JavaRuntimeInstallTask(int majorVersion)
    : Task(), m_majorVersion(majorVersion),
      m_supportedArchitecture(SysInfo::getSupportedJavaArchitecture())
{
}

bool JavaRuntimeInstallTask::isUsableJava(const QString &javaPath)
{
    return JavaUtils::isJavaRuntimeLayoutComplete(javaPath);
}

bool JavaRuntimeInstallTask::canAbort() const
{
    return m_currentTask && m_currentTask->canAbort();
}

bool JavaRuntimeInstallTask::abort()
{
    return m_currentTask ? m_currentTask->abort() : Task::abort();
}

void JavaRuntimeInstallTask::executeTask()
{
    auto *application = APPLICATION_DYN;
    if (!application) {
        emitFailed(tr("The application runtime is not available."));
        return;
    }
    if (m_majorVersion <= 0) {
        emitFailed(tr("The required Java version could not be determined."));
        return;
    }
    if (m_supportedArchitecture.isEmpty()) {
        emitFailed(tr("Automatic Java installation is not available for %1-%2.")
                       .arg(SysInfo::currentSystem(), SysInfo::useQTForArch()));
        return;
    }

    setStatus(tr("Loading Java %1 runtime information...").arg(m_majorVersion));
    const auto versionList = application->metadataIndex()->get("net.minecraft.java");
    m_currentTask = versionList->getLoadTask();
    attachTask(m_currentTask);
    connect(m_currentTask.get(), &Task::succeeded, this,
            &JavaRuntimeInstallTask::loadMajorVersion);
    connect(m_currentTask.get(), &Task::failed, this,
            &JavaRuntimeInstallTask::emitFailed);
    connect(m_currentTask.get(), &Task::aborted, this,
            &JavaRuntimeInstallTask::emitAborted);
    if (!m_currentTask->isRunning()) {
        m_currentTask->start();
    }
}

void JavaRuntimeInstallTask::loadMajorVersion()
{
    auto *application = APPLICATION_DYN;
    if (!application || !isRunning()) {
        return;
    }
    const auto versionList = application->metadataIndex()->get("net.minecraft.java");
    const auto version = versionList->getVersion(QString("java%1").arg(m_majorVersion));
    if (!version) {
        emitFailed(tr("Java %1 is not present in the launcher metadata.").arg(m_majorVersion));
        return;
    }
    if (version->isLoaded()) {
        installRuntime(version);
        return;
    }

    m_currentTask = application->metadataIndex()->loadVersion(
        "net.minecraft.java", version->version(), Net::Mode::Online);
    attachTask(m_currentTask);
    connect(m_currentTask.get(), &Task::succeeded, this,
            [this, version] { installRuntime(version); });
    connect(m_currentTask.get(), &Task::failed, this,
            &JavaRuntimeInstallTask::emitFailed);
    connect(m_currentTask.get(), &Task::aborted, this,
            &JavaRuntimeInstallTask::emitAborted);
    if (!m_currentTask->isRunning()) {
        m_currentTask->start();
    }
}

void JavaRuntimeInstallTask::installRuntime(
    const std::shared_ptr<Meta::Version> &version)
{
    auto *application = APPLICATION_DYN;
    if (!application || !isRunning() || !version || !version->data()) {
        if (isRunning()) {
            emitFailed(tr("Java %1 runtime metadata is incomplete.").arg(m_majorVersion));
        }
        return;
    }

    Java::MetadataPtr selected;
    for (const auto &runtime : version->data()->runtimes) {
        if (runtime && runtime->runtimeOS == m_supportedArchitecture
            && runtime->version.major() == m_majorVersion) {
            selected = runtime;
            break;
        }
    }
    if (!selected) {
        emitFailed(tr("No Java %1 runtime is published for %2.")
                       .arg(m_majorVersion)
                       .arg(m_supportedArchitecture));
        return;
    }
    if (selected->m_name.isEmpty() || selected->m_name == "."
        || selected->m_name == ".." || selected->m_name.contains('/')
        || selected->m_name.contains('\\')) {
        emitFailed(tr("The Java runtime metadata contains an unsafe installation name."));
        return;
    }

    const QDir javaDirectory(application->javaPath());
    m_runtimeDirectory = javaDirectory.absoluteFilePath(selected->m_name);
    m_javaPath = QDir(m_runtimeDirectory).filePath(
        QStringLiteral("bin/") + JavaUtils::javaExecutable);
    if (isUsableJava(m_javaPath)) {
        emitSucceeded();
        return;
    }
    if (QFileInfo::exists(m_runtimeDirectory)) {
        FS::deletePath(m_runtimeDirectory);
    }

    setStatus(tr("Downloading Java %1...").arg(m_majorVersion));
    switch (selected->downloadType) {
        case DownloadType::Manifest:
            m_currentTask = makeShared<ManifestDownloadTask>(
                QUrl(selected->url), m_runtimeDirectory,
                selected->checksumType, selected->checksumHash);
            break;
        case DownloadType::Archive:
            m_currentTask = makeShared<ArchiveDownloadTask>(
                QUrl(selected->url), m_runtimeDirectory,
                selected->checksumType, selected->checksumHash);
            break;
        case DownloadType::Unknown:
            emitFailed(tr("The Java %1 download type is unsupported.").arg(m_majorVersion));
            return;
    }

    attachTask(m_currentTask);
    connect(m_currentTask.get(), &Task::succeeded, this,
            &JavaRuntimeInstallTask::finishInstallation);
    connect(m_currentTask.get(), &Task::failed, this, [this](const QString &reason) {
        FS::deletePath(m_runtimeDirectory);
        emitFailed(reason);
    });
    connect(m_currentTask.get(), &Task::aborted, this, [this] {
        FS::deletePath(m_runtimeDirectory);
        emitAborted();
    });
    m_currentTask->start();
}

void JavaRuntimeInstallTask::attachTask(const Task::Ptr &task)
{
    if (!task) {
        return;
    }
    connect(task.get(), &Task::progress, this, &JavaRuntimeInstallTask::setProgress);
    connect(task.get(), &Task::stepProgress, this,
            &JavaRuntimeInstallTask::propagateStepProgress);
    connect(task.get(), &Task::status, this, &JavaRuntimeInstallTask::setStatus);
    connect(task.get(), &Task::details, this, &JavaRuntimeInstallTask::setDetails);
}

void JavaRuntimeInstallTask::finishInstallation()
{
    if (!isUsableJava(m_javaPath)) {
        FS::deletePath(m_runtimeDirectory);
        emitFailed(tr("The downloaded Java %1 runtime is incomplete.").arg(m_majorVersion));
        return;
    }
    emitSucceeded();
}

}  // namespace Java
