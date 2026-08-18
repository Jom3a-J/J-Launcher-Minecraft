// SPDX-License-Identifier: GPL-3.0-only

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "Application.h"
#include "net/ByteArraySink.h"
#include "net/Download.h"
#include "net/RawHeaderProxy.h"
#include "net/NetUtils.h"

namespace {
struct Response {
    int status = 200;
    QByteArray location;
    QByteArray body;
};

class LoopbackHttpServer final
{
   public:
    LoopbackHttpServer()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                auto request = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket, request, handled]() {
                                     request->append(socket->readAll());
                                     if (*handled || !request->contains("\r\n\r\n")) {
                                         return;
                                     }
                                     *handled = true;

                                     ++m_requestCount;
                                     const QByteArray firstLine =
                                         request->left(request->indexOf("\r\n"));
                                     const QByteArray target = firstLine.split(' ').value(1);
                                     m_requestTargets.append(target);
                                     if (request->toLower().contains("\nauthorization:")) {
                                         ++m_authorizedRequestCount;
                                     }

                                     Response response;
                                     if (m_routes.contains(target)) {
                                         response = m_routes.value(target);
                                     } else {
                                         response.status = 404;
                                         response.body = "not found";
                                     }

                                     const QByteArray reason = response.status == 200
                                                                   ? QByteArrayLiteral("OK")
                                                                   : response.status == 302
                                                                         ? QByteArrayLiteral("Found")
                                                                         : QByteArrayLiteral("Not Found");
                                     QByteArray reply = "HTTP/1.1 "
                                         + QByteArray::number(response.status) + ' ' + reason
                                         + "\r\nConnection: close\r\n";
                                     if (!response.location.isEmpty()) {
                                         reply += "Location: " + response.location + "\r\n";
                                     }
                                     reply += "Content-Length: "
                                         + QByteArray::number(response.body.size())
                                         + "\r\n\r\n" + response.body;
                                     socket->write(reply);
                                     socket->disconnectFromHost();
                                 });
            }
        });
    }

    bool start()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    QUrl url(const QByteArray& path) const
    {
        QUrl result;
        result.setScheme(QStringLiteral("http"));
        result.setHost(QStringLiteral("127.0.0.1"));
        result.setPort(m_server.serverPort());
        result.setPath(QString::fromUtf8(path));
        return result;
    }

    void redirect(const QByteArray& path, const QUrl& destination)
    {
        redirect(path, destination.toEncoded(QUrl::FullyEncoded));
    }

    void redirect(const QByteArray& path, const QByteArray& location)
    {
        Response response;
        response.status = 302;
        response.location = location;
        m_routes.insert(path, response);
    }

    void body(const QByteArray& path, const QByteArray& contents)
    {
        Response response;
        response.body = contents;
        m_routes.insert(path, response);
    }

    int requestCount() const { return m_requestCount; }
    int authorizedRequestCount() const { return m_authorizedRequestCount; }

   private:
    QTcpServer m_server;
    QHash<QByteArray, Response> m_routes;
    QList<QByteArray> m_requestTargets;
    int m_requestCount = 0;
    int m_authorizedRequestCount = 0;
};

class TestDownload final : public Net::Download
{
   public:
    explicit TestDownload(QUrl url)
    {
        setUrl(std::move(url));
        auto sink = std::make_unique<Net::ByteArraySink>();
        m_output = sink->output();
        m_sink = std::move(sink);
    }

    QByteArray* output() const { return m_output; }

   protected:
    QNetworkReply* getReply(QNetworkRequest& request) override
    {
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        return m_network->get(request);
    }

   private:
    QByteArray* m_output = nullptr;
};
}  // namespace

class NetRequestTest final : public QObject
{
    Q_OBJECT

   private slots:
    void parsesRetryAfterDelaySeconds()
    {
        const QDateTime nowUtc(QDate(2026, 8, 17), QTime(22, 0), Qt::UTC);
        const auto thirtySeconds = Net::parseRetryAfterDelay("30", nowUtc);
        const auto zeroSeconds = Net::parseRetryAfterDelay(" 0 ", nowUtc);
        QVERIFY(thirtySeconds);
        QVERIFY(zeroSeconds);
        QCOMPARE(*thirtySeconds, int64_t(30));
        QCOMPARE(*zeroSeconds, int64_t(0));
    }

    void parsesRetryAfterHttpDateAsUtc()
    {
        const QDateTime nowUtc(QDate(2026, 8, 17), QTime(22, 0), Qt::UTC);
        const auto future = Net::parseRetryAfterDelay(
            "Mon, 17 Aug 2026 22:00:45 GMT", nowUtc);
        const auto past = Net::parseRetryAfterDelay(
            "Mon, 17 Aug 2026 21:59:00 GMT", nowUtc);
        QVERIFY(future);
        QVERIFY(past);
        QCOMPARE(*future, int64_t(45));
        QCOMPARE(*past, int64_t(0));
    }

    void rejectsInvalidRetryAfterValue()
    {
        const QDateTime nowUtc(QDate(2026, 8, 17), QTime(22, 0), Qt::UTC);
        QVERIFY(!Net::parseRetryAfterDelay("soon", nowUtc));
        QVERIFY(!Net::parseRetryAfterDelay("-10", nowUtc));
        QVERIFY(!Net::parseRetryAfterDelay("", nowUtc));
    }

    void credentialedCrossOriginRedirectIsRejected()
    {
        LoopbackHttpServer origin;
        LoopbackHttpServer destination;
        QVERIFY(origin.start());
        QVERIFY(destination.start());
        origin.redirect("/cross-origin", destination.url("/received"));
        destination.body("/received", "must not be requested");

        QNetworkAccessManager network;
        network.setProxy(QNetworkProxy::NoProxy);
        TestDownload request(origin.url("/cross-origin"));
        request.setNetwork(&network);
        auto headers = std::make_unique<Net::RawHeaderProxy>();
        headers->addHeader("Authorization", "Bearer batch6-test-credential");
        request.addHeaderProxy(std::move(headers));

        QSignalSpy failed(&request, &Task::failed);
        QSignalSpy finished(&request, &Task::finished);
        request.start();

        QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
        QCOMPARE(failed.count(), 1);
        QVERIFY(request.failReason().contains(
            QStringLiteral("credentials cannot cross origins")));
        QCOMPARE(origin.requestCount(), 1);
        QCOMPARE(origin.authorizedRequestCount(), 1);
        QCOMPARE(destination.requestCount(), 0);
    }

    void credentialedSameOriginRelativeRedirectSucceeds()
    {
        LoopbackHttpServer server;
        QVERIFY(server.start());
        server.redirect("/start", QByteArrayLiteral("/final"));
        server.body("/final", "same-origin");

        QNetworkAccessManager network;
        network.setProxy(QNetworkProxy::NoProxy);
        TestDownload request(server.url("/start"));
        request.setNetwork(&network);
        auto headers = std::make_unique<Net::RawHeaderProxy>();
        headers->addHeader("Authorization", "Bearer batch6-test-credential");
        request.addHeaderProxy(std::move(headers));

        QSignalSpy failed(&request, &Task::failed);
        QSignalSpy succeeded(&request, &Task::succeeded);
        QSignalSpy finished(&request, &Task::finished);
        request.start();

        QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(succeeded.count(), 1);
        QCOMPARE(*request.output(), QByteArrayLiteral("same-origin"));
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(server.authorizedRequestCount(), 2);
    }

    void credentialFreeCrossOriginRedirectSucceeds()
    {
        LoopbackHttpServer origin;
        LoopbackHttpServer destination;
        QVERIFY(origin.start());
        QVERIFY(destination.start());
        origin.redirect("/cdn", destination.url("/asset"));
        destination.body("/asset", "cdn-content");

        QNetworkAccessManager network;
        network.setProxy(QNetworkProxy::NoProxy);
        TestDownload request(origin.url("/cdn"));
        request.setNetwork(&network);

        QSignalSpy failed(&request, &Task::failed);
        QSignalSpy succeeded(&request, &Task::succeeded);
        QSignalSpy finished(&request, &Task::finished);
        request.start();

        QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(succeeded.count(), 1);
        QCOMPARE(*request.output(), QByteArrayLiteral("cdn-content"));
        QCOMPARE(origin.requestCount(), 1);
        QCOMPARE(destination.requestCount(), 1);
        QCOMPARE(destination.authorizedRequestCount(), 0);
    }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir dataDirectory;
    if (!dataDirectory.isValid()) {
        return 1;
    }

    QByteArray applicationName(argv[0]);
    QByteArray directoryOption("--dir");
    QByteArray directoryPath = dataDirectory.path().toUtf8();
    char* applicationArguments[] = {
        applicationName.data(), directoryOption.data(), directoryPath.data(), nullptr,
    };
    int applicationArgumentCount = 3;
    Application application(applicationArgumentCount, applicationArguments);
    NetRequestTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "NetRequest_test.moc"
