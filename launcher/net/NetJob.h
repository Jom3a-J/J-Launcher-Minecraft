// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (c) 2022 flowln <flowlnlnln@gmail.com>
 *  Copyright (C) 2023 Rachel Powers <508861+Ryex@users.noreply.github.com>
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

#pragma once

#include <QtNetwork>

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include "net/HostScheduler.h"
#include "net/NetRequest.h"
#include "tasks/ConcurrentTask.h"

// Those are included so that they are also included by anyone using NetJob
#include "net/Download.h"
#include "net/HttpMetaCache.h"

/*! A group of network requests.
 *
 *  How many of them actually run at once is decided by a Net::HostScheduler that is shared with
 *  every other NetJob in the process, so provider limits hold across simultaneous jobs. The
 *  job's own max concurrency is only an upper bound a caller can tighten (FTB, for instance,
 *  pins its job to one request).
 */
class NetJob : public ConcurrentTask {
    Q_OBJECT

   public:
    // TODO: delete
    using Ptr = shared_qobject_ptr<NetJob>;

    /*! \param max_concurrent upper bound on requests this job runs at once; negative means
     *         "let the scheduler decide".
     *  \param scheduler admission control to use; nullptr means the process wide one.
     */
    explicit NetJob(QString job_name,
                    QNetworkAccessManager* network,
                    int max_concurrent = -1,
                    Net::HostScheduler* scheduler = nullptr);
    ~NetJob() override;

    auto size() const -> int;

    auto canAbort() const -> bool override;
    auto addNetAction(Net::NetRequest::Ptr action) -> bool;

    auto getFailedActions() -> QList<Net::NetRequest*>;
    auto getFailedFiles() -> QList<QString>;
    void setAskRetry(bool askRetry);

    Net::HostScheduler* scheduler() const { return m_scheduler.data(); }

   public slots:
    // Qt can't handle auto at the start for some reason?
    bool abort() override;
    void emitFailed(QString reason) override;

   protected slots:
    void executeTask() override;
    void executeNextSubTask() override;
    void subTaskFinished(Task::Ptr task, TaskStepState state) override;

   protected:
    void updateState() override;
    Task::Ptr takeNextSubTask() override;
    bool isOnline();

   private:
    void onCapacityAvailable();
    void releasePermit(Task* task, Net::HostOutcome outcome);
    void releaseAllPermits(Net::HostOutcome outcome);
    static Net::HostOutcome outcomeFor(Task* task, TaskStepState state);
    void emitState(bool terminal);

    QNetworkAccessManager* m_network;
    /*! The process wide scheduler is never destroyed, but an injected one can be. A guarded
     *  pointer keeps a job that outlives its scheduler from releasing permits into freed memory. */
    QPointer<Net::HostScheduler> m_scheduler;

    /// Permits held by the requests currently in m_doing, keyed by request.
    QHash<Task*, Net::HostScheduler::Permit> m_permits;
    bool m_capacity_wakeup_pending = false;
    /// Set when a full pass over the queue found nothing admissible; avoids rescanning a large
    /// queue until either the scheduler or the queue has changed.
    bool m_admission_blocked = false;

    /// Aggregate progress and status are coalesced to this rate; terminal updates are exact.
    static constexpr int StateUpdateIntervalMs = 100;
    QElapsedTimer m_state_clock;
    QTimer m_state_flush;
    qint64 m_last_reported_progress = 0;

    int m_try = 1;
    bool m_ask_retry = true;
    int m_manual_try = 0;
};
