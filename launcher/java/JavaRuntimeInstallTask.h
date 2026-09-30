// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "tasks/Task.h"

namespace Meta {
class Version;
}

namespace Java {

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
    /// Key under which this task claims m_runtimeDirectory while it downloads into it.
    QString m_claimedRuntimeKey;
    /// True while another task downloads the runtime this task needs.
    bool m_waitingForOtherInstall = false;
};

}  // namespace Java
