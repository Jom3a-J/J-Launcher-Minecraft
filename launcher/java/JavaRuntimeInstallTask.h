// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "tasks/Task.h"

namespace Meta {
class Version;
}

namespace Java {

/*! Only one task at a time may download into a managed Java runtime folder: a second one would
 *  delete the first one's half-written files. Server installs and game launches both claim the
 *  folder before downloading, and anyone else waits for the running owner to finish.
 */
/// The running task downloading into directory, or nullptr.
Task* runtimeInstallInProgress(const QString& directory);
/// Claims directory for task. False when another running task already holds it.
bool claimRuntimeDirectory(const QString& directory, Task* task);
/// Gives up task's claim on directory, if it still holds it.
void releaseRuntimeDirectory(const QString& directory, const Task* task);

class JavaRuntimeInstallTask final : public Task
{
    Q_OBJECT

public:
    explicit JavaRuntimeInstallTask(int majorVersion);

    QString javaPath() const { return m_javaPath; }
    static bool isUsableJava(const QString &javaPath);

    bool canAbort() const override;
    bool abort() override;

protected:
    void executeTask() override;

private:
    void loadMajorVersion();
    void installRuntime(const std::shared_ptr<Meta::Version> &version);
    void attachTask(const Task::Ptr &task);
    void finishInstallation();
    /// Stops claiming m_runtimeDirectory, so another install of the same runtime may proceed.
    void releaseRuntimeDirectory();
    /// Ends a wait on another task that was installing the same runtime.
    void finishAfterOtherInstall();

    int m_majorVersion = 0;
    QString m_supportedArchitecture;
    QString m_runtimeDirectory;
    QString m_javaPath;
    Task::Ptr m_currentTask;
    /// The runtime folder this task has claimed while it downloads into it.
    QString m_claimedRuntimeDirectory;
    /// True while another task downloads the runtime this task needs.
    bool m_waitingForOtherInstall = false;
};

}  // namespace Java
