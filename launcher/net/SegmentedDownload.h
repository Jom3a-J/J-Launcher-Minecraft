// SPDX-License-Identifier: GPL-3.0-only
/*
 *  J Launcher - Minecraft Launcher
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
 */

#pragma once

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>
#include <vector>

#include "net/NetJob.h"
#include "tasks/Task.h"

namespace Net {

class PartFile;
class Validator;
struct SegmentState;
struct ModrinthDownloadMeta;

/*! Downloads one large file over several concurrent HTTP Range requests.
 *
 *  Why this is a plain Task and not a NetRequest: the outer NetJob must count the whole file as
 *  a single unit of work (so a 187 file job stays a 187 file job), while the requests that
 *  actually move bytes have to be admitted one by one. A plain Task is taken by
 *  NetJob::takeNextSubTask() without a permit, and the segments live in an inner NetJob that
 *  shares the caller's HostScheduler - so every live segment holds a real permit, and provider
 *  ceilings, FTB pinning, cooldowns, 429/503 handling, redirect permit migration and abort all
 *  keep working without a line of policy being reimplemented here.
 *
 *  Shape of a transfer:
 *
 *    1. Discovery - one GET with "Range: bytes=0-1048575" and "Accept-Encoding: identity".
 *       A 200 means the server ignored the range: that same reply keeps streaming the whole file
 *       into the part file and becomes the ordinary single stream download. If the 403/404
 *       fallback GET is redirected to a large range-capable CurseForge file host, that probe is
 *       retired and one ranged Discovery is started against the resolved URL.
 *       A 206 with a concrete Content-Range tells us the total size and that ranges work.
 *    2. Fan out - the remainder is split into K segments, each an ordinary Net::Download in an
 *       inner NetJob. K is bounded by the caller's setting, by MaxSegments, by MinSegmentSize and
 *       by half the host's provider ceiling, so a segmented file can never take the whole pool.
 *    3. Assemble - the caller's validators are replayed over the finished part file off the GUI
 *       thread, which validates the bytes that are actually on disk rather than the bytes that
 *       went past on the wire.
 *    4. Promote - PartFile::promote() renames the part file over the target. The target path
 *       never exists until the bytes are complete and validated.
 *
 *  Anything the server does that we cannot verify - an unparsable Content-Range, an unknown
 *  total, a content coding that would make byte offsets meaningless, a 416, or the entity
 *  changing under us - either degrades to a single unranged stream or fails closed. A 403 or 404
 *  during Discovery also retries once without Range because some CDNs reject ranged URLs at their
 *  edge host; the ordinary request still decides whether the file is available. It never promotes
 *  a partially correct file.
 */
class SegmentedDownload : public Task {
    Q_OBJECT

   public:
    using Ptr = shared_qobject_ptr<SegmentedDownload>;
    /// Applied to every request this task issues, so caller supplied header proxies survive.
    using Decorator = std::function<void(Net::NetRequest&)>;

    /// Files smaller than this are never split.
    static constexpr qint64 MinSegmentedSize = 32LL * 1024 * 1024;
    /// No segment is ever smaller than this, so a small file cannot spawn many tiny requests.
    static constexpr qint64 MinSegmentSize = 8LL * 1024 * 1024;
    /// How much the discovery request asks for. It is also the first part of the file.
    static constexpr qint64 DiscoveryChunk = 1LL * 1024 * 1024;
    /// Per reply read buffer, so N segments cannot queue an unbounded amount of memory.
    static constexpr qint64 ReadBufferBytes = 1LL * 1024 * 1024;
    /// Larger buffer for opted-in HTTP/1 CDN replies, where a small buffer can stall the socket.
    static constexpr qint64 CdnHttp1ReadBufferBytes = 4LL * 1024 * 1024;
    /// Chunk size of the final validator replay.
    static constexpr qint64 ValidationChunkBytes = 1LL * 1024 * 1024;
    /// The default of the "SegmentedDownloadSegments" setting.
    static constexpr int DefaultSegments = 4;
    /// Hard upper bound, whatever the setting says.
    static constexpr int MaxSegments = 8;

    /*! Prefer the factories below; this is public only so makeShared() can reach it. */
    SegmentedDownload(QUrl url, QString path, QNetworkAccessManager* network, Net::HostScheduler* scheduler, int maxSegments);
    ~SegmentedDownload() override;

    /*! \param maxSegments the user's "SegmentedDownloadSegments" setting; 0 or 1 disables
     *         segmentation entirely and the task behaves exactly like Net::Download::makeFile.
     *  \param scheduler admission control to use; nullptr means the process wide one.
     */
    static Ptr makeFile(QUrl url,
                        QString path,
                        QNetworkAccessManager* network,
                        Net::HostScheduler* scheduler = nullptr,
                        int maxSegments = DefaultSegments);

    /// makeFile() plus the ApiHeaderProxy every Net::ApiDownload request carries.
    static Ptr makeApiFile(QUrl url,
                           QString path,
                           QNetworkAccessManager* network,
                           Net::HostScheduler* scheduler = nullptr,
                           int maxSegments = DefaultSegments);

    /*! makeApiFile() with Modrinth download metadata, exactly as Net::ApiDownload::makeFile()
     *  attaches it.
     *
     *  The metadata rides on the same ApiHeaderProxy an ordinary request uses, so it reaches the
     *  discovery request and every segment, and - because that proxy scopes the header to the
     *  official download host - a redirect to anywhere else is not told anything.
     */
    static Ptr makeApiFile(QUrl url,
                           QString path,
                           QNetworkAccessManager* network,
                           Net::HostScheduler* scheduler,
                           int maxSegments,
                           const ModrinthDownloadMeta& meta);

    /// Takes ownership, exactly like Net::Sink::addValidator().
    void addValidator(Net::Validator* validator);

    /*! Applied to every request this task issues.
     *
     *  This is how header proxies reach the individual segments; makeApiFile() uses it to attach
     *  the same ApiHeaderProxy an ordinary Net::ApiDownload carries.
     */
    void setRequestDecorator(Decorator decorator) { m_decorate = std::move(decorator); }

    QUrl url() const { return m_originalUrl; }
    QString targetPath() const { return m_targetPath; }

    bool abort() override;
    bool canAbort() const override { return true; }

    /// How many segments the remainder was split into; 0 while no fan out has happened.
    int segmentsUsed() const { return m_segmentsUsed; }
    /// True when the transfer ran as a single unsegmented stream for any reason.
    bool ranUnsegmented() const;
    /// The size the server reported, or -1 when it never gave a usable one.
    qint64 contentLength() const { return m_total; }

   protected:
    void executeTask() override;

   private:
    enum class Mode {
        Idle,
        Legacy,     //!< one ordinary Net::Download straight to the target; no part file
        Discovery,  //!< bounded ranged GET that doubles as the no-range fallback
        Streaming,  //!< discovery answered 200 and is downloading the whole file
        Segments,   //!< fanned out over several ranged requests
        Plain,      //!< restarted as a single unranged GET into the part file; may resolve a CDN
        Validating,
        Done,
    };

    int allowedSegments() const;
    int allowedSegments(const QUrl& url) const;
    int segmentCountFor(qint64 total) const;
    int segmentCountFor(qint64 total, const QUrl& url) const;

    void startLegacy();
    void startDiscovery();
    void startSegments(qint64 from);
    void restartUnranged(const QString& why);
    void restartDiscoveryAtResolvedUrl(const QUrl& url);
    void assemble();
    void onValidationFinished();
    void promote();

    void onSegmentHeaders(const std::shared_ptr<SegmentState>& state, QNetworkReply& reply);
    void onDiscoveryHeaders(QNetworkReply& reply);
    /// Sends a segment back to the start of its slice, dropping whatever a previous attempt wrote.
    void rewindToStart(const std::shared_ptr<SegmentState>& state);

    void onJobSucceeded();
    void onJobFailed(const QString& reason);
    void onJobAborted();

    NetJob::Ptr newJob(int maxConcurrent);
    void connectJob();
    void retireJob();
    Net::NetRequest::Ptr makeSegmentRequest(const std::shared_ptr<SegmentState>& state, bool ranged);

    qint64 writtenBytes() const;
    /*! Proves the segments actually tile [0, total) with no gap and no overlap.
     *
     *  The size of the part file says nothing on its own - it was preallocated to the full length
     *  before a single segment ran - so coverage is checked against what the segments reported
     *  writing, and only then against the file.
     */
    bool coversWholeFile(QString* error) const;
    void resetForRestart();
    void publishProgress(bool terminal);
    void failWith(const QString& reason);
    void logOutcome(const char* result) const;

    QUrl m_url;          //!< Active URL, updated when a refused Forge edge redirects to its file host.
    QUrl m_originalUrl;  //!< Caller-supplied URL restored if the task is started again.
    QString m_targetPath;
    QNetworkAccessManager* m_network = nullptr;
    QPointer<Net::HostScheduler> m_scheduler;
    int m_maxSegments = DefaultSegments;
    Decorator m_decorate;

    std::vector<std::shared_ptr<Net::Validator>> m_validators;
    std::shared_ptr<PartFile> m_part;
    std::vector<std::shared_ptr<SegmentState>> m_states;
    std::shared_ptr<SegmentState> m_discovery;

    NetJob::Ptr m_job;
    /*! Jobs that were replaced mid transfer.
     *
     *  They are aborted (which hands their permits back) but kept alive until this task ends, so
     *  a reply that is still unwinding cannot land in a freed job.
     */
    std::vector<NetJob::Ptr> m_retired;

    Mode m_mode = Mode::Idle;
    qint64 m_total = -1;
    QByteArray m_entityValidator;
    bool m_rangesUsable = false;
    bool m_restartPending = false;
    bool m_restarted = false;
    bool m_discoveryRangeRefused = false;  //!< Discovery got a 403/404; only this case probes a redirected file host.
    bool m_redirectRangeRetried = false;   //!< The resolved target gets at most one new ranged Discovery.
    bool m_aborting = false;
    int m_segmentsUsed = 0;

    QTimer m_progressTimer;
    QElapsedTimer m_clock;
    qint64 m_lastProgress = 0;
    qint64 m_lastSpeedBytes = 0;
    qint64 m_lastSpeedMs = 0;

    QFutureWatcher<QString> m_validation;
};

}  // namespace Net
