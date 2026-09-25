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

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <memory>

/*! A loopback HTTP server that understands Range, accepts proxy absolute-URI targets, and can be told to misbehave.
 *
 *  Modelled on the LoopbackHttpServer in NetRequest_test.cpp and kept just as small: it reads a
 *  whole request, answers it, and closes. Everything a segmented download has to survive is a
 *  flag here rather than a live server somewhere, so every test is deterministic and offline.
 */
class RangeHttpServer {
   public:
    struct Resource {
        QByteArray body;
        QByteArray etag;          //!< full quoted value, e.g. "\"v1\""; empty to omit
        QByteArray lastModified;  //!< empty to omit
        bool acceptRanges = true;
        //! Answer 200 with the whole body even when a Range was asked for.
        bool ignoreRange = false;
        //! Sent verbatim; the body is NOT actually encoded, which is the point - a client that
        //! trusts a coded response would place bytes at offsets that mean nothing.
        QByteArray contentEncoding;
        //! Answer 206 with a Content-Range that does not describe what was asked for.
        bool badContentRange = false;
        //! Answer 206 but send more bytes than the range covers.
        bool overlongBody = false;
        //! Answer 206 with "bytes a-b/*".
        bool unknownTotal = false;
        //! Answer 206 with Content-Type: multipart/byteranges.
        bool multipart = false;
        //! Answer with this status and an empty body instead of anything else.
        int forcedStatus = 0;
        //! Answer ranged requests with this status, while unranged requests still serve the file.
        int rangeStatus = 0;
        //! Close the connection after this many body bytes, for the next dropCount requests.
        //! The response still promises the full Content-Length, so the client sees a truncated
        //! transfer rather than a body that simply ended.
        qint64 dropAfter = -1;
        int dropCount = 0;
        //! Send this many body bytes and then keep the connection open forever. Gives a test a
        //! transfer that is reliably still in flight when it wants to cancel one.
        qint64 stallAfter = -1;
    };

    struct RequestRecord {
        QByteArray target;
        QByteArray host;
        QByteArray range;
        QByteArray ifRange;
        QByteArray acceptEncoding;
        /// The "modrinth-download-meta" header, so a test can prove where it does and does not go.
        QByteArray modrinthMeta;
        bool authorized = false;
        int status = 0;
    };

    RangeHttpServer()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() { acceptPending(); });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost, 0); }

    QUrl url(const QByteArray& path) const
    {
        QUrl result;
        result.setScheme(QStringLiteral("http"));
        result.setHost(QStringLiteral("127.0.0.1"));
        result.setPort(m_server.serverPort());
        result.setPath(QString::fromUtf8(path));
        return result;
    }

    void serve(const QByteArray& path, Resource resource) { m_routes.insert(path, std::move(resource)); }

    void redirect(const QByteArray& path, const QUrl& destination)
    {
        Resource resource;
        resource.forcedStatus = 302;
        resource.body = destination.toEncoded(QUrl::FullyEncoded);
        m_routes.insert(path, std::move(resource));
    }

    Resource* route(const QByteArray& path)
    {
        auto it = m_routes.find(path);
        return it == m_routes.end() ? nullptr : &(*it);
    }

    const QList<RequestRecord>& requests() const { return m_requests; }
    int requestCount() const { return m_requests.size(); }
    int authorizedRequestCount() const
    {
        int count = 0;
        for (const auto& record : m_requests) {
            if (record.authorized)
                count++;
        }
        return count;
    }
    /// Highest number of sockets that were open at the same time.
    int peakConcurrency() const { return m_peakConcurrency; }
    int rangeRequestCount() const
    {
        int count = 0;
        for (const auto& record : m_requests) {
            if (!record.range.isEmpty())
                count++;
        }
        return count;
    }

    /*! The byte spans the server actually served, as [first, last] inclusive pairs.
     *
     *  Lets a test prove the client asked for a partition of the file rather than inferring it
     *  from the bytes that came out the other end.
     */
    const QList<QPair<qint64, qint64>>& servedRanges() const { return m_servedRanges; }

   private:
    void acceptPending()
    {
        while (QTcpSocket* socket = m_server.nextPendingConnection()) {
            m_openConnections++;
            m_peakConcurrency = qMax(m_peakConcurrency, m_openConnections);
            QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket]() {
                m_openConnections--;
                socket->deleteLater();
            });

            auto buffer = std::make_shared<QByteArray>();
            auto handled = std::make_shared<bool>(false);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer, handled]() {
                buffer->append(socket->readAll());
                if (*handled || !buffer->contains("\r\n\r\n"))
                    return;
                *handled = true;
                respond(socket, *buffer);
            });
        }
    }

    static QByteArray headerValue(const QByteArray& request, const QByteArray& name)
    {
        const QByteArray needle = "\r\n" + name.toLower() + ":";
        const QByteArray lower = request.toLower();
        const int at = lower.indexOf(needle);
        if (at < 0)
            return {};
        const int valueStart = at + needle.size();
        const int end = request.indexOf("\r\n", valueStart);
        return request.mid(valueStart, end - valueStart).trimmed();
    }

    /// Parses "bytes=a-b" and "bytes=a-". Returns false when there is no usable range.
    static bool parseRange(const QByteArray& value, qint64 size, qint64* first, qint64* last)
    {
        if (!value.startsWith("bytes="))
            return false;
        const QByteArray spec = value.mid(6).trimmed();
        const int dash = spec.indexOf('-');
        if (dash <= 0)
            return false;
        bool okFirst = false;
        const qint64 start = spec.left(dash).toLongLong(&okFirst);
        if (!okFirst || start < 0 || start >= size)
            return false;

        const QByteArray endPart = spec.mid(dash + 1).trimmed();
        qint64 end = size - 1;
        if (!endPart.isEmpty()) {
            bool okLast = false;
            end = endPart.toLongLong(&okLast);
            if (!okLast)
                return false;
        }
        *first = start;
        *last = qMin(end, size - 1);
        return *last >= *first;
    }

    static QByteArray reasonFor(int status)
    {
        switch (status) {
            case 200:
                return "OK";
            case 206:
                return "Partial Content";
            case 302:
                return "Found";
            case 403:
                return "Forbidden";
            case 404:
                return "Not Found";
            case 416:
                return "Range Not Satisfiable";
            default:
                return "Unknown";
        }
    }

    void respond(QTcpSocket* socket, const QByteArray& request)
    {
        const QByteArray firstLine = request.left(request.indexOf("\r\n"));
        const QByteArray requestTarget = firstLine.split(' ').value(1);
        const QUrl absoluteTarget = QUrl::fromEncoded(requestTarget);
        const bool proxyRequest = absoluteTarget.isValid() && !absoluteTarget.scheme().isEmpty()
                               && !absoluteTarget.host().isEmpty();
        const QByteArray target = proxyRequest ? absoluteTarget.path(QUrl::FullyEncoded).toUtf8() : requestTarget;

        RequestRecord record;
        record.target = target;
        record.host = proxyRequest ? absoluteTarget.host().toUtf8() : headerValue(request, "Host");
        record.range = headerValue(request, "Range");
        record.ifRange = headerValue(request, "If-Range");
        record.acceptEncoding = headerValue(request, "Accept-Encoding");
        record.modrinthMeta = headerValue(request, "modrinth-download-meta");
        record.authorized = !headerValue(request, "Authorization").isEmpty() || !headerValue(request, "X-Api-Key").isEmpty();

        auto it = m_routes.find(target);
        if (it == m_routes.end()) {
            record.status = 404;
            m_requests.append(record);
            writeSimple(socket, 404, "not found");
            return;
        }
        Resource& resource = *it;

        if (!record.range.isEmpty() && resource.rangeStatus != 0) {
            record.status = resource.rangeStatus;
            m_requests.append(record);
            writeSimple(socket, resource.rangeStatus, {});
            return;
        }

        if (resource.forcedStatus == 302) {
            record.status = 302;
            m_requests.append(record);
            QByteArray reply = "HTTP/1.1 302 Found\r\nConnection: close\r\nLocation: " + resource.body + "\r\nContent-Length: 0\r\n\r\n";
            socket->write(reply);
            socket->disconnectFromHost();
            return;
        }
        if (resource.forcedStatus != 0) {
            record.status = resource.forcedStatus;
            m_requests.append(record);
            writeSimple(socket, resource.forcedStatus, {});
            return;
        }

        const qint64 size = resource.body.size();
        qint64 first = 0;
        qint64 last = size - 1;
        bool partial = !record.range.isEmpty() && resource.acceptRanges && !resource.ignoreRange
                       && parseRange(record.range, size, &first, &last);

        // If-Range that no longer matches means the entity changed: answer with the whole thing.
        if (partial && !record.ifRange.isEmpty()) {
            const QByteArray current = !resource.etag.isEmpty() ? resource.etag : resource.lastModified;
            if (record.ifRange != current) {
                partial = false;
                first = 0;
                last = size - 1;
            }
        }

        QByteArray headers = "HTTP/1.1 ";
        QByteArray body;
        if (partial) {
            record.status = 206;
            m_servedRanges.append({ first, last });
            body = resource.body.mid(static_cast<int>(first), static_cast<int>(last - first + 1));
            if (resource.overlongBody) {
                const qint64 extra = qMin<qint64>(64, size - last - 1);
                if (extra > 0)
                    body.append(resource.body.mid(static_cast<int>(last + 1), static_cast<int>(extra)));
            }

            QByteArray contentRange;
            if (resource.badContentRange) {
                contentRange = "bytes " + QByteArray::number(first + 1) + "-" + QByteArray::number(last) + "/" + QByteArray::number(size);
            } else if (resource.unknownTotal) {
                contentRange = "bytes " + QByteArray::number(first) + "-" + QByteArray::number(last) + "/*";
            } else {
                contentRange =
                    "bytes " + QByteArray::number(first) + "-" + QByteArray::number(last) + "/" + QByteArray::number(size);
            }
            headers += "206 " + reasonFor(206) + "\r\nContent-Range: " + contentRange + "\r\n";
        } else {
            record.status = 200;
            body = resource.body;
            headers += "200 " + reasonFor(200) + "\r\n";
        }
        m_requests.append(record);

        headers += "Connection: close\r\n";
        headers += "Accept-Ranges: " + QByteArray(resource.acceptRanges ? "bytes" : "none") + "\r\n";
        if (!resource.etag.isEmpty())
            headers += "ETag: " + resource.etag + "\r\n";
        if (!resource.lastModified.isEmpty())
            headers += "Last-Modified: " + resource.lastModified + "\r\n";
        if (!resource.contentEncoding.isEmpty())
            headers += "Content-Encoding: " + resource.contentEncoding + "\r\n";
        headers += "Content-Type: ";
        headers += resource.multipart ? "multipart/byteranges; boundary=x" : "application/octet-stream";
        headers += "\r\n";
        headers += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";

        socket->write(headers);

        if (resource.stallAfter >= 0) {
            socket->write(body.left(static_cast<int>(qMin<qint64>(resource.stallAfter, body.size()))));
            socket->flush();
            // Deliberately never finished: the transfer stays in flight until the client gives up
            // or is cancelled.
            return;
        }

        if (resource.dropCount > 0 && resource.dropAfter >= 0) {
            resource.dropCount--;
            const qint64 keep = qMin<qint64>(resource.dropAfter, body.size());
            socket->write(body.left(static_cast<int>(keep)));
            socket->flush();
            // A clean close after a short body, so the bytes that did arrive are not thrown away
            // by a reset - the client must resume from exactly where it stopped.
            socket->disconnectFromHost();
            return;
        }

        socket->write(body);
        socket->disconnectFromHost();
    }

    void writeSimple(QTcpSocket* socket, int status, const QByteArray& body)
    {
        QByteArray reply = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reasonFor(status)
                           + "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        socket->write(reply);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QByteArray, Resource> m_routes;
    QList<RequestRecord> m_requests;
    QList<QPair<qint64, qint64>> m_servedRanges;
    int m_openConnections = 0;
    int m_peakConcurrency = 0;
};
