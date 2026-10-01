// SPDX-License-Identifier: GPL-3.0-only

#include "JavaRuntimeInstallTask.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPointer>

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

namespace {
/*! Runtime folders currently being downloaded, keyed by normalized path.
 *
 *  Two installs of the same runtime (two servers that need Java 21 starting together, or a
 *  server and a game launch) must not share a folder: the second would delete the first one's
 *  half-written download.
 */
QHash<QString, QPointer<Task>>& runtimeInstallsInProgress()
{
    static QHash<QString, QPointer<Task>> installs;
    return installs;
}

QString runtimeKey(const QString& directory)
{
    return QDir::cleanPath(QDir(directory).absolutePath()).toLower();
}
}  // namespace

Task* runtimeInstallInProgress(const QString& directory)
{
    Task* owner = runtimeInstallsInProgress().value(runtimeKey(directory));
    return owner && owner->isRunning() ? owner : nullptr;
}

bool claimRuntimeDirectory(const QString& directory, Task* task)
{
    Task* owner = runtimeInstallInProgress(directory);
    if (owner && owner != task) {
        return false;
    }
    runtimeInstallsInProgress().insert(runtimeKey(directory), task);
    return true;
}

void releaseRuntimeDirectory(const QString& directory, const Task* task)
{
    auto& installs = runtimeInstallsInProgress();
    const QString key = runtimeKey(directory);
    if (installs.value(key) == task) {
        installs.remove(key);
    }
}

JavaRuntimeInstallTask::JavaRuntimeInstallTask(int majorVersion)
    : Task(), m_majorVersion(majorVersion),
      m_supportedArchitecture(SysInfo::getSupportedJavaArchitecture())
{
    connect(this, &Task::finished, this, &JavaRuntimeInstallTask::releaseRuntimeDirectory);
}

void JavaRuntimeInstallTask::releaseRuntimeDirectory()
{
    if (m_claimedRuntimeDirectory.isEmpty()) {
        return;
    }
    Java::releaseRuntimeDirectory(m_claimedRuntimeDirectory, this);
    m_claimedRuntimeDirectory.clear();
}

bool JavaRuntimeInstallTask::isUsableJava(const QString &javaPath)
{
    return JavaUtils::isJavaRuntimeLayoutComplete(javaPath);
}

bool JavaRuntimeInstallTask::canAbort() const
{
    return m_waitingForOtherInstall || (m_currentTask && m_currentTask->canAbort());
}

bool JavaRuntimeInstallTask::abort()
{
    if (m_waitingForOtherInstall) {
        // Only stop waiting; the other task keeps downloading for whoever started it.
        m_waitingForOtherInstall = false;
        if (isRunning()) {
            emitAborted();
        }
        return true;
    }
    return m_currentTask ? m_currentTask->abort() : Task::abort();
}

void JavaRuntimeInstallTask::finishAfterOtherInstall()
{
    if (!m_waitingForOtherInstall || !isRunning()) {
        return;
    }
    m_waitingForOtherInstall = false;
    if (isUsableJava(m_javaPath)) {
        emitSucceeded();
    } else {
        emitFailed(tr("Another download of Java %1 did not complete. Please try again.").arg(m_majorVersion));
    }
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

    if (!claimRuntimeDirectory(m_runtimeDirectory, this)) {
        // Another task is already downloading this exact runtime. Deleting its folder would break
        // both installs, so wait for it and then use whatever it produced.
        Task *owner = runtimeInstallInProgress(m_runtimeDirectory);
        setStatus(tr("Waiting for another Java %1 download to finish...").arg(m_majorVersion));
        m_currentTask.reset();
        m_waitingForOtherInstall = true;
        emit abortStatusChanged(true);
        connect(owner, &Task::finished, this, &JavaRuntimeInstallTask::finishAfterOtherInstall);
        connect(owner, &QObject::destroyed, this, &JavaRuntimeInstallTask::finishAfterOtherInstall);
        return;
    }
    m_claimedRuntimeDirectory = m_runtimeDirectory;

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
