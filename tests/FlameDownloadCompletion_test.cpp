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
#include <QTimer>
#include <QTest>
#include <QVector>

#include "modplatform/flame/FlameInstanceCreationTask.h"

namespace {

class FailingTask final : public Task {
   public:
    FailingTask() : Task(false) {}

   private:
    void executeTask() override
    {
        QTimer::singleShot(10, this, [this] { emitFailed(QStringLiteral("download failed")); });
    }
};

class WaitingTask final : public Task {
   public:
    WaitingTask() : Task(false) {}

    int startCount() const { return m_startCount; }
    int abortCount() const { return m_abortCount; }
    bool canAbort() const override { return true; }
    bool abort() override
    {
        if (isRunning()) {
            m_abortCount++;
            emitAborted();
        }
        return true;
    }

   private:
    void executeTask() override { m_startCount++; }

    int m_startCount = 0;
    int m_abortCount = 0;
};

class QueuedTask final : public Task {
   public:
    QueuedTask() : Task(false) {}

    int startCount() const { return m_startCount; }
    bool canAbort() const override { return true; }

   private:
    void executeTask() override
    {
        m_startCount++;
        emitSucceeded();
    }

    int m_startCount = 0;
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

    void failedServerPackAbortsRemainingModDownloads()
    {
        Net::HostScheduler scheduler;
        QNetworkAccessManager network;
        NetJob downloadJob(QStringLiteral("Fail-fast modpack download"), &network, 2, &scheduler);
        InstallTask installTask;
        auto serverPack = makeShared<FailingTask>();
        auto activeDownload = makeShared<WaitingTask>();
        downloadJob.addTask(serverPack);
        downloadJob.addTask(activeDownload);

        QVector<shared_qobject_ptr<QueuedTask>> queuedDownloads;
        for (int i = 0; i < 3; i++) {
            auto download = makeShared<QueuedTask>();
            queuedDownloads.append(download);
            downloadJob.addTask(download);
        }

        bool serverPackFailureHandled = false;
        int serverPackProcessingCalls = 0;
        QSignalSpy installFailed(&installTask, &Task::failed);
        QSignalSpy jobAborted(&downloadJob, &Task::aborted);
        QSignalSpy jobFailed(&downloadJob, &Task::failed);

        installTask.start();
        Flame::Internal::connectDownloadJobCompletion(
            &downloadJob, &installTask,
            [&] { serverPackProcessingCalls++; },
            [&](QString reason) {
                if (!serverPackFailureHandled) {
                    serverPackFailureHandled = true;
                    installTask.fail(reason);
                }
            },
            [] {});
        Flame::Internal::abortDownloadJobOnTaskFailure(
            &downloadJob, serverPack.get(), &installTask, [&](QString reason) {
                if (serverPackFailureHandled)
                    return;
                serverPackFailureHandled = true;
                installTask.fail(reason);
            });

        downloadJob.start();
        QTRY_COMPARE_WITH_TIMEOUT(activeDownload->startCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(installFailed.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(jobAborted.count(), 1, 5000);

        QCOMPARE(installTask.failureCalls(), 1);
        QCOMPARE(installTask.failReason(), QStringLiteral("download failed"));
        QCOMPARE(activeDownload->abortCount(), 1);
        QCOMPARE(serverPackProcessingCalls, 0);
        QCOMPARE(jobFailed.count(), 0);
        for (const auto& download : queuedDownloads)
            QCOMPARE(download->startCount(), 0);
    }

    void failedServerPackStopsQueuedDownloadsWhenNothingElseIsActive()
    {
        Net::HostScheduler scheduler;
        QNetworkAccessManager network;
        NetJob downloadJob(QStringLiteral("Queued fail-fast modpack download"), &network, 1, &scheduler);
        InstallTask installTask;
        auto serverPack = makeShared<FailingTask>();
        downloadJob.addTask(serverPack);

        QVector<shared_qobject_ptr<QueuedTask>> queuedDownloads;
        for (int i = 0; i < 2; i++) {
            auto download = makeShared<QueuedTask>();
            queuedDownloads.append(download);
            downloadJob.addTask(download);
        }

        bool serverPackFailureHandled = false;
        QSignalSpy installFailed(&installTask, &Task::failed);
        QSignalSpy jobAborted(&downloadJob, &Task::aborted);
        installTask.start();
        Flame::Internal::connectDownloadJobCompletion(
            &downloadJob, &installTask, [] {},
            [&](QString reason) {
                if (!serverPackFailureHandled) {
                    serverPackFailureHandled = true;
                    installTask.fail(reason);
                }
            },
            [] {});
        Flame::Internal::abortDownloadJobOnTaskFailure(
            &downloadJob, serverPack.get(), &installTask, [&](QString reason) {
                if (serverPackFailureHandled)
                    return;
                serverPackFailureHandled = true;
                installTask.fail(reason);
            });

        downloadJob.start();
        QTRY_COMPARE_WITH_TIMEOUT(installFailed.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(jobAborted.count(), 1, 5000);

        QCOMPARE(installTask.failureCalls(), 1);
        QCOMPARE(installTask.failReason(), QStringLiteral("download failed"));
        for (const auto& download : queuedDownloads)
            QCOMPARE(download->startCount(), 0);
    }
};

QTEST_GUILESS_MAIN(FlameDownloadCompletionTest)

#include "FlameDownloadCompletion_test.moc"
