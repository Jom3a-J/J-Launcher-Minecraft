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

#include "net/SegmentedDownload.h"

#include <QFile>
#include <QNetworkReply>
#include <QtConcurrent>
#include <algorithm>

#include "MMCTime.h"
#include "StringUtils.h"
#include "logs/Privacy.h"
#include "net/Download.h"
#include "net/HeaderProxy.h"
#include "net/Logging.h"
#include "net/PartFile.h"
#include "net/Sink.h"
#include "net/Validator.h"

#if defined(LAUNCHER_APPLICATION)
#include "net/ApiHeaderProxy.h"
#endif

namespace Net {

/*! Everything one request of a segmented download needs to know about its slice of the file.
 *
 *  Shared between the coordinator, the request's sink and its header proxy, so a retry can pick
 *  up where the previous attempt stopped without any of them holding a raw back pointer.
 */
struct SegmentState {
    qint64 start = 0;   //!< first byte of the slice; fixed for the life of the segment
    qint64 end = -1;    //!< last byte of the slice, inclusive; -1 means "until the server stops"
    qint64 cursor = 0;  //!< next byte offset to write; also where a retry resumes from

    bool headersSeen = false;  //!< the current attempt has had its response headers inspected
    bool accepted = false;     //!< ...and they were acceptable, so the body may be written

    bool dropWrites = false;  //!< the coordinator gave up on this attempt; swallow the rest
    QString hardError;        //!< a protocol violation; retrying cannot help

    qint64 done() const { return cursor - start; }
    bool complete() const { return end >= 0 && cursor == end + 1; }
};

namespace {

/*! Lets a validator owned elsewhere be handed to a Sink without transferring ownership.
 *
 *  Sink::addValidator() takes a raw pointer and wraps it in a shared_ptr of its own, which would
 *  double own a validator the coordinator already holds. The adapter is the thing the sink owns.
 */
class BorrowedValidator final : public Validator {
   public:
    explicit BorrowedValidator(std::shared_ptr<Validator> inner) : m_inner(std::move(inner)) {}

    bool init(QNetworkRequest& request) override { return m_inner->init(request); }
    bool write(QByteArray& data) override { return m_inner->write(data); }
    bool abort() override { return m_inner->abort(); }
    bool validate(QNetworkReply& reply) override { return m_inner->validate(reply); }

   private:
    std::shared_ptr<Validator> m_inner;
};

/*! A finished, bodiless reply describing the assembled file.
 *
 *  Validator::validate() takes a QNetworkReply, but a segmented download has no single reply that
 *  covers the whole entity - and holding on to one of the segment replies to pass here would mean
 *  reaching into an object whose owner has already moved on. This is the stable stand-in: it
 *  carries the URL and length of what was actually validated and nothing else.
 */
class AssembledEntityReply final : public QNetworkReply {
   public:
    AssembledEntityReply(const QUrl& url, qint64 length, QObject* parent = nullptr) : QNetworkReply(parent)
    {
        setRequest(QNetworkRequest(url));
        setUrl(url);
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        if (length >= 0)
            setHeader(QNetworkRequest::ContentLengthHeader, length);
        setOpenMode(QIODevice::ReadOnly);
        setFinished(true);
    }

    void abort() override {}

   protected:
    qint64 readData(char*, qint64) override { return -1; }
};

/*! Emits the Range family of headers from the segment's live cursor.
 *
 *  NetRequest re-runs the header proxies on every attempt, so a retry automatically asks for what
 *  is still missing rather than for the range the first attempt asked for.
 */
class RangeHeaderProxy final : public HeaderProxy {
   public:
    RangeHeaderProxy(std::shared_ptr<SegmentState> state, QByteArray entityValidator)
        : m_state(std::move(state)), m_entityValidator(std::move(entityValidator))
    {}

    QList<HeaderPair> headers(const QNetworkRequest&) const override
    {
        QByteArray range = "bytes=" + QByteArray::number(m_state->cursor) + "-";
        if (m_state->end >= 0)
            range += QByteArray::number(m_state->end);

        QList<HeaderPair> result;
        result.append({ .headerName = "Range", .headerValue = range });
        // Qt decodes Content-Encoding transparently, which would make the byte offsets in
        // Content-Range refer to something other than what the sink receives. Ask for none.
        result.append({ .headerName = "Accept-Encoding", .headerValue = "identity" });
        if (!m_entityValidator.isEmpty())
            result.append({ .headerName = "If-Range", .headerValue = m_entityValidator });
        return result;
    }

   private:
    std::shared_ptr<SegmentState> m_state;
    QByteArray m_entityValidator;
};

/*! Writes one segment's body into the shared part file at the segment's own offset.
 *
 *  No validators are attached here on purpose: segments arrive out of order, so the whole file
 *  hash is computed once over the assembled part file instead of being streamed.
 */
class SegmentSink final : public Sink {
   public:
    SegmentSink(std::shared_ptr<PartFile> file, std::shared_ptr<SegmentState> state)
        : m_file(std::move(file)), m_state(std::move(state))
    {}

    Task::State init(QNetworkRequest&) override
    {
        if (!m_state->hardError.isEmpty()) {
            // A protocol violation cannot be retried away; fail before touching the network.
            m_fail_reason = m_state->hardError;
            return Task::State::Failed;
        }
        m_state->headersSeen = false;
        m_state->accepted = false;
        return Task::State::Running;
    }

    Task::State write(QByteArray& data) override
    {
        if (m_state->dropWrites)
            return Task::State::Running;
        if (data.isEmpty())
            return Task::State::Running;

        if (!m_state->accepted) {
            m_fail_reason = m_state->hardError.isEmpty()
                                ? QObject::tr("The server sent a response body the launcher cannot place in the file.")
                                : m_state->hardError;
            return Task::State::Failed;
        }
        if (m_state->end >= 0 && m_state->cursor + data.size() - 1 > m_state->end) {
            m_state->hardError = QObject::tr("The server sent more data than the requested range.");
            m_fail_reason = m_state->hardError;
            return Task::State::Failed;
        }

        QString error;
        if (!m_file->writeAt(m_state->cursor, data, &error)) {
            m_fail_reason = error;
            return Task::State::Failed;
        }
        m_state->cursor += data.size();
        return Task::State::Running;
    }

    Task::State abort() override
    {
        failAllValidators();
        return Task::State::Failed;
    }

    Task::State finalize(QNetworkReply&) override
    {
        if (m_state->dropWrites)
            return Task::State::Succeeded;
        if (!m_state->accepted) {
            m_fail_reason = m_state->hardError.isEmpty() ? QObject::tr("The server did not answer with the requested range.")
                                                         : m_state->hardError;
            return Task::State::Failed;
        }
        if (m_state->end >= 0 && !m_state->complete()) {
            m_fail_reason = QObject::tr("The server closed the connection before the requested range was complete.");
            return Task::State::Failed;
        }
        return Task::State::Succeeded;
    }

    bool hasLocalData() override { return false; }

   private:
    std::shared_ptr<PartFile> m_file;
    std::shared_ptr<SegmentState> m_state;
};

/*! One request of a segmented download.
 *
 *  An ordinary Net::Download in every way that matters to NetJob and to HostScheduler - it is a
 *  Net::NetRequest, so it is admitted with a real permit, reports 429/503, migrates its permit
 *  across a cross host redirect and keeps all of NetRequest's redirect and credential checks.
 */
class SegmentRequest final : public Download {
   public:
    using HeaderCallback = std::function<void(QNetworkReply&)>;

    SegmentRequest(QUrl url, std::shared_ptr<PartFile> file, std::shared_ptr<SegmentState> state)
    {
        m_url = std::move(url);
        setObjectName(QStringLiteral("SEGMENT:") + Privacy::sanitizeUrl(m_url));
        m_sink = std::make_unique<SegmentSink>(std::move(file), std::move(state));
    }

    void setHeaderCallback(HeaderCallback callback) { m_onHeaders = std::move(callback); }

   protected:
    QNetworkReply* getReply(QNetworkRequest& request) override
    {
        auto* reply = Download::getReply(request);
        if (!reply)
            return nullptr;
        // Bound per-reply buffering; the opted-in HTTP/1 CDN path needs more room if the GUI
        // thread misses an event-loop turn, while other requests keep the existing 1 MiB cap.
        const qint64 readBufferBytes = m_cdnHttp1PolicyApplied ? SegmentedDownload::CdnHttp1ReadBufferBytes
                                                              : SegmentedDownload::ReadBufferBytes;
        reply->setReadBufferSize(readBufferBytes);
        QObject::connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply]() {
            if (m_onHeaders)
                m_onHeaders(*reply);
        });
        return reply;
    }

   private:
    HeaderCallback m_onHeaders;
};

int replyStatus(QNetworkReply& reply)
{
    return reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

bool hasTransformingContentEncoding(QNetworkReply& reply)
{
    const QByteArray encoding = reply.rawHeader("Content-Encoding").trimmed().toLower();
    return !encoding.isEmpty() && encoding != "identity";
}

bool isMultipartRange(QNetworkReply& reply)
{
    return reply.header(QNetworkRequest::ContentTypeHeader).toString().trimmed().startsWith(QStringLiteral("multipart/byteranges"),
                                                                                            Qt::CaseInsensitive);
}

/*! Parses "bytes <first>-<last>/<complete-length>".
 *
 *  Deliberately strict: "bytes * /<len>" and an unknown complete length are both rejected,
 *  because without a concrete total there is nothing safe to split.
 */
bool parseContentRange(const QByteArray& raw, qint64* first, qint64* last, qint64* total)
{
    const QByteArray value = raw.trimmed();
    if (!value.startsWith("bytes"))
        return false;

    const int slash = value.lastIndexOf('/');
    const int dash = value.indexOf('-');
    if (slash < 0 || dash < 0 || dash > slash)
        return false;

    bool okFirst = false;
    bool okLast = false;
    bool okTotal = false;
    const qint64 parsedFirst = value.mid(5, dash - 5).trimmed().toLongLong(&okFirst);
    const qint64 parsedLast = value.mid(dash + 1, slash - dash - 1).trimmed().toLongLong(&okLast);
    const qint64 parsedTotal = value.mid(slash + 1).trimmed().toLongLong(&okTotal);

    if (!okFirst || !okLast || !okTotal)
        return false;
    if (parsedFirst < 0 || parsedLast < parsedFirst || parsedTotal <= 0 || parsedLast >= parsedTotal)
        return false;

    *first = parsedFirst;
    *last = parsedLast;
    *total = parsedTotal;
    return true;
}

/*! The strongest cache validator the response offers, or empty when there is none worth using.
 *
 *  A weak ETag is deliberately ignored: If-Range is only meaningful with a strong validator, and
 *  guessing wrong here means assembling two different versions of a file into one.
 */
QByteArray entityValidatorOf(QNetworkReply& reply)
{
    const QByteArray etag = reply.rawHeader("ETag").trimmed();
    if (etag.startsWith('"') && etag.endsWith('"') && etag.size() >= 2)
        return etag;

    const QByteArray lastModified = reply.rawHeader("Last-Modified").trimmed();
    if (!lastModified.isEmpty())
        return lastModified;

    return {};
}

/*! Feeds the finished part file through \a validators. Runs on a worker thread.
 *
 *  Returns an empty string on success, or the reason it could not be done.
 */
QString replayValidators(QString path, QUrl url, std::vector<std::shared_ptr<Validator>> validators)
{
    QNetworkRequest request(url);
    for (const auto& validator : validators) {
        if (!validator->init(request))
            return QObject::tr("Failed to initialize validators");
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QObject::tr("Could not read the finished download back: %1").arg(file.errorString());
    }

    while (!file.atEnd()) {
        QByteArray chunk = file.read(SegmentedDownload::ValidationChunkBytes);
        if (chunk.isEmpty()) {
            if (file.error() != QFileDevice::NoError)
                return QObject::tr("Could not read the finished download back: %1").arg(file.errorString());
            break;
        }
        for (const auto& validator : validators) {
            if (!validator->write(chunk))
                return QObject::tr("Failed to write validators");
        }
    }
    return {};
}

}  // namespace

SegmentedDownload::SegmentedDownload(QUrl url,
                                     QString path,
                                     QNetworkAccessManager* network,
                                     HostScheduler* scheduler,
                                     int maxSegments)
    : Task()
    , m_url(std::move(url))
    , m_targetPath(std::move(path))
    , m_network(network)
    , m_scheduler(scheduler ? scheduler : HostScheduler::global())
    , m_maxSegments(maxSegments)
{
    setObjectName(QStringLiteral("SEGMENTED:") + Privacy::sanitizeUrl(m_url));

    m_progressTimer.setTimerType(Qt::CoarseTimer);
    m_progressTimer.setInterval(100);
    connect(&m_progressTimer, &QTimer::timeout, this, [this] { publishProgress(false); });
    connect(&m_validation, &QFutureWatcher<QString>::finished, this, &SegmentedDownload::onValidationFinished);
}

SegmentedDownload::~SegmentedDownload()
{
    m_progressTimer.stop();
    // Destruction may race a reply that is already unwinding. Explicit abort() handles user
    // cancellation; here detach the current job without issuing a second QNetworkReply::abort()
    // against a reply whose lifetime may already be ending.
    if (m_job) {
        auto job = m_job;
        m_job.reset();
        disconnect(job.get(), nullptr, this, nullptr);
        m_retired.push_back(job);
    }
    for (const auto& job : m_retired) {
        disconnect(job.get(), nullptr, this, nullptr);
    }
    if (m_validation.isRunning())
        m_validation.waitForFinished();
}

auto SegmentedDownload::makeFile(QUrl url, QString path, QNetworkAccessManager* network, HostScheduler* scheduler, int maxSegments)
    -> Ptr
{
    return makeShared<SegmentedDownload>(std::move(url), std::move(path), network, scheduler, maxSegments);
}

auto SegmentedDownload::makeApiFile(QUrl url, QString path, QNetworkAccessManager* network, HostScheduler* scheduler, int maxSegments)
    -> Ptr
{
    auto task = makeFile(std::move(url), std::move(path), network, scheduler, maxSegments);
#if defined(LAUNCHER_APPLICATION)
    task->m_decorate = [](NetRequest& request) { request.addHeaderProxy(std::make_unique<ApiHeaderProxy>()); };
#endif
    return task;
}

auto SegmentedDownload::makeApiFile(QUrl url,
                                    QString path,
                                    QNetworkAccessManager* network,
                                    HostScheduler* scheduler,
                                    int maxSegments,
                                    const ModrinthDownloadMeta& meta) -> Ptr
{
    auto task = makeFile(std::move(url), std::move(path), network, scheduler, maxSegments);
#if defined(LAUNCHER_APPLICATION)
    task->m_decorate = [meta](NetRequest& request) { request.addHeaderProxy(std::make_unique<ApiHeaderProxy>(meta)); };
#else
    Q_UNUSED(meta)
#endif
    return task;
}

void SegmentedDownload::addValidator(Validator* validator)
{
    if (validator)
        m_validators.push_back(std::shared_ptr<Validator>(validator));
}

bool SegmentedDownload::ranUnsegmented() const
{
    return m_segmentsUsed <= 1;
}

int SegmentedDownload::allowedSegments() const
{
    const int wanted = qBound(0, m_maxSegments, MaxSegments);
    if (wanted < 2)
        return 1;
    // Never take more than half of what the host is allowed to run at once, so the ordinary
    // files queued next to this one keep a share of the pool.
    const int ceiling = m_scheduler ? m_scheduler->ceilingFor(m_url) : HostScheduler::UnknownHostCeiling;
    return qMin(wanted, qMax(1, ceiling / 2));
}

int SegmentedDownload::segmentCountFor(qint64 total) const
{
    if (total < MinSegmentedSize)
        return 1;
    const qint64 byMinimumSize = total / MinSegmentSize;
    return static_cast<int>(qBound<qint64>(1, byMinimumSize, static_cast<qint64>(allowedSegments())));
}

void SegmentedDownload::resetForRestart()
{
    // Everything a previous run left behind. Task::start() allows a finished task to be started
    // again, so this is what makes that safe rather than half restoring an old transfer.
    retireJob();
    for (const auto& job : m_retired) {
        disconnect(job.get(), nullptr, this, nullptr);
    }
    m_retired.clear();
    if (m_part) {
        m_part->discard();
        m_part.reset();
    }
    m_states.clear();
    m_discovery.reset();
    m_mode = Mode::Idle;
    m_total = -1;
    m_entityValidator.clear();
    m_rangesUsable = false;
    m_restartPending = false;
    m_restarted = false;
    m_aborting = false;
    m_segmentsUsed = 0;
    m_lastProgress = 0;
}

void SegmentedDownload::executeTask()
{
    resetForRestart();

    setStatus(tr("Downloading %1").arg(Privacy::sanitizeUrl(m_url, 80)));
    m_clock.start();
    m_lastSpeedMs = 0;
    m_lastSpeedBytes = 0;

    if (!m_network) {
        emitFailed(tr("No network access manager was provided."));
        return;
    }
    if (!m_url.isValid() || m_url.isEmpty()) {
        emitFailed(tr("The download address is not valid."));
        return;
    }

    // With segmentation switched off, or with a target a part file cannot sit next to, behave
    // exactly like an ordinary download rather than degrading anything.
    if (allowedSegments() < 2 || !PartFile::isUsableFor(m_targetPath)) {
        startLegacy();
        return;
    }

    m_part = std::make_shared<PartFile>(m_targetPath);
    QString error;
    if (!m_part->open(&error)) {
        m_part.reset();
        qCDebug(taskDownloadLogC) << getUid().toString() << "Falling back to an unsegmented download:" << Privacy::sanitizeText(error);
        startLegacy();
        return;
    }

    m_progressTimer.start();
    startDiscovery();
}

NetJob::Ptr SegmentedDownload::newJob(int maxConcurrent)
{
    auto job = makeShared<NetJob>(objectName(), m_network, maxConcurrent, m_scheduler.data());
    // The coordinator owns the user facing outcome; an inner job must never open its own dialog.
    job->setAskRetry(false);
    return job;
}

void SegmentedDownload::connectJob()
{
    connect(m_job.get(), &Task::succeeded, this, &SegmentedDownload::onJobSucceeded);
    connect(m_job.get(), &Task::failed, this, &SegmentedDownload::onJobFailed);
    connect(m_job.get(), &Task::aborted, this, &SegmentedDownload::onJobAborted);
}

void SegmentedDownload::retireJob()
{
    if (!m_job)
        return;
    auto job = m_job;
    m_job.reset();
    disconnect(job.get(), nullptr, this, nullptr);
    if (job->isRunning())
        job->abort();
    // Kept alive until this task ends: a reply that is still unwinding must not land in a job
    // that has already been destroyed.
    m_retired.push_back(job);
}

void SegmentedDownload::rewindToStart(const std::shared_ptr<SegmentState>& state)
{
    if (state->cursor == state->start)
        return;
    state->cursor = state->start;
    // The response restarts the entity from the beginning, so anything the previous attempt wrote
    // past this point belongs to a version we are no longer downloading.
    if (m_part && state->start == 0) {
        QString error;
        if (!m_part->truncateAll(&error))
            qCDebug(taskDownloadLogC) << "Could not drop the previous attempt's bytes:" << Privacy::sanitizeText(error);
    }
}

Net::NetRequest::Ptr SegmentedDownload::makeSegmentRequest(const std::shared_ptr<SegmentState>& state, bool ranged)
{
    auto request = makeShared<SegmentRequest>(m_url, m_part, state);
    if (ranged)
        request->addHeaderProxy(std::make_unique<RangeHeaderProxy>(state, m_entityValidator));
    if (m_decorate)
        m_decorate(*request);

    // Guarded rather than raw: a retired job is released with deleteLater(), so one of its
    // replies can still be unwinding after this task itself is gone.
    QPointer<SegmentedDownload> self(this);
    std::weak_ptr<SegmentState> weakState = state;
    request->setHeaderCallback([self, weakState](QNetworkReply& reply) {
        auto locked = weakState.lock();
        if (self && locked)
            self->onSegmentHeaders(locked, reply);
    });
    return request;
}

void SegmentedDownload::startLegacy()
{
    m_mode = Mode::Legacy;
    m_segmentsUsed = 1;

    auto download = Download::makeFile(m_url, m_targetPath);
    if (m_decorate)
        m_decorate(*download);
    for (const auto& validator : m_validators)
        download->addValidator(new BorrowedValidator(validator));

    connect(download.get(), &Task::progress, this, [this](qint64 current, qint64 total) { setProgress(current, total); });
    connect(download.get(), &Task::details, this, &SegmentedDownload::setDetails);

    m_job = newJob(1);
    m_job->addNetAction(download);
    connectJob();
    m_job->start();
}

void SegmentedDownload::startDiscovery()
{
    m_mode = Mode::Discovery;
    m_discovery = std::make_shared<SegmentState>();
    m_discovery->start = 0;
    m_discovery->end = DiscoveryChunk - 1;
    m_discovery->cursor = 0;
    m_states.clear();
    m_states.push_back(m_discovery);

    m_job = newJob(1);
    m_job->addNetAction(makeSegmentRequest(m_discovery, true));
    connectJob();
    m_job->start();
}

void SegmentedDownload::onDiscoveryHeaders(QNetworkReply& reply)
{
    const int status = replyStatus(reply);
    if (status >= 300 && status < 400)
        return;  // Qt is still following a redirect; the response that matters comes later.

    if (status == 200) {
        // The server ignored the range. This same reply is already streaming the whole file into
        // the part file, so it simply becomes the ordinary single stream download.
        m_mode = Mode::Streaming;
        m_discovery->end = -1;
        // A 200 body always starts at byte zero, even if a previous attempt had already moved
        // the cursor along. Drop whatever that attempt wrote so no stale tail can survive.
        rewindToStart(m_discovery);
        m_discovery->accepted = true;
        m_discovery->headersSeen = true;
        if (!hasTransformingContentEncoding(reply)) {
            // Only trust the length when nothing re-encodes the body on the way here.
            bool ok = false;
            const qint64 length = reply.header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
            m_total = (ok && length > 0) ? length : -1;
        }
        qCDebug(taskDownloadLogC) << getUid().toString() << "Ranges are not supported; downloading in one stream";
        return;
    }

    if (status != 206) {
        // Not something we can place in the file. NetRequest's own error handling reports it.
        m_discovery->headersSeen = true;
        m_discovery->accepted = false;
        if (status == 416) {
            restartUnranged(QStringLiteral("the server rejected the requested range"));
        } else if (status == 403 || status == 404) {
            // Some CDNs reject any ranged URL at this hostname while redirecting ordinary GETs
            // to a working file host. The unranged retry still decides whether the file exists.
            restartUnranged(tr("the server refused a ranged request with HTTP %1").arg(status));
        }
        return;
    }

    qint64 first = 0;
    qint64 last = 0;
    qint64 total = 0;
    if (!parseContentRange(reply.rawHeader("Content-Range"), &first, &last, &total) || first != m_discovery->cursor) {
        restartUnranged(QStringLiteral("the server sent an unusable Content-Range"));
        return;
    }
    if (hasTransformingContentEncoding(reply)) {
        restartUnranged(QStringLiteral("the response body is content-encoded, so byte offsets are meaningless"));
        return;
    }
    if (isMultipartRange(reply)) {
        restartUnranged(QStringLiteral("the server answered with a multipart range"));
        return;
    }

    m_total = total;
    m_discovery->end = last;
    m_discovery->accepted = true;
    m_discovery->headersSeen = true;
    m_entityValidator = entityValidatorOf(reply);
    // Accept-Ranges is advisory next to a valid 206, but an explicit refusal is honoured.
    m_rangesUsable = reply.rawHeader("Accept-Ranges").trimmed().toLower() != "none";
}

void SegmentedDownload::onSegmentHeaders(const std::shared_ptr<SegmentState>& state, QNetworkReply& reply)
{
    if (m_restartPending || m_aborting)
        return;

    if (state == m_discovery && m_mode == Mode::Discovery) {
        onDiscoveryHeaders(reply);
        return;
    }

    const int status = replyStatus(reply);
    if (status >= 300 && status < 400)
        return;

    if (m_mode == Mode::Plain) {
        // No Range was sent, so any successful response is the whole entity, from byte zero.
        state->headersSeen = true;
        state->accepted = status >= 200 && status < 300;
        state->end = -1;
        rewindToStart(state);
        return;
    }

    if (m_mode == Mode::Streaming) {
        // A retry of the single stream: the server may hand us the whole entity again, or it may
        // honour the range this time. Both are fine as long as we write where it actually starts.
        state->headersSeen = true;
        state->end = -1;
        if (status == 200) {
            rewindToStart(state);
            state->accepted = true;
            return;
        }
        qint64 resumedFirst = 0;
        qint64 resumedLast = 0;
        qint64 resumedTotal = 0;
        state->accepted = status == 206 && parseContentRange(reply.rawHeader("Content-Range"), &resumedFirst, &resumedLast, &resumedTotal)
                          && resumedFirst == state->cursor && !hasTransformingContentEncoding(reply) && !isMultipartRange(reply);
        return;
    }

    state->headersSeen = true;

    if (status == 200) {
        // If-Range did not match, so the server is offering the whole (new) entity instead of the
        // range we asked for. Everything downloaded so far belongs to a different version.
        restartUnranged(QStringLiteral("the file changed on the server while it was being downloaded"));
        return;
    }

    qint64 first = 0;
    qint64 last = 0;
    qint64 total = 0;
    const bool parsed = parseContentRange(reply.rawHeader("Content-Range"), &first, &last, &total);
    if (status != 206 || !parsed || first != state->cursor || last != state->end || total != m_total
        || hasTransformingContentEncoding(reply) || isMultipartRange(reply)) {
        // Retrying cannot fix a server that answers a range request with the wrong range.
        state->accepted = false;
        state->hardError = tr("The server answered with the wrong part of the file.");
        return;
    }

    state->accepted = true;
}

void SegmentedDownload::restartUnranged(const QString& why)
{
    if (m_restartPending || m_aborting || !isRunning())
        return;

    if (m_restarted || !m_part) {
        failWith(tr("The download could not be completed: %1.").arg(why));
        return;
    }

    qCDebug(taskDownloadLogC) << getUid().toString() << "Restarting without ranges:" << why;
    m_restartPending = true;
    m_restarted = true;
    for (const auto& state : m_states)
        state->dropWrites = true;

    // Unwind out of the reply's own signal before tearing its job down.
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_restartPending = false;
            if (!isRunning() || m_aborting)
                return;

            retireJob();

            QString error;
            if (!m_part->truncateAll(&error)) {
                failWith(error);
                return;
            }

            m_mode = Mode::Plain;
            m_total = -1;
            m_entityValidator.clear();
            m_segmentsUsed = 1;

            m_states.clear();
            m_discovery = std::make_shared<SegmentState>();
            m_states.push_back(m_discovery);

            m_job = newJob(1);
            m_job->addNetAction(makeSegmentRequest(m_discovery, false));
            connectJob();
            m_job->start();
        },
        Qt::QueuedConnection);
}

void SegmentedDownload::startSegments(qint64 from)
{
    const qint64 remaining = m_total - from;
    int count = m_rangesUsable ? segmentCountFor(m_total) : 1;
    // The discovery request already took the head of the file, so bound the split by what is
    // actually left. This also makes a zero length segment impossible.
    count = static_cast<int>(qBound<qint64>(1, remaining / MinSegmentSize, static_cast<qint64>(count)));

    m_segmentsUsed = count;
    m_mode = Mode::Segments;
    m_part->preallocate(m_total);

    m_job = newJob(count);
    qint64 cursor = from;
    for (int i = 0; i < count; i++) {
        // The last segment absorbs the rounding so the ranges cover [from, m_total) exactly.
        const qint64 length = (i == count - 1) ? (m_total - cursor) : (remaining / count);
        auto state = std::make_shared<SegmentState>();
        state->start = cursor;
        state->cursor = cursor;
        state->end = cursor + length - 1;
        cursor += length;

        m_states.push_back(state);
        m_job->addNetAction(makeSegmentRequest(state, true));
    }
    Q_ASSERT(cursor == m_total);

    qCDebug(taskDownloadLogC) << getUid().toString() << "Splitting" << m_total << "bytes into" << count << "segments";
    connectJob();
    m_job->start();
}

void SegmentedDownload::onJobSucceeded()
{
    if (m_restartPending || m_aborting || !isRunning())
        return;

    switch (m_mode) {
        case Mode::Legacy:
            m_progressTimer.stop();
            m_mode = Mode::Done;
            logOutcome("ok");
            emitSucceeded();
            return;

        case Mode::Discovery: {
            const qint64 next = m_discovery->cursor;
            if (m_total > 0 && next < m_total) {
                startSegments(next);
                return;
            }
            // The whole file fitted inside the discovery request.
            if (m_total <= 0)
                m_total = m_part->size();
            m_segmentsUsed = 1;
            assemble();
            return;
        }

        case Mode::Streaming:
        case Mode::Plain:
            if (m_total <= 0)
                m_total = m_part->size();
            m_segmentsUsed = 1;
            assemble();
            return;

        case Mode::Segments:
            assemble();
            return;

        default:
            return;
    }
}

void SegmentedDownload::onJobFailed(const QString& reason)
{
    if (m_restartPending || m_aborting || !isRunning())
        return;

    // A protocol violation recorded on a segment is a better explanation than the generic one.
    for (const auto& state : m_states) {
        if (!state->hardError.isEmpty()) {
            failWith(state->hardError);
            return;
        }
    }
    failWith(reason);
}

void SegmentedDownload::onJobAborted()
{
    if (m_restartPending || m_aborting || !isRunning())
        return;
    failWith(tr("The download was interrupted."));
}

void SegmentedDownload::assemble()
{
    m_mode = Mode::Validating;
    m_progressTimer.stop();

    if (!m_part->flush()) {
        failWith(tr("Failed writing into %1.").arg(Privacy::sanitizePath(m_targetPath)));
        return;
    }

    if (m_total <= 0)
        m_total = writtenBytes();

    // The part file was preallocated to the full length before any segment ran, so its size on
    // its own proves nothing. Coverage is what proves the bytes are all there.
    QString coverage;
    if (!coversWholeFile(&coverage)) {
        failWith(coverage);
        return;
    }
    const qint64 onDisk = m_part->size();
    if (onDisk != m_total) {
        failWith(tr("The download is %1 bytes on disk but should be %2 bytes.")
                     .arg(QString::number(onDisk), QString::number(m_total)));
        return;
    }

    publishProgress(true);

    if (m_validators.empty()) {
        promote();
        return;
    }

    setStatus(tr("Verifying %1").arg(Privacy::sanitizePath(m_targetPath)));
    m_validation.setFuture(QtConcurrent::run(&replayValidators, m_part->partPath(), m_url, m_validators));
}

void SegmentedDownload::onValidationFinished()
{
    if (m_aborting || !isRunning())
        return;

    const QString error = m_validation.result();
    if (!error.isEmpty()) {
        for (const auto& validator : m_validators)
            validator->abort();
        failWith(error);
        return;
    }

    // ChecksumValidator only reads the URL off this, but the interface takes a reply, and holding
    // on to one of the segment replies to pass here would reach into an object whose owner has
    // already moved on. AssembledEntityReply describes what was actually validated instead.
    AssembledEntityReply reply(m_url, m_total);
    for (const auto& validator : m_validators) {
        if (!validator->validate(reply)) {
            for (const auto& other : m_validators)
                other->abort();
            failWith(tr("Failed to finalize validators"));
            return;
        }
    }
    promote();
}

void SegmentedDownload::promote()
{
    QString error;
    if (!m_part->promote(&error)) {
        failWith(error);
        return;
    }
    m_part.reset();
    m_mode = Mode::Done;
    publishProgress(true);
    logOutcome("ok");
    emitSucceeded();
}

void SegmentedDownload::failWith(const QString& reason)
{
    if (!isRunning())
        return;
    m_progressTimer.stop();
    m_mode = Mode::Done;
    retireJob();
    if (m_part) {
        // Anything short of a user abort leaves no partial state behind.
        m_part->discard();
        m_part.reset();
    }
    logOutcome("failed");
    emitFailed(reason);
}

bool SegmentedDownload::abort()
{
    if (!isRunning())
        return true;

    m_aborting = true;
    m_progressTimer.stop();
    retireJob();
    if (m_part) {
        // The bytes stay for as long as whatever directory holds them does; nothing is promoted.
        m_part->keep();
        m_part.reset();
    }
    m_mode = Mode::Done;
    logOutcome("aborted");
    emitAborted();
    return true;
}

qint64 SegmentedDownload::writtenBytes() const
{
    qint64 total = 0;
    for (const auto& state : m_states)
        total += state->done();
    return total;
}

bool SegmentedDownload::coversWholeFile(QString* error) const
{
    const auto fail = [error](const QString& reason) {
        if (error)
            *error = reason;
        return false;
    };

    if (m_total <= 0)
        return fail(tr("The server never said how large the file is."));

    // [start, cursor) of every segment, in file order.
    QList<QPair<qint64, qint64>> written;
    for (const auto& state : m_states) {
        if (state->cursor > state->start)
            written.append({ state->start, state->cursor });
    }
    std::sort(written.begin(), written.end());

    qint64 covered = 0;
    for (const auto& span : written) {
        if (span.first > covered) {
            return fail(tr("The download is missing the bytes from %1 to %2.")
                            .arg(QString::number(covered), QString::number(span.first - 1)));
        }
        if (span.first < covered) {
            return fail(tr("Two parts of the download claim the same bytes at %1.").arg(QString::number(span.first)));
        }
        covered = span.second;
    }

    if (covered != m_total) {
        return fail(tr("The download stopped at %1 bytes but the server said it would be %2 bytes.")
                        .arg(QString::number(covered), QString::number(m_total)));
    }
    return true;
}

void SegmentedDownload::publishProgress(bool terminal)
{
    qint64 current = writtenBytes();
    if (terminal) {
        m_lastProgress = current;
    } else {
        // Intermediate updates never go backwards: an unranged restart makes progress plateau
        // rather than jump back.
        current = qMax(current, m_lastProgress);
        m_lastProgress = current;
    }

    const qint64 total = m_total > 0 ? m_total : -1;

    const qint64 elapsedMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    const qint64 sinceMs = elapsedMs - m_lastSpeedMs;
    if (sinceMs > 0) {
        const double speed = static_cast<double>(current - m_lastSpeedBytes) / sinceMs * 1000;
        m_lastSpeedBytes = current;
        m_lastSpeedMs = elapsedMs;

        QString details = tr("%1 / %2")
                              .arg(StringUtils::humanReadableFileSize(current), StringUtils::humanReadableFileSize(total));
        if (total > 0 && speed > 0) {
            details += QStringLiteral("\n")
                       + tr("%1 /s (%2)")
                             .arg(StringUtils::humanReadableFileSize(speed), Time::humanReadableDuration((total - current) / speed));
        } else {
            details += QStringLiteral("\n") + tr("%1 /s").arg(StringUtils::humanReadableFileSize(speed));
        }
        setDetails(details);
    }

    setProgress(current, total);
}

void SegmentedDownload::logOutcome(const char* result) const
{
    const qint64 elapsedMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    const double seconds = elapsedMs / 1000.0;
    const double mib = m_total > 0 ? m_total / (1024.0 * 1024.0) : 0.0;
    qCInfo(taskDownloadLogC).nospace() << "Segmented download " << result << ": host=" << m_url.host()
                                       << " bytes=" << m_total << " segments=" << m_segmentsUsed << " seconds="
                                       << QString::number(seconds, 'f', 1)
                                       << " MiB/s=" << QString::number(seconds > 0 ? mib / seconds : 0.0, 'f', 2)
                                       << " restarted=" << m_restarted;
}

}  // namespace Net
