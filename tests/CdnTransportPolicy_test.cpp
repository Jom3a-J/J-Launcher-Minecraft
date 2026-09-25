// SPDX-License-Identifier: GPL-3.0-only
/*
 *  J Launcher - Minecraft Launcher
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 */

#include <QHttp1Configuration>
#include <QList>
#include <QNetworkRequest>
#include <QStringList>
#include <QTest>

#include <tuple>

#include "net/HostScheduler.h"
#include "net/NetRequest.h"

using Net::HostClass;
using Net::HostScheduler;

namespace {

QUrl httpsUrl(const QString& host)
{
    return QUrl(QStringLiteral("https://") + host + QStringLiteral("/file.jar"));
}

}  // namespace

class CdnTransportPolicyTest final : public QObject {
    Q_OBJECT

   private slots:
    void knownCurseForgeFileHostsUseTheirSchedulerCeiling()
    {
        const QList<std::tuple<QString, HostClass, int>> hosts = {
            { QStringLiteral("edge.forgecdn.net"), HostClass::FlameCdn, 16 },
            { QStringLiteral("mediafilez.forgecdn.net"), HostClass::FlameCdn, 16 },
            { QStringLiteral("media.forgecdn.net"), HostClass::FlameCdn, 16 },
        };

        for (const auto& [host, expectedClass, expectedLimit] : hosts) {
            const QUrl url = httpsUrl(host);
            QCOMPARE(HostScheduler::classify(url), expectedClass);
            QCOMPARE(HostScheduler::hardCeiling(expectedClass), expectedLimit);

            QNetworkRequest request(url);
            QVERIFY(Net::applyCdnHttp1TransportPolicy(request, true));
            const QVariant http2Allowed = request.attribute(QNetworkRequest::Http2AllowedAttribute);
            QVERIFY(http2Allowed.isValid());
            QCOMPARE(http2Allowed.toBool(), false);
            QCOMPARE(request.http1Configuration().numberOfConnectionsPerHost(), qsizetype(expectedLimit));
        }
    }

    void otherHostsAreLeftUntouched()
    {
        const QStringList hosts = {
            QStringLiteral("api.curseforge.com"),
            QStringLiteral("cdn.modrinth.com"),
            QStringLiteral("piston-data.mojang.com"),
            QStringLiteral("other.forgecdn.net"),
        };

        for (const auto& host : hosts) {
            QNetworkRequest request(httpsUrl(host));
            QHttp1Configuration originalHttp1 = request.http1Configuration();
            originalHttp1.setNumberOfConnectionsPerHost(13);
            request.setHttp1Configuration(originalHttp1);
            request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);

            QVERIFY(!Net::applyCdnHttp1TransportPolicy(request, true));
            const QVariant http2Allowed = request.attribute(QNetworkRequest::Http2AllowedAttribute);
            QVERIFY(http2Allowed.isValid());
            QCOMPARE(http2Allowed.toBool(), true);
            QCOMPARE(request.http1Configuration().numberOfConnectionsPerHost(), qsizetype(13));
        }
    }

    void disabledSettingLeavesFreshRequestUnchanged()
    {
        QNetworkRequest request(httpsUrl(QStringLiteral("edge.forgecdn.net")));
        const qsizetype originalConnectionCount = request.http1Configuration().numberOfConnectionsPerHost();

        QVERIFY(!Net::applyCdnHttp1TransportPolicy(request, false));
        QVERIFY(!request.attribute(QNetworkRequest::Http2AllowedAttribute).isValid());
        QCOMPARE(request.http1Configuration().numberOfConnectionsPerHost(), originalConnectionCount);
    }
};

QTEST_GUILESS_MAIN(CdnTransportPolicyTest)

#include "CdnTransportPolicy_test.moc"
