#include <QJsonObject>
#include <QStandardItemModel>
#include <QtTest>

#include "modplatform/atlauncher/ATLPackIndex.h"
#include "modplatform/atlauncher/ATLPackManifest.h"
#include "ui/pages/modplatform/atlauncher/AtlFilterModel.h"

class ATLPackManifestTest : public QObject
{
    Q_OBJECT

private slots:
    void preservesIndependentClientAndServerCompatibility()
    {
        QJsonObject mod{
            { "name", "Client UI" },
            { "version", "1.0" },
            { "url", "mods/client-ui.jar" },
            { "file", "client-ui.jar" },
            { "download", "server" },
            { "type", "mods" },
            { "client", true },
            { "server", false },
            { "serverSeparate", false },
        };
        QJsonObject root{
            { "version", "test" },
            { "minecraft", "1.21.1" },
            { "noConfigs", true },
            { "loader", QJsonObject{
                { "type", "neoforge" },
                { "metadata", QJsonObject{{ "version", "21.1.219" }} },
            } },
            { "mods", QJsonArray{mod} },
        };

        ATLauncher::PackVersion version;
        ATLauncher::loadVersion(version, root);
        QCOMPARE(version.mods.size(), 1);
        QVERIFY(version.mods.constFirst().client);
        QVERIFY(!version.mods.constFirst().server);
        QVERIFY(!version.mods.constFirst().serverSeparate);
    }

    void defaultsLegacyManifestToServerCompatible()
    {
        QJsonObject mod{
            { "name", "Legacy Mod" },
            { "version", "1.0" },
            { "url", "mods/legacy.jar" },
            { "file", "legacy.jar" },
            { "download", "server" },
            { "type", "mods" },
            { "client", true },
        };
        QJsonObject root{
            { "version", "test" },
            { "minecraft", "1.20.1" },
            { "noConfigs", true },
            { "loader", QJsonObject{
                { "type", "forge" },
                { "metadata", QJsonObject{{ "version", "47.4.0" }} },
            } },
            { "mods", QJsonArray{mod} },
        };

        ATLauncher::PackVersion version;
        ATLauncher::loadVersion(version, root);
        QVERIFY(version.mods.constFirst().server);
    }

    void expandsSeparateServerArtifactForPairedInstall()
    {
        QJsonObject mod{
            { "name", "Dual Artifact Mod" },
            { "version", "1.0" },
            { "url", "mods/client.jar" },
            { "file", "client.jar" },
            { "md5", "client-md5" },
            { "download", "server" },
            { "type", "mods" },
            { "client", true },
            { "server", true },
            { "serverSeparate", true },
            { "serverUrl", "mods/server.jar" },
            { "serverFile", "server.jar" },
            { "serverMd5", "server-md5" },
            { "serverDownload", "direct" },
            { "serverType", "plugins" },
            { "serverOptional", true },
        };
        QJsonObject root{
            { "version", "test" },
            { "minecraft", "1.21.1" },
            { "noConfigs", true },
            { "loader", QJsonObject{
                { "type", "neoforge" },
                { "metadata", QJsonObject{{ "version", "21.1.219" }} },
            } },
            { "mods", QJsonArray{mod} },
        };

        ATLauncher::PackVersion version;
        ATLauncher::loadVersion(version, root);
        const auto expanded = ATLauncher::expandModsForPairedServer(version.mods);

        QCOMPARE(expanded.size(), 2);
        const auto& client = expanded.at(0);
        QVERIFY(client.client);
        QVERIFY(!client.server);
        QCOMPARE(client.file, QString("client.jar"));

        const auto& server = expanded.at(1);
        QVERIFY(!server.client);
        QVERIFY(server.server);
        QVERIFY(server.optional);
        QCOMPARE(server.url, QString("mods/server.jar"));
        QCOMPARE(server.file, QString("server.jar"));
        QCOMPARE(server.md5, QString("server-md5"));
        QCOMPARE(server.download, ATLauncher::DownloadType::Direct);
        QCOMPARE(server.type, ATLauncher::ModType::Plugins);
    }

    void suppliesSkyFactoryOneServerWorldPreset()
    {
        const auto properties =
            ATLauncher::serverPropertyOverridesForPack("SkyFactoryOne");
        QCOMPARE(properties.value("topography-preset"), QString("void"));
        QVERIFY(ATLauncher::serverPropertyOverridesForPack("OtherPack").isEmpty());
    }

    void filtersAtLauncherClientOnlyPacksInServerMode()
    {
        QJsonObject serverPackObject{
            { "id", 1 },
            { "position", 1 },
            { "name", "Server Pack" },
            { "type", "public" },
            { "versions", QJsonArray{ QJsonObject{
                { "version", "1.0" }, { "minecraft", "1.20.1" } } } },
            { "createServer", true },
        };
        QJsonObject clientPackObject = serverPackObject;
        clientPackObject["id"] = 2;
        clientPackObject["name"] = "Client Pack";
        clientPackObject["createServer"] = false;

        ATLauncher::IndexedPack serverPack;
        ATLauncher::IndexedPack clientPack;
        ATLauncher::loadIndexedPack(serverPack, serverPackObject);
        ATLauncher::loadIndexedPack(clientPack, clientPackObject);
        QVERIFY(serverPack.createServer);
        QVERIFY(!clientPack.createServer);

        QStandardItemModel source;
        source.setRowCount(2);
        source.setColumnCount(1);
        source.setData(source.index(0, 0), QVariant::fromValue(serverPack),
                       Qt::UserRole);
        source.setData(source.index(1, 0), QVariant::fromValue(clientPack),
                       Qt::UserRole);
        Atl::FilterModel filter;
        filter.setSourceModel(&source);
        filter.setServerReadyOnly(true);
        QCOMPARE(filter.rowCount(), 1);
        QCOMPARE(filter.data(filter.index(0, 0), Qt::UserRole)
                     .value<ATLauncher::IndexedPack>().name,
                 QString("Server Pack"));
        filter.setServerReadyOnly(false);
        QCOMPARE(filter.rowCount(), 2);
    }
};

QTEST_GUILESS_MAIN(ATLPackManifestTest)

#include "ATLPackManifest_test.moc"
