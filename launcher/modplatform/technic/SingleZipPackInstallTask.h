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

#pragma once

#include "InstanceTask.h"
#include "net/NetJob.h"

#include <QFutureWatcher>
#include <QUrl>

namespace Technic {

class SingleZipPackInstallTask : public InstanceTask {
    Q_OBJECT

   public:
    SingleZipPackInstallTask(const QUrl& sourceUrl, const QString& minecraftVersion, const QUrl& serverPackUrl = {});

    bool canAbort() const override { return true; }
    bool abort() override;

   protected:
    void executeTask() override;

   private slots:
    void downloadSucceeded();
    void downloadFailed(QString reason);
    void downloadProgressChanged(qint64 current, qint64 total);
    void extractFinished();
    void extractAborted();

   private:
    bool m_abortable = false;

    QUrl m_sourceUrl;
    QUrl m_serverPackUrl;
    QString m_minecraftVersion;
    QString m_archivePath;
    QString m_serverArchivePath;
    NetJob::Ptr m_filesNetJob;
    QFuture<QString> m_extractFuture;
    QFutureWatcher<QString> m_extractFutureWatcher;
};

}  // namespace Technic
