// SPDX-License-Identifier: GPL-3.0-only
/*
 *  J Launcher - Minecraft Launcher
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 */

#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTest>

#include "modplatform/flame/FlameInstanceCreationTask.h"

namespace {

class FailingTask final : public Task {
   public:
    FailingTask() : Task(false) {}

   private:
    void executeTask() override { emitFailed(QStringLiteral("download failed")); }
};

class InstallTask final : public Task {
   public:
    InstallTask() : Task(false) {}

    void fail(const QString& reason)
    {
        m_failureCalls++;
        emitFailed(reason);
    }
    int failureCalls() const { return m_failureCalls; }

   private:
    void executeTask() override {}

    int m_failureCalls = 0;
};

}  // namespace

class FlameDownloadCompletionTest : public QObject {
    Q_OBJECT

   private slots:
    void failedDownloadDoesNotRunServerPackContinuation()
    {
        Net::HostScheduler scheduler;
        QNetworkAccessManager network;
        NetJob downloadJob(QStringLiteral("Failing modpack download"), &network, -1, &scheduler);
        InstallTask installTask;
        auto failedDownload = makeShared<FailingTask>();
        downloadJob.addTask(failedDownload);

        int serverPackProcessingCalls = 0;
        int abortCalls = 0;
        QString installFailure;
        QSignalSpy downloadFailed(&downloadJob, &Task::failed);
        QSignalSpy installFailed(&installTask, &Task::failed);

        installTask.start();
        QVERIFY(installTask.isRunning());
        Flame::Internal::connectDownloadJobCompletion(
            &downloadJob, &installTask,
            [&] {
                // Mirrors extractServerPack(): a missing archive would fail the install again.
                serverPackProcessingCalls++;
                installTask.fail(QStringLiteral("server-pack archive is missing"));
            },
            [&](QString reason) {
                installFailure = reason;
                installTask.fail(reason);
            },
            [&] { abortCalls++; });

        downloadJob.start();
        QTRY_COMPARE_WITH_TIMEOUT(downloadFailed.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(installFailed.count(), 1, 5000);

        QCOMPARE(installFailure, QStringLiteral("download failed"));
        QCOMPARE(installTask.failureCalls(), 1);
        QCOMPARE(serverPackProcessingCalls, 0);
        QCOMPARE(abortCalls, 0);
    }
};

QTEST_GUILESS_MAIN(FlameDownloadCompletionTest)

#include "FlameDownloadCompletion_test.moc"
