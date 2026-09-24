// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (c) 2022 flowln <flowlnlnln@gmail.com>
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
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

#include "NetJob.h"
#include <QNetworkReply>
#include "net/NetRequest.h"
#include "logs/Privacy.h"
#include "tasks/ConcurrentTask.h"
#if defined(LAUNCHER_APPLICATION)
#include "Application.h"
#include "settings/SettingsObject.h"
#include "ui/dialogs/NetworkJobFailedDialog.h"
#endif

NetJob::NetJob(QString job_name, QNetworkAccessManager* network, int max_concurrent, Net::HostScheduler* scheduler)
    : ConcurrentTask(job_name), m_network(network), m_scheduler(scheduler ? scheduler : Net::HostScheduler::global())
{
    // Admission is decided per host by the scheduler. The job's own width only has to be wide
    // enough not to be the binding constraint; callers that pass an explicit value still win.
    setMaxConcurrent(max_concurrent > 0 ? max_concurrent : Net::HostScheduler::GlobalCap);

    m_state_flush.setSingleShot(true);
    m_state_flush.setTimerType(Qt::CoarseTimer);
    connect(&m_state_flush, &QTimer::timeout, this, [this] { emitState(m_queue.isEmpty() && m_doing.isEmpty()); });

    connect(m_scheduler, &Net::HostScheduler::capacityAvailable, this, &NetJob::onCapacityAvailable);
}

NetJob::~NetJob()
{
    // Nothing else can report these requests as finished any more.
    releaseAllPermits(Net::HostOutcome::Aborted);
}

auto NetJob::addNetAction(Net::NetRequest::Ptr action) -> bool
{
    action->setNetwork(m_network);

    addTask(action);

    return true;
}

void NetJob::addTask(Task::Ptr task)
{
    ConcurrentTask::addTask(std::move(task));
    // The queue changed, so a previous "nothing is admissible" verdict no longer holds.
    m_admission_blocked = false;
}

void NetJob::executeTask()
{
    // One kick is enough: executeNextSubTask() posts itself again for as long as it keeps being
    // admitted, so there is no need to queue one invocation per concurrency slot.
    QMetaObject::invokeMethod(this, &NetJob::executeNextSubTask, Qt::QueuedConnection);
}

void NetJob::executeNextSubTask()
{
    m_capacity_wakeup_pending = false;

    // We're finished, check for failures and retry if we can (up to 3 times)
    if (isRunning() && m_queue.isEmpty() && m_doing.isEmpty() && !m_failed.isEmpty() && m_try < 3) {
        m_try += 1;
        m_failed.removeIf([this](QHash<Task*, Task::Ptr>::iterator task) {
            auto* request = dynamic_cast<Net::NetRequest*>(task->get());
            // Only requests are retried here. A sub task that is not one has no status code to
            // judge and no promise that starting it a second time is even meaningful, so leave it
            // failed rather than restarting something arbitrary; such a task is expected to do its
            // own retrying (Net::SegmentedDownload retries its segments in its inner job).
            if (!request) {
                return false;
            }
            // there is no point in retying on 404 Not Found
            if (request->replyStatusCode() == 404) {
                return false;
            }
            m_done.remove(task->get());
            m_queue.enqueue(*task);
            return true;
        });
    }

    const auto doing_before = m_doing.count();
    ConcurrentTask::executeNextSubTask();

    // One wake-up can cover several freed permits, so keep filling the pipeline until admission
    // stops granting or the queue runs dry.
    if (m_doing.count() > doing_before && !m_queue.isEmpty()) {
        QMetaObject::invokeMethod(this, &NetJob::executeNextSubTask, Qt::QueuedConnection);
    }
}

Task::Ptr NetJob::takeNextSubTask()
{
    if (!m_scheduler) {
        // The injected scheduler outlived its usefulness; fall back to plain FIFO rather than
        // stalling the job forever.
        return ConcurrentTask::takeNextSubTask();
    }

    // A queue of thousands of files is normal, so do not rescan it while the scheduler is known
    // to have nothing for us. The flag is cleared whenever capacity or the queue changes, and it
    // is ignored while nothing is running so a missed wake-up can never stall the job for good.
    if (m_admission_blocked && !m_doing.isEmpty())
        return nullptr;

    QSet<QString> refusedHosts;
    for (int i = 0; i < m_queue.size(); i++) {
        auto* request = dynamic_cast<Net::NetRequest*>(m_queue.at(i).get());
        if (!request) {
            // Not a network request, so there is nothing to admit: run it like ConcurrentTask would.
            m_admission_blocked = false;
            return m_queue.takeAt(i);
        }

        const QUrl url = request->url();
        // One refusal per host is enough; the answer cannot change mid-scan.
        if (refusedHosts.contains(Net::HostScheduler::hostKey(url)))
            continue;

        const auto permit = m_scheduler->tryAcquire(url, request->isLatencyCritical());
        if (permit == Net::HostScheduler::InvalidPermit) {
            // Skip past saturated or cooling down hosts instead of letting the head of the queue
            // stall requests to hosts that still have capacity.
            refusedHosts.insert(Net::HostScheduler::hostKey(url));
            continue;
        }

        auto task = m_queue.takeAt(i);
        m_permits.insert(task.get(), permit);
        m_admission_blocked = false;

        // Both connections are torn down again by ConcurrentTask::subTaskFinished(), which
        // disconnects everything from the request to this job.
        auto* admitted = task.get();
        connect(request, &Net::NetRequest::rateLimited, this, [this](const QUrl& url, qint64 retryAfterSeconds) {
            if (m_scheduler)
                m_scheduler->reportRateLimited(url, retryAfterSeconds);
        });
        connect(request, &Net::NetRequest::redirectedToNewHost, this, [this, admitted](const QUrl& url) {
            const auto held = m_permits.value(admitted, Net::HostScheduler::InvalidPermit);
            if (held != Net::HostScheduler::InvalidPermit && m_scheduler)
                m_scheduler->migratePermit(held, url);
        });

        return task;
    }

    // Nothing admissible right now; the requests stay queued, so the denominator is unchanged.
    m_admission_blocked = true;
    return nullptr;
}

void NetJob::onCapacityAvailable()
{
    m_admission_blocked = false;

    if (m_capacity_wakeup_pending || !isRunning() || m_queue.isEmpty())
        return;
    if (m_doing.count() >= m_total_max_size)
        return;

    m_capacity_wakeup_pending = true;
    QMetaObject::invokeMethod(this, &NetJob::executeNextSubTask, Qt::QueuedConnection);
}

Net::HostOutcome NetJob::outcomeFor(Task* task, TaskStepState state)
{
    if (state == TaskStepState::Succeeded)
        return Net::HostOutcome::Success;

    auto* request = dynamic_cast<Net::NetRequest*>(task);
    if (!request)
        return Net::HostOutcome::Failure;

    // 429/503 is reported through NetRequest::rateLimited() the moment it is seen, so it is
    // deliberately not classified again here: doing both would penalise the host twice.
    switch (request->error()) {
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::ConnectionRefusedError:
            return Net::HostOutcome::ConnectionReset;
        case QNetworkReply::OperationCanceledError:
            return Net::HostOutcome::Aborted;
        default:
            return Net::HostOutcome::Failure;
    }
}

void NetJob::subTaskFinished(Task::Ptr task, TaskStepState state)
{
    releasePermit(task.get(), outcomeFor(task.get(), state));

    ConcurrentTask::subTaskFinished(task, state);
}

void NetJob::releasePermit(Task* task, Net::HostOutcome outcome)
{
    const auto permit = m_permits.take(task);
    if (permit == Net::HostScheduler::InvalidPermit)
        return;  // never held one, or it was already given back
    if (m_scheduler)
        m_scheduler->release(permit, outcome);
}

void NetJob::releaseAllPermits(Net::HostOutcome outcome)
{
    const auto permits = m_permits;
    m_permits.clear();
    if (!m_scheduler)
        return;
    for (const auto permit : permits) {
        m_scheduler->release(permit, outcome);
    }
}

auto NetJob::size() const -> int
{
    return m_queue.size() + m_doing.size() + m_done.size();
}

auto NetJob::canAbort() const -> bool
{
    bool canFullyAbort = true;

    // can abort the downloads on the queue?
    for (auto part : m_queue)
        canFullyAbort &= part->canAbort();

    // can abort the active downloads?
    for (auto part : m_doing)
        canFullyAbort &= part->canAbort();

    return canFullyAbort;
}

auto NetJob::abort() -> bool
{
    // fail all downloads on the queue
    for (auto task : m_queue)
        m_failed.insert(task.get(), task);
    m_queue.clear();

    if (m_doing.isEmpty()) {
        // The queue may have held all remaining work; close the job instead of leaving it running
        // with its queue cleared and no active task left to report completion.
        if (isRunning())
            emitAborted();
        return true;
    }

    bool fullyAborted = true;

    // abort active downloads
    auto toKill = m_doing.values();
    for (auto part : toKill) {
        fullyAborted &= part->abort();
    }

    // Requests that reported back during the loop above already gave their permit back; hand back
    // whatever is left so that an abort can never leak capacity.
    releaseAllPermits(Net::HostOutcome::Aborted);

    if (fullyAborted)
        emitAborted();
    else
        emitFailed(tr("Failed to abort all tasks in the NetJob!"));

    return fullyAborted;
}

auto NetJob::getFailedActions() -> QList<Net::NetRequest*>
{
    QList<Net::NetRequest*> failed;
    for (auto index : m_failed) {
        // A job may hold sub tasks that are not requests; they have no reply to report on.
        if (auto* request = dynamic_cast<Net::NetRequest*>(index.get())) {
            failed.push_back(request);
        }
    }
    return failed;
}

auto NetJob::getFailedFiles() -> QList<QString>
{
    QList<QString> failed;
    for (auto index : m_failed) {
        if (auto* request = dynamic_cast<Net::NetRequest*>(index.get())) {
            failed.append(Privacy::sanitizeUrl(request->url()));
        } else {
            failed.append(index->objectName());
        }
    }
    return failed;
}

void NetJob::updateState()
{
    // A terminal state is always published exactly. Everything in between is coalesced to
    // StateUpdateIntervalMs: a job with hundreds of requests otherwise spends its time rebuilding
    // the same status string.
    if (m_queue.isEmpty() && m_doing.isEmpty()) {
        emitState(true);
        return;
    }

    if (!m_state_clock.isValid() || m_state_clock.elapsed() >= StateUpdateIntervalMs) {
        emitState(false);
        return;
    }

    if (!m_state_flush.isActive())
        m_state_flush.start(static_cast<int>(StateUpdateIntervalMs - m_state_clock.elapsed()));
}

void NetJob::emitState(bool terminal)
{
    m_state_flush.stop();
    m_state_clock.start();

    const qint64 total = totalSize();
    qint64 current = m_done.count();
    if (terminal) {
        // The exact value wins, even when a retry pass pushed finished requests back into the
        // queue and the count therefore went down.
        m_last_reported_progress = current;
    } else {
        // Intermediate updates never go backwards: a retry pass makes progress plateau rather
        // than jump back.
        current = qMax(current, m_last_reported_progress);
        m_last_reported_progress = current;
    }

    setProgress(current, total);
    setStatus(tr("Executing %1 task(s) (%2 out of %3 are done)")
                  .arg(QString::number(m_doing.count()), QString::number(current), QString::number(total)));
}

bool NetJob::isOnline()
{
    // check some errors that are ussually associated with the lack of internet
    for (auto job : getFailedActions()) {
        auto err = job->error();
        if (err != QNetworkReply::HostNotFoundError && err != QNetworkReply::NetworkSessionFailedError) {
            return true;
        }
    }
    return false;
};

void NetJob::emitFailed(QString reason)
{
#if defined(LAUNCHER_APPLICATION)

    if (APPLICATION_DYN && m_ask_retry && m_manual_try < APPLICATION->settings()->get("NumberOfManualRetries").toInt() && isOnline()) {
        m_manual_try++;
        auto failed = getFailedActions();
        auto dialog = new NetworkJobFailedDialog(objectName(), m_try, m_done.size(), failed.size(), nullptr);
        dialog->setAttribute(Qt::WA_DeleteOnClose);

        for (const auto& request : failed) {
            dialog->addFailedRequest(request->url(), request->errorString());
        }

        dialog->open();

        connect(dialog, &QDialog::finished, this, [this, reason = std::move(reason)](int result) {
            if (result == QDialog::Accepted) {
                m_try = 0;
                executeNextSubTask();
            } else {
                ConcurrentTask::emitFailed(reason);
            }
        });

        return;
    }
#endif

    ConcurrentTask::emitFailed(reason);
}

void NetJob::setAskRetry(bool askRetry)
{
    m_ask_retry = askRetry;
}
