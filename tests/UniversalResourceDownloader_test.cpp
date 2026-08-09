// SPDX-License-Identifier: GPL-3.0-only

#include <QTest>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUuid>

#include <utility>

#include "BuildConfig.h"
#include "Version.h"
#include "modplatform/ResourceAPI.h"
#include "modplatform/ResourceType.h"
#include "modplatform/flame/FlameAPI.h"
#include "modplatform/flame/FlameModIndex.h"
#include "modplatform/modrinth/ModrinthAPI.h"
#include "net/ApiHeaderProxy.h"
#include "settings/CredentialStore.h"

namespace {
class CredentialCleanup final {
   public:
    explicit CredentialCleanup(QString name) : m_name(std::move(name)) {}
    ~CredentialCleanup()
    {
        QString ignoredError;
        CredentialStore::remove(m_name, &ignoredError);
    }

   private:
    QString m_name;
};
}  // namespace

class UniversalResourceDownloaderTest : public QObject {
    Q_OBJECT

   private slots:
    void createsModrinthPluginSearch()
    {
        ResourceAPI::SearchArgs args;
        args.type = ModPlatform::ResourceType::Plugin;
        args.versions = std::vector<Version>{ Version("1.21.8") };
        args.loaderNames = QStringList{ "paper", "spigot", "bukkit" };

        const auto url = ModrinthAPI::get().getSearchURL(args);
        QVERIFY(url.has_value());
        QVERIFY(url->contains("project_type:plugin"));
        QVERIFY(url->contains("categories:paper"));
        QVERIFY(url->contains("categories:spigot"));
        QVERIFY(url->contains("categories:bukkit"));
        QVERIFY(url->contains("versions:1.21.8"));
    }

    void createsCurseForgePluginSearch()
    {
        ResourceAPI::SearchArgs args;
        args.type = ModPlatform::ResourceType::Plugin;
        args.versions = std::vector<Version>{ Version("1.21.8") };
        args.loaderNames = QStringList{ "paper", "spigot", "bukkit" };

        const auto url = FlameAPI::get().getSearchURL(args);
        QVERIFY(url.has_value());
        QVERIFY(url->contains("classId=5"));
        QVERIFY(url->contains("gameVersion=1.21.8"));
        QVERIFY(!url->contains("modLoaderTypes"));
    }

    void createsPluginVersionRequest()
    {
        auto pack = std::make_shared<ModPlatform::IndexedPack>();
        pack->addonId = "test-plugin";
        ResourceAPI::VersionSearchArgs args;
        args.pack = pack;
        args.mcVersions = std::vector<Version>{ Version("1.21.8") };
        args.resourceType = ModPlatform::ResourceType::Plugin;
        args.loaderNames = QStringList{ "paper", "spigot", "bukkit" };

        const auto url = ModrinthAPI::get().getVersionsURL(args);
        QVERIFY(url.has_value());
        QVERIFY(url->contains("/project/test-plugin/version"));
        QVERIFY(url->contains("game_versions="));
        QVERIFY(url->contains("loaders=[\"paper\",\"spigot\",\"bukkit\"]"));
    }

    void keepsInstanceModSearchUnchanged()
    {
        ResourceAPI::SearchArgs args;
        args.type = ModPlatform::ResourceType::Mod;
        args.loaders = ModPlatform::Fabric;
        args.versions = std::vector<Version>{ Version("1.21.8") };

        const auto url = ModrinthAPI::get().getSearchURL(args);
        QVERIFY(url.has_value());
        QVERIFY(url->contains("project_type:mod"));
        QVERIFY(url->contains("categories:fabric"));
        QCOMPARE(ModPlatform::ResourceTypeUtils::getName(ModPlatform::ResourceType::Plugin), QString("plugin"));
    }

    void readsCurseForgeServerPackMetadata()
    {
        QJsonObject file{
            { "gameVersions", QJsonArray{ "1.20.1", "Forge" } },
            { "modId", 123 },
            { "id", 456 },
            { "serverPackFileId", 789 },
            { "fileDate", "2026-07-21T00:00:00Z" },
            { "displayName", "Pack 1.0" },
            { "downloadUrl", "https://example.invalid/client.zip" },
            { "fileName", "client.zip" },
            { "releaseType", 1 },
            { "hashes", QJsonArray{} },
            { "dependencies", QJsonArray{} },
        };

        const auto version = FlameMod::loadIndexedPackVersion(file);
        QCOMPARE(version.fileId.toInt(), 456);
        QCOMPARE(version.serverPackFileId.toInt(), 789);
    }

    void scopesCurseForgeApiKeyToOfficialHttpsOrigin()
    {
        Net::CurseForgeApiKeyHeaderProxy proxy("test-secret");

        const QNetworkRequest apiRequest(
            QUrl("https://api.curseforge.com/v1/games/432"));
        const auto apiHeaders = proxy.headers(apiRequest);
        QCOMPARE(apiHeaders.size(), 1);
        QCOMPARE(apiHeaders.constFirst().headerName, QByteArray("x-api-key"));

        QVERIFY(proxy.headers(QNetworkRequest(
                    QUrl("http://api.curseforge.com/v1/games/432")))
                    .isEmpty());
        QVERIFY(proxy.headers(QNetworkRequest(
                    QUrl("https://edge.forgecdn.net/files/pack.zip")))
                    .isEmpty());
        QVERIFY(proxy.headers(QNetworkRequest(
                    QUrl("https://api.curseforge.com.example/v1/games/432")))
                    .isEmpty());
        QVERIFY(proxy.headers(QNetworkRequest(
                    QUrl("https://api.curseforge.com:444/v1/games/432")))
                    .isEmpty());
    }

    void scopesModrinthApiTokenToConfiguredHttpsOrigins()
    {
        const QList<QUrl> configuredOrigins{
            QUrl(BuildConfig.MODRINTH_PROD_URL),
            QUrl(BuildConfig.MODRINTH_STAGING_URL),
        };

        for (const QUrl& configured : configuredOrigins) {
            QVERIFY2(Net::isModrinthApiRequest(configured),
                     qPrintable(configured.toString()));

            QUrl http = configured;
            http.setScheme(QStringLiteral("http"));
            QVERIFY(!Net::isModrinthApiRequest(http));

            QUrl lookalike = configured;
            lookalike.setHost(configured.host() + QStringLiteral(".example"));
            QVERIFY(!Net::isModrinthApiRequest(lookalike));

            QUrl wrongPort = configured;
            const int configuredPort = configured.port(443);
            wrongPort.setPort(configuredPort == 65535 ? configuredPort - 1
                                                      : configuredPort + 1);
            QVERIFY(!Net::isModrinthApiRequest(wrongPort));
        }
    }

    void buildsAndParsesCurseForgeDownloadUrlRequest()
    {
        QCOMPARE(FlameAPI::fileDownloadUrlEndpoint("123", "789"),
                 QUrl("https://api.curseforge.com/v1/mods/123/files/789/download-url"));

        QString error;
        const QUrl url = FlameAPI::loadFileDownloadUrl(
            R"({"data":"https://mediafilez.forgecdn.net/files/server.zip"})",
            &error);
        QCOMPARE(url,
                 QUrl("https://mediafilez.forgecdn.net/files/server.zip"));
        QVERIFY(error.isEmpty());

        const QUrl insecureUrl = FlameAPI::loadFileDownloadUrl(
            QByteArrayLiteral(
                "{\"data\":\"http://example.invalid/server.zip\"}"),
            &error);
        QVERIFY(insecureUrl.isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void storesApiKeysOutsideLauncherSettings()
    {
        const QString credentialName = QStringLiteral("TestCurseForgeApiKey-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        CredentialCleanup cleanup(credentialName);
        QString error;
        QVERIFY2(CredentialStore::write(credentialName,
                                        QStringLiteral("secret-value"), &error),
                 qPrintable(error));
        QCOMPARE(CredentialStore::read(credentialName, &error),
                 QStringLiteral("secret-value"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(CredentialStore::remove(credentialName, &error),
                 qPrintable(error));
        QVERIFY(CredentialStore::read(credentialName, &error).isEmpty());
    }
};

QTEST_GUILESS_MAIN(UniversalResourceDownloaderTest)

#include "UniversalResourceDownloader_test.moc"
