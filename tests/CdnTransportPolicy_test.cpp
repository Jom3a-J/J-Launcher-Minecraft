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
#include "net/Request.h"
#include "net/RequestPolicy.h"

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

    void knownMojangFileHostsUseTheirSchedulerCeiling()
    {
        const QList<std::tuple<QString, HostClass, int>> hosts = {
            { QStringLiteral("resources.download.minecraft.net"), HostClass::MinecraftResources, 16 },
            { QStringLiteral("libraries.minecraft.net"), HostClass::MinecraftLibraries, 12 },
            { QStringLiteral("piston-data.mojang.com"), HostClass::Unknown, 6 },
        };

        for (const auto& [host, expectedClass, expectedLimit] : hosts) {
            const QUrl url = httpsUrl(host);
            QCOMPARE(HostScheduler::classify(url), expectedClass);
            QCOMPARE(HostScheduler::hardCeiling(expectedClass), expectedLimit);

            QNetworkRequest request(url);
            QVERIFY(Net::applyMojangHttp1TransportPolicy(request, true));
            const QVariant http2Allowed = request.attribute(QNetworkRequest::Http2AllowedAttribute);
            QVERIFY(http2Allowed.isValid());
            QCOMPARE(http2Allowed.toBool(), false);
            QCOMPARE(request.http1Configuration().numberOfConnectionsPerHost(), qsizetype(expectedLimit));
        }
    }

    void otherMojangHostsAreLeftUntouched()
    {
        const QStringList hosts = {
            QStringLiteral("piston-meta.mojang.com"),
            QStringLiteral("evil-resources.download.minecraft.net.example.com"),
            QStringLiteral("resources.download.minecraft.net.evil.example.com"),
            QStringLiteral("unrelated.example.com"),
        };

        for (const auto& host : hosts) {
            QNetworkRequest request(httpsUrl(host));
            QHttp1Configuration originalHttp1 = request.http1Configuration();
            originalHttp1.setNumberOfConnectionsPerHost(13);
            request.setHttp1Configuration(originalHttp1);
            request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);

            QVERIFY(!Net::applyMojangHttp1TransportPolicy(request, true));
            QCOMPARE(request.attribute(QNetworkRequest::Http2AllowedAttribute).toBool(), true);
            QCOMPARE(request.http1Configuration().numberOfConnectionsPerHost(), qsizetype(13));
        }
    }

    void providerSettingsCanBeToggledIndependently()
    {
        QNetworkRequest mojangWhenDisabled(httpsUrl(QStringLiteral("resources.download.minecraft.net")));
        const qsizetype defaultMojangConnections =
            mojangWhenDisabled.http1Configuration().numberOfConnectionsPerHost();
        QVERIFY(!Net::applyCdnHttp1TransportPolicy(mojangWhenDisabled, true));
        QVERIFY(!Net::applyMojangHttp1TransportPolicy(mojangWhenDisabled, false));
        QVERIFY(!mojangWhenDisabled.attribute(QNetworkRequest::Http2AllowedAttribute).isValid());
        QCOMPARE(mojangWhenDisabled.http1Configuration().numberOfConnectionsPerHost(), defaultMojangConnections);

        QNetworkRequest curseForgeWhenMojangDisabled(httpsUrl(QStringLiteral("edge.forgecdn.net")));
        QVERIFY(Net::applyCdnHttp1TransportPolicy(curseForgeWhenMojangDisabled, true));
        QCOMPARE(curseForgeWhenMojangDisabled.attribute(QNetworkRequest::Http2AllowedAttribute).toBool(), false);
        QCOMPARE(curseForgeWhenMojangDisabled.http1Configuration().numberOfConnectionsPerHost(), qsizetype(16));

        QNetworkRequest curseForgeWhenDisabled(httpsUrl(QStringLiteral("edge.forgecdn.net")));
        const qsizetype defaultCdnConnections =
            curseForgeWhenDisabled.http1Configuration().numberOfConnectionsPerHost();
        QVERIFY(!Net::applyCdnHttp1TransportPolicy(curseForgeWhenDisabled, false));
        QVERIFY(!Net::applyMojangHttp1TransportPolicy(curseForgeWhenDisabled, true));
        QVERIFY(!curseForgeWhenDisabled.attribute(QNetworkRequest::Http2AllowedAttribute).isValid());
        QCOMPARE(curseForgeWhenDisabled.http1Configuration().numberOfConnectionsPerHost(), defaultCdnConnections);

        QNetworkRequest mojangWhenCdnDisabled(httpsUrl(QStringLiteral("piston-data.mojang.com")));
        QVERIFY(Net::applyMojangHttp1TransportPolicy(mojangWhenCdnDisabled, true));
        QCOMPARE(mojangWhenCdnDisabled.attribute(QNetworkRequest::Http2AllowedAttribute).toBool(), false);
        QCOMPARE(mojangWhenCdnDisabled.http1Configuration().numberOfConnectionsPerHost(), qsizetype(6));
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

    void redirectsAreFollowedOnlyWhenSafe()
    {
        const QUrl current(QStringLiteral("https://api.example.com/files/1"));
        const auto check = [&current](const QByteArray& location, bool credentials = false, int followed = 0) {
            return Net::checkRedirect(current, location, credentials, followed);
        };

        // Relative and absolute redirects resolve against the current address.
        QCOMPARE(check("/files/2").target, QUrl(QStringLiteral("https://api.example.com/files/2")));
        QCOMPARE(check("//cdn.example.com/f").target, QUrl(QStringLiteral("https://cdn.example.com/f")));
        QVERIFY(check("https://cdn.example.com/f").rejection.isEmpty());
        // HTTP may move up to HTTPS.
        QVERIFY(Net::checkRedirect(QUrl(QStringLiteral("http://a.example.com/")), "https://b.example.com/", false, 0)
                    .rejection.isEmpty());

        const auto rejects = [](const Net::RedirectDecision& decision, const QString& expected) {
            QVERIFY2(decision.rejection.contains(expected), qPrintable(decision.rejection));
            QVERIFY(decision.target.isEmpty());
        };
        rejects(check(""), QStringLiteral("empty"));
        rejects(check("http://cdn.example.com/f"), QStringLiteral("cannot be downgraded"));
        rejects(check("ftp://cdn.example.com/f"), QStringLiteral("not permitted"));
        rejects(check("https://other.example.com/f", true), QStringLiteral("credentials cannot cross origins"));
        rejects(check("https://api.example.com:8443/f", true), QStringLiteral("credentials cannot cross origins"));
        QVERIFY(check("https://api.example.com:443/f", true).rejection.isEmpty());
        rejects(check("/f", false, Net::MaxRedirects), QStringLiteral("too many redirects"));
        QVERIFY(check("/f", false, Net::MaxRedirects - 1).rejection.isEmpty());

        QNetworkRequest withKey(QUrl(QStringLiteral("https://api.example.com/")));
        QVERIFY(!Net::containsCredentials(withKey));
        withKey.setRawHeader("X-Api-Key", "secret");
        QVERIFY(Net::containsCredentials(withKey));
    }
};

QTEST_GUILESS_MAIN(CdnTransportPolicyTest)

#include "CdnTransportPolicy_test.moc"
