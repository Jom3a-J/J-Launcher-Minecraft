/* SPDX-License-Identifier: GPL-3.0-only */

#include "server/ServerPackCompatibility.h"
#include "server/ServerModpackInstaller.h"

#include "modplatform/flame/CurseForgeHash.h"

#include "net/ChecksumValidator.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

bool writeJson(const QString &path, const QJsonObject &object)
{
    return writeFile(path, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

const ServerPackFileDecision *findFile(const ServerPackCompatibilityReport &report,
                                       const QString &path)
{
    for (const auto &file : report.files) {
        if (file.path.compare(path, Qt::CaseInsensitive) == 0) {
            return &file;
        }
    }
    return nullptr;
}

void writeComponents(const QString &root, const QString &minecraft,
                     const QString &loader = {}, const QString &loaderVersion = {})
{
    QJsonArray components{
        QJsonObject{{"uid", "net.minecraft"}, {"version", minecraft}},
    };
    if (!loader.isEmpty()) {
        const QString uid = loader == QStringLiteral("fabric")
            ? QStringLiteral("net.fabricmc.fabric-loader")
            : loader == QStringLiteral("forge")
                ? QStringLiteral("net.minecraftforge")
                : loader == QStringLiteral("neoforge")
                    ? QStringLiteral("net.neoforged")
                    : QStringLiteral("org.quiltmc.quilt-loader");
        components.append(QJsonObject{{"uid", uid}, {"version", loaderVersion}});
    }
    QVERIFY(writeJson(QDir(root).filePath("mmc-pack.json"),
                      QJsonObject{{"formatVersion", 1}, {"components", components}}));
}

class FixtureReply final : public QNetworkReply {
   public:
    FixtureReply()
    {
        setUrl(QUrl(QStringLiteral("https://fixture.invalid/file")));
    }

    void abort() override {}

   protected:
    qint64 readData(char *, qint64) override { return -1; }
};

}  // namespace

class ServerPackCompatibilityTest : public QObject
{
    Q_OBJECT

private slots:
    void readsModrinthSideAndHashMetadata()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.20.1", "fabric", "0.15.0");
        QVERIFY(writeJson(
            QDir(root.path()).filePath("mrpack/modrinth.index.json"),
            QJsonObject{
                {"formatVersion", 1},
                {"game", "minecraft"},
                {"dependencies", QJsonObject{{"minecraft", "1.20.1"},
                                               {"fabric-loader", "0.15.0"}}},
                {"files", QJsonArray{
                    QJsonObject{{"path", "mods/universal.jar"},
                                 {"env", QJsonObject{{"client", "required"},
                                                      {"server", "required"}}},
                                 {"hashes", QJsonObject{{"sha512", QString(128, 'a')}}}},
                    QJsonObject{{"path", "mods/client.jar"},
                                 {"env", QJsonObject{{"client", "required"},
                                                      {"server", "unsupported"}}}},
                    QJsonObject{{"path", "mods/unknown.jar"}},
                }},
            }));

        const auto report = evaluateServerPack(root.path(), "1.20.1", "fabric", "0.15.0");
        QCOMPARE(report.provider, QString("modrinth"));
        QCOMPARE(report.state, ServerPackCompatibilityState::Unknown);
        QVERIFY(!report.hasDedicatedServerPack);
        const auto *universal = findFile(report, "mods/universal.jar");
        QVERIFY(universal);
        QCOMPARE(universal->side, ServerPackFileSide::Universal);
        QCOMPARE(universal->integrity, ServerPackIntegrityState::ProviderHashAvailable);
        QCOMPARE(universal->hashAlgorithm, QString("sha512"));
        QCOMPARE(universal->hashValue, QString(128, 'a'));
        QCOMPARE(universal->hashSource, QString("mrpack/modrinth.index.json"));
        const auto *clientOnly = findFile(report, "mods/client.jar");
        QVERIFY(clientOnly);
        QCOMPARE(clientOnly->side, ServerPackFileSide::ClientOnly);
        const auto *unknown = findFile(report, "mods/unknown.jar");
        QVERIFY(unknown);
        QCOMPARE(unknown->side, ServerPackFileSide::Unknown);
        QCOMPARE(unknown->integrity, ServerPackIntegrityState::Unverified);
        QVERIFY(!report.warnings.isEmpty());

        QVERIFY(writeFile(QDir(root.path()).filePath("minecraft/mods/universal.jar"), "universal"));
        QVERIFY(writeFile(QDir(root.path()).filePath("minecraft/mods/client.jar"), "client"));
        QVERIFY(writeFile(QDir(root.path()).filePath("minecraft/mods/unknown.jar"), "unknown"));
        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        QStringList skipped;
        QString error;
        QVERIFY(ServerModpackInstaller::prepareContent(
            root.path(), QDir(root.path()).filePath("minecraft"), destination.path(), &skipped,
            &error));
        QVERIFY(QFileInfo::exists(QDir(destination.path()).filePath("mods/universal.jar")));
        QVERIFY(QFileInfo::exists(QDir(destination.path()).filePath("mods/unknown.jar")));
        QVERIFY(!QFileInfo::exists(QDir(destination.path()).filePath("mods/client.jar")));
    }

    void fullyKnownModrinthPackIsKnownCompatible()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.20.1", "fabric", "0.15.0");
        QVERIFY(writeJson(
            QDir(root.path()).filePath("mrpack/modrinth.index.json"),
            QJsonObject{
                {"formatVersion", 1},
                {"game", "minecraft"},
                {"dependencies", QJsonObject{{"minecraft", "1.20.1"},
                                               {"fabric-loader", "0.15.0"}}},
                {"files", QJsonArray{
                    QJsonObject{{"path", "mods/universal.jar"},
                                 {"env", QJsonObject{{"client", "required"},
                                                      {"server", "required"}}}},
                    QJsonObject{{"path", "mods/client.jar"},
                                 {"env", QJsonObject{{"client", "required"},
                                                      {"server", "unsupported"}}}},
                }},
            }));

        const auto report = evaluateServerPack(root.path(), "1.20.1", "fabric", "0.15.0");
        QCOMPARE(report.state, ServerPackCompatibilityState::KnownCompatible);
    }

    void conflictingSideDeclarationsAreOrderIndependent()
    {
        const auto createFixture = [](QTemporaryDir &root, bool universalFirst) {
            QVERIFY(root.isValid());
            writeComponents(root.path(), "1.20.1", "fabric", "0.15.0");
            const QJsonObject universal{
                {"path", "mods/shared.jar"},
                {"env", QJsonObject{{"client", "required"}, {"server", "required"}}},
            };
            const QJsonObject clientOnly{
                {"path", "mods/shared.jar"},
                {"env", QJsonObject{{"client", "required"}, {"server", "unsupported"}}},
            };
            const QJsonArray files = universalFirst
                ? QJsonArray{universal, clientOnly}
                : QJsonArray{clientOnly, universal};
            QVERIFY(writeJson(
                QDir(root.path()).filePath("mrpack/modrinth.index.json"),
                QJsonObject{
                    {"formatVersion", 1},
                    {"game", "minecraft"},
                    {"dependencies", QJsonObject{{"minecraft", "1.20.1"},
                                                   {"fabric-loader", "0.15.0"}}},
                    {"files", files},
                }));
        };

        QTemporaryDir universalFirst;
        createFixture(universalFirst, true);
        QTemporaryDir clientFirst;
        createFixture(clientFirst, false);
        const auto first = inspectServerPack(universalFirst.path());
        const auto second = inspectServerPack(clientFirst.path());
        QVERIFY(!first.isIncompatible());
        QVERIFY(!second.isIncompatible());
        QCOMPARE(findFile(first, "mods/shared.jar")->side,
                 ServerPackFileSide::Universal);
        QCOMPARE(findFile(second, "mods/shared.jar")->side,
                 ServerPackFileSide::Universal);
        QVERIFY(first.projectionWarnings.join('\n').contains("mods/shared.jar"));
        QVERIFY(second.projectionWarnings.join('\n').contains("mods/shared.jar"));
        QVERIFY(!first.projectionWarnings.join('\n').contains(
            "published server pack", Qt::CaseInsensitive));
        QVERIFY(!second.projectionWarnings.join('\n').contains(
            "published server pack", Qt::CaseInsensitive));

        QVERIFY(writeFile(QDir(universalFirst.path()).filePath(
                              "server-pack/server-only.txt"),
                          "mods/shared.jar\n"));
        QVERIFY(writeFile(QDir(clientFirst.path()).filePath(
                              "server-pack/server-only.txt"),
                          "mods/shared.jar\n"));
        const auto serverFirst = inspectServerPack(universalFirst.path());
        const auto serverSecond = inspectServerPack(clientFirst.path());
        QCOMPARE(findFile(serverFirst, "mods/shared.jar")->side,
                 ServerPackFileSide::ServerOnly);
        QCOMPARE(findFile(serverSecond, "mods/shared.jar")->side,
                 ServerPackFileSide::ServerOnly);
    }

    void malformedModrinthEnvironmentMetadataIsRejected()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.20.1", "fabric", "0.15.0");
        QVERIFY(writeJson(
            QDir(root.path()).filePath("mrpack/modrinth.index.json"),
            QJsonObject{
                {"formatVersion", 1},
                {"game", "minecraft"},
                {"dependencies", QJsonObject{{"minecraft", "1.20.1"},
                                               {"fabric-loader", "0.15.0"}}},
                {"files", QJsonArray{
                    QJsonObject{{"path", "mods/string-invalid.jar"}, {"env", "invalid"}},
                    QJsonObject{{"path", "mods/value-invalid.jar"},
                                 {"env", QJsonObject{{"server", "sometimes"}}}},
                    QJsonObject{{"path", "mods/type-invalid.jar"},
                                 {"env", QJsonObject{{"client", QJsonObject{}}}}},
                }},
            }));

        const auto report = inspectServerPack(root.path());
        QVERIFY(!report.isIncompatible());
        QVERIFY(report.hasAdvisory(ServerPackIssueKind::InvalidFileEntry));
        QVERIFY(report.warnings.join('\n').contains("side", Qt::CaseInsensitive));
        // Invalid env entries fall back to Unknown side (conservative include).
        QVERIFY(findFile(report, "mods/string-invalid.jar"));
        QCOMPARE(findFile(report, "mods/string-invalid.jar")->side,
                 ServerPackFileSide::Unknown);
    }

    void unknownSideWarningsAreBounded()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.20.1", "fabric", "0.15.0");
        QJsonArray files;
        for (int i = 0; i < 7; ++i) {
            files.append(QJsonObject{{"path", QString("mods/unknown%1.jar").arg(i)}});
        }
        QVERIFY(writeJson(
            QDir(root.path()).filePath("mrpack/modrinth.index.json"),
            QJsonObject{
                {"formatVersion", 1},
                {"game", "minecraft"},
                {"dependencies", QJsonObject{{"minecraft", "1.20.1"},
                                               {"fabric-loader", "0.15.0"}}},
                {"files", files},
            }));

        const auto report = inspectServerPack(root.path());
        QCOMPARE(report.state, ServerPackCompatibilityState::Unknown);
        QVERIFY(report.warnings.size() <= 8);
        QVERIFY(report.warnings.join('\n').contains("2 additional"));
    }

    void rejectsMinecraftAndLoaderMismatches()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.20.2", "forge", "47.1.0");
        QVERIFY(writeJson(
            QDir(root.path()).filePath("mrpack/modrinth.index.json"),
            QJsonObject{
                {"formatVersion", 1},
                {"game", "minecraft"},
                {"dependencies", QJsonObject{{"minecraft", "1.20.1"},
                                               {"fabric-loader", "0.15.0"}}},
                {"files", QJsonArray{}},
            }));

        const auto report = evaluateServerPack(root.path(), "1.20.2", "forge", "47.1.0");
        QVERIFY(!report.isIncompatible());
        QVERIFY(report.hasAdvisory(ServerPackIssueKind::VersionMismatch));
        QVERIFY(report.warnings.join('\n').contains("Minecraft"));
        QVERIFY(report.warnings.join('\n').contains("loader", Qt::CaseInsensitive));
        QVERIFY(report.warnings.join('\n').contains("installed", Qt::CaseInsensitive));
    }

    void readsCurseForgeDedicatedPackAndServerProjection()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.20.1", "fabric", "0.15.0");
        QVERIFY(writeJson(
            QDir(root.path()).filePath("flame/manifest.json"),
            QJsonObject{
                {"manifestType", "minecraftModpack"},
                {"manifestVersion", 1},
                {"minecraft", QJsonObject{
                    {"version", "1.20.1"},
                    {"modLoaders", QJsonArray{QJsonObject{{"id", "fabric-0.15.0"}}}},
                }},
            }));
        QVERIFY(writeFile(QDir(root.path()).filePath("server-pack/published-server-pack.txt"),
                          "curseforge\n"));
        QVERIFY(writeFile(QDir(root.path()).filePath("server-pack/client-only.txt"),
                          "mods/client.jar\n"));
        QVERIFY(writeFile(QDir(root.path()).filePath("server-pack/server-files/mods/server.jar"),
                          "server"));

        const auto report = inspectServerPack(root.path());
        QCOMPARE(report.provider, QString("curseforge"));
        QVERIFY(report.hasDedicatedServerPack);
        QCOMPARE(report.state, ServerPackCompatibilityState::KnownCompatible);
        QCOMPARE(findFile(report, "mods/client.jar")->side, ServerPackFileSide::ClientOnly);
        QCOMPARE(findFile(report, "mods/server.jar")->side, ServerPackFileSide::ServerOnly);
    }

    void validatesCurseForgeNeoForgeProjectionVersions()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.21.1", "neoforge", "21.1.100");
        QVERIFY(writeJson(
            QDir(root.path()).filePath("flame/manifest.json"),
            QJsonObject{
                {"manifestType", "minecraftModpack"},
                {"manifestVersion", 1},
                {"minecraft", QJsonObject{
                    {"version", "1.21.1"},
                    {"modLoaders", QJsonArray{QJsonObject{
                        {"id", "neoforge-1.21.1-21.1.100"}}}},
                }},
            }));
        QVERIFY(writeFile(QDir(root.path()).filePath("server-pack/include.txt"),
                          "mods/common.jar\n"));
        QVERIFY(writeFile(QDir(root.path()).filePath("server-pack/client-only.txt"),
                          "mods/client.jar\n"));

        const auto compatible = evaluateServerPack(
            root.path(), "1.21.1", "neoforge", "21.1.100");
        QCOMPARE(compatible.provider, QString("curseforge"));
        QCOMPARE(compatible.loaderType, QString("neoforge"));
        QCOMPARE(compatible.loaderVersion, QString("21.1.100"));
        QCOMPARE(compatible.state, ServerPackCompatibilityState::KnownCompatible);
        QCOMPARE(findFile(compatible, "mods/common.jar")->side,
                 ServerPackFileSide::Universal);
        QCOMPARE(findFile(compatible, "mods/client.jar")->side,
                 ServerPackFileSide::ClientOnly);

        const auto mismatch = evaluateServerPack(
            root.path(), "1.21.1", "neoforge", "21.1.101");
        QVERIFY(!mismatch.isIncompatible());
        QVERIFY(mismatch.hasAdvisory(ServerPackIssueKind::VersionMismatch));
        QVERIFY(mismatch.warnings.join('\n').contains("loader version", Qt::CaseInsensitive));
        QVERIFY(mismatch.warnings.join('\n').contains("installed", Qt::CaseInsensitive));
    }

    void validatesFtbAppProjectionAndRejectsUnknownLoader()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.20.1", "forge", "47.2.0");
        QVERIFY(writeJson(
            QDir(root.path()).filePath("minecraft/instance.json"),
            QJsonObject{{"mcVersion", "1.20.1"},
                        {"modLoader", "forge-47.2.0"}}));
        QVERIFY(writeFile(QDir(root.path()).filePath("server-pack/include.txt"),
                          "mods/common.jar\n"));
        QVERIFY(writeFile(QDir(root.path()).filePath("server-pack/client-only.txt"),
                          "mods/client.jar\n"));

        const auto compatible = evaluateServerPack(
            root.path(), "1.20.1", "forge", "47.2.0");
        QCOMPARE(compatible.provider, QString("ftb-app"));
        QCOMPARE(compatible.state, ServerPackCompatibilityState::KnownCompatible);
        QCOMPARE(findFile(compatible, "mods/common.jar")->side,
                 ServerPackFileSide::Universal);
        QCOMPARE(findFile(compatible, "mods/client.jar")->side,
                 ServerPackFileSide::ClientOnly);

        QVERIFY(writeJson(
            QDir(root.path()).filePath("minecraft/instance.json"),
            QJsonObject{{"mcVersion", "1.20.1"},
                        {"modLoader", "mystery-1.0"}}));
        const auto unknownLoader = inspectServerPack(root.path());
        QVERIFY(!unknownLoader.isIncompatible());
        QVERIFY(unknownLoader.hasAdvisory(ServerPackIssueKind::UnknownLoader));
        QVERIFY(unknownLoader.warnings.join('\n').contains("unknown loader", Qt::CaseInsensitive));
    }

    void rejectsConflictingProviderDocuments()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.20.1", "fabric", "0.15.0");
        QVERIFY(writeJson(
            QDir(root.path()).filePath("mrpack/modrinth.index.json"),
            QJsonObject{
                {"formatVersion", 1},
                {"game", "minecraft"},
                {"dependencies", QJsonObject{{"minecraft", "1.20.1"},
                                               {"fabric-loader", "0.15.0"}}},
                {"files", QJsonArray{}},
            }));
        QVERIFY(writeJson(
            QDir(root.path()).filePath("flame/manifest.json"),
            QJsonObject{
                {"manifestType", "minecraftModpack"},
                {"manifestVersion", 1},
                {"minecraft", QJsonObject{
                    {"version", "1.20.1"},
                    {"modLoaders", QJsonArray{QJsonObject{
                        {"id", "fabric-0.15.0"}}}},
                }},
            }));

        const auto report = inspectServerPack(root.path());
        QVERIFY(!report.isIncompatible());
        QVERIFY(report.hasAdvisory(ServerPackIssueKind::ConflictingProviders));
        QVERIFY(report.warnings.join('\n').contains("conflicting provider", Qt::CaseInsensitive));
        // Deterministic fallback: Modrinth wins over CurseForge.
        QCOMPARE(report.provider, QString("modrinth"));
        QCOMPARE(report.minecraftVersion, QString("1.20.1"));
    }

    void readsAtLauncherFtbTechnicAndLocalFallbackMetadata()
    {
        QTemporaryDir atlRoot;
        QVERIFY(atlRoot.isValid());
        writeComponents(atlRoot.path(), "1.12.2", "forge", "14.23.5.2859");
        QVERIFY(writeFile(QDir(atlRoot.path()).filePath("server-pack/provider.txt"),
                          "atlauncher\n"));
        QVERIFY(writeFile(QDir(atlRoot.path()).filePath("server-pack/client-only.txt"),
                          "mods/client.jar\n"));
        QVERIFY(writeFile(QDir(atlRoot.path()).filePath("server-pack/server-files/mods/server.jar"),
                          "server"));
        const auto atl = inspectServerPack(atlRoot.path());
        QCOMPARE(atl.provider, QString("atlauncher"));
        QCOMPARE(atl.state, ServerPackCompatibilityState::Unknown);
        QCOMPARE(findFile(atl, "mods/client.jar")->side, ServerPackFileSide::ClientOnly);
        QCOMPARE(findFile(atl, "mods/server.jar")->side, ServerPackFileSide::ServerOnly);

        QTemporaryDir ftbRoot;
        QVERIFY(ftbRoot.isValid());
        writeComponents(ftbRoot.path(), "1.19.2", "forge", "43.2.0");
        QVERIFY(writeFile(QDir(ftbRoot.path()).filePath("server-pack/provider.txt"),
                          "ftb\n"));
        QVERIFY(writeFile(QDir(ftbRoot.path()).filePath("server-pack/include.txt"),
                          "mods/common.jar\n"));
        const auto ftb = inspectServerPack(ftbRoot.path());
        QCOMPARE(ftb.provider, QString("ftb"));
        QCOMPARE(findFile(ftb, "mods/common.jar")->side, ServerPackFileSide::Universal);

        QTemporaryDir appRoot;
        QVERIFY(appRoot.isValid());
        writeComponents(appRoot.path(), "1.20.1", "fabric", "0.15.0");
        QVERIFY(writeJson(QDir(appRoot.path()).filePath("minecraft/instance.json"),
                          QJsonObject{{"mcVersion", "1.20.1"},
                                      {"modLoader", "fabric-0.15.0"}}));
        const auto ftbApp = inspectServerPack(appRoot.path());
        QCOMPARE(ftbApp.provider, QString("ftb-app"));
        QCOMPARE(ftbApp.state, ServerPackCompatibilityState::Unknown);

        QTemporaryDir technicRoot;
        QVERIFY(technicRoot.isValid());
        writeComponents(technicRoot.path(), "1.7.10", "forge", "10.13.4.1614");
        QVERIFY(writeJson(QDir(technicRoot.path()).filePath("minecraft/bin/version.json"),
                          QJsonObject{{"inheritsFrom", "1.7.10"},
                                      {"libraries", QJsonArray{QJsonObject{
                                          {"name", "net.minecraftforge:forge:10.13.4.1614"}}}}}));
        const auto technic = inspectServerPack(technicRoot.path());
        QCOMPARE(technic.provider, QString("technic"));
        QCOMPARE(technic.state, ServerPackCompatibilityState::Unknown);
        QCOMPARE(technic.loaderType, QString("forge"));

        QTemporaryDir localRoot;
        QVERIFY(localRoot.isValid());
        writeComponents(localRoot.path(), "1.20.1", "fabric", "0.15.0");
        const auto local = inspectServerPack(localRoot.path());
        QCOMPARE(local.provider, QString("local/custom"));
        QCOMPARE(local.state, ServerPackCompatibilityState::Unknown);
        QVERIFY(!local.warnings.isEmpty());
    }

    void normalizedProviderProjectionFiltersFiles_data()
    {
        QTest::addColumn<QString>("provider");

        QTest::newRow("atlauncher") << QStringLiteral("atlauncher");
        QTest::newRow("ftb") << QStringLiteral("ftb");
        QTest::newRow("legacy-ftb") << QStringLiteral("ftb-legacy");
        QTest::newRow("technic") << QStringLiteral("technic");
        QTest::newRow("local-custom") << QStringLiteral("local/custom");
    }

    void normalizedProviderProjectionFiltersFiles()
    {
        QFETCH(QString, provider);

        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString gameRoot = QDir(root.path()).filePath("minecraft");
        writeComponents(root.path(), "1.20.1", "forge", "47.2.0");
        if (provider != QStringLiteral("local/custom")) {
            QVERIFY(writeFile(QDir(root.path()).filePath("server-pack/provider.txt"),
                              provider.toUtf8() + '\n'));
        }
        QVERIFY(writeFile(QDir(root.path()).filePath("server-pack/include.txt"),
                          "mods/common.jar\n"));
        QVERIFY(writeFile(QDir(root.path()).filePath("server-pack/client-only.txt"),
                          "mods/client.jar\n"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/common.jar"), "common"));
        QVERIFY(writeFile(QDir(gameRoot).filePath("mods/client.jar"), "client"));

        const auto report = inspectServerPack(root.path());
        QCOMPARE(report.provider, provider);
        QCOMPARE(findFile(report, "mods/common.jar")->side,
                 ServerPackFileSide::Universal);
        QCOMPARE(findFile(report, "mods/client.jar")->side,
                 ServerPackFileSide::ClientOnly);

        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        QStringList skipped;
        QString error;
        QVERIFY2(ServerModpackInstaller::prepareContent(
                     root.path(), gameRoot, destination.path(), &skipped, &error),
                 qPrintable(error));
        QVERIFY(QFileInfo::exists(
            QDir(destination.path()).filePath("mods/common.jar")));
        QVERIFY(!QFileInfo::exists(
            QDir(destination.path()).filePath("mods/client.jar")));
        QVERIFY(skipped.contains("mods/client.jar"));
    }

    void unsafePathsAndInvalidHashesAreRejected()
    {
        QTemporaryDir unsafeRoot;
        QVERIFY(unsafeRoot.isValid());
        QVERIFY(writeJson(
            QDir(unsafeRoot.path()).filePath("mrpack/modrinth.index.json"),
            QJsonObject{
                {"formatVersion", 1},
                {"game", "minecraft"},
                {"files", QJsonArray{
                    QJsonObject{{"path", "../outside.jar"}},
                }},
            }));

        const auto unsafeReport = inspectServerPack(unsafeRoot.path());
        QVERIFY(unsafeReport.isIncompatible());
        QVERIFY(unsafeReport.hasBlocker(ServerPackIssueKind::UnsafePath));
        QVERIFY(serverPackCompatibilityDescription(unsafeReport).contains("unsafe server file path"));

        QTemporaryDir hashRoot;
        QVERIFY(hashRoot.isValid());
        writeComponents(hashRoot.path(), "1.20.1", "fabric", "0.15.0");
        QVERIFY(writeJson(
            QDir(hashRoot.path()).filePath("mrpack/modrinth.index.json"),
            QJsonObject{
                {"formatVersion", 1},
                {"game", "minecraft"},
                {"dependencies", QJsonObject{{"minecraft", "1.20.1"},
                                               {"fabric-loader", "0.15.0"}}},
                {"files", QJsonArray{
                    QJsonObject{{"path", "mods/bad-hash.jar"},
                                {"hashes", QJsonObject{{"sha256", "not-a-hash"}}}},
                    QJsonObject{{"path", "mods/good.jar"},
                                {"env", QJsonObject{{"client", "required"},
                                                      {"server", "required"}}}},
                }},
            }));
        const auto hashReport = inspectServerPack(hashRoot.path());
        QVERIFY(!hashReport.isIncompatible());
        QVERIFY(hashReport.hasAdvisory(ServerPackIssueKind::UnverifiedIntegrity));
        QVERIFY(hashReport.warnings.join('\n').contains("invalid sha256", Qt::CaseInsensitive));
        const auto *badFile = findFile(hashReport, "mods/bad-hash.jar");
        QVERIFY(badFile);
        QCOMPARE(badFile->integrity, ServerPackIntegrityState::Unverified);
    }

    void malformedProviderMetadataIsRejected()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.20.1", "fabric", "0.15.0");
        QVERIFY(writeFile(QDir(root.path()).filePath("mrpack/modrinth.index.json"),
                          "{ malformed"));
        const auto report = inspectServerPack(root.path());
        QVERIFY(!report.isIncompatible());
        QVERIFY(report.hasAdvisory(ServerPackIssueKind::MalformedMetadata));
        QVERIFY(report.warnings.join('\n').contains("malformed", Qt::CaseInsensitive));
        // Malformed optional metadata falls back safely: no trusted file list.
        QVERIFY(report.files.isEmpty());
    }

    void unknownLoaderMetadataIsRejected()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.20.1", "fabric", "0.15.0");
        QVERIFY(writeJson(
            QDir(root.path()).filePath("mrpack/modrinth.index.json"),
            QJsonObject{
                {"formatVersion", 1},
                {"game", "minecraft"},
                {"dependencies", QJsonObject{{"minecraft", "1.20.1"},
                                               {"custom-loader", "1.0"}}},
                {"files", QJsonArray{}},
            }));

        const auto report = inspectServerPack(root.path());
        QVERIFY(!report.isIncompatible());
        QVERIFY(report.hasAdvisory(ServerPackIssueKind::UnknownLoader));
        QVERIFY(report.warnings.join('\n').contains("unknown loader", Qt::CaseInsensitive));
    }

    void technicForgeCoordinateVersionsNormalizeNarrowly()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeComponents(root.path(), "1.7.10", "forge", "10.13.4.1614");
        QVERIFY(writeJson(QDir(root.path()).filePath("minecraft/bin/version.json"),
                          QJsonObject{{"inheritsFrom", "1.7.10"},
                                      {"libraries", QJsonArray{QJsonObject{
                                          {"name", "net.minecraftforge:forge:1.7.10-10.13.4.1614-1.7.10"}}}}}));
        QVERIFY(writeFile(QDir(root.path()).filePath("server-pack/include.txt"),
                          "mods/common.jar\n"));

        const auto compatible = evaluateServerPack(
            root.path(), "1.7.10", "forge", "10.13.4.1614");
        QVERIFY(!compatible.isIncompatible());
        QCOMPARE(compatible.state, ServerPackCompatibilityState::KnownCompatible);
        const auto mismatch = evaluateServerPack(
            root.path(), "1.7.10", "forge", "10.13.4.1615");
        QVERIFY(!mismatch.isIncompatible());
        QVERIFY(mismatch.hasAdvisory(ServerPackIssueKind::VersionMismatch));
        QVERIFY(mismatch.warnings.join('\n').contains("loader version", Qt::CaseInsensitive));
    }

    void curseForgeHashMappingIsStrictAndAttachesValidators()
    {
        const QByteArray actual("fixture-bytes");
        const QString sha1 = QCryptographicHash::hash(actual, QCryptographicHash::Sha1).toHex();
        const QString md5 = QCryptographicHash::hash(actual, QCryptographicHash::Md5).toHex();

        const auto parsedSha1 = Flame::parseCurseForgeHash(
            QJsonObject{{"algo", 1}, {"value", sha1}});
        QVERIFY(parsedSha1);
        QCOMPARE(parsedSha1->algorithmName, QString("sha1"));
        const auto parsedMd5 = Flame::parseCurseForgeHash(
            QJsonObject{{"algo", 2}, {"value", md5}});
        QVERIFY(parsedMd5);
        QCOMPARE(parsedMd5->algorithmName, QString("md5"));

        for (const QJsonObject &invalid : {
                 QJsonObject{{"value", sha1}},
                 QJsonObject{{"algo", 3}, {"value", sha1}},
                 QJsonObject{{"algo", 1}, {"value", "not-a-hash"}},
                 QJsonObject{{"algo", "1"}, {"value", sha1}},
             }) {
            QVERIFY(!Flame::parseCurseForgeHash(invalid));
        }
        QVERIFY(!Flame::createCurseForgeChecksumValidator("murmur2", sha1));
        QVERIFY(!Flame::createCurseForgeChecksumValidator({}, {}));

        for (const auto &pair : { std::pair{QString("sha1"), sha1},
                                  std::pair{QString("md5"), md5} }) {
            std::unique_ptr<Net::ChecksumValidator> validator(
                Flame::createCurseForgeChecksumValidator(pair.first, pair.second));
            QVERIFY(validator);
            QNetworkRequest request(QUrl(QStringLiteral("https://fixture.invalid/file")));
            QVERIFY(validator->init(request));
            QByteArray chunk = actual;
            QVERIFY(validator->write(chunk));
            FixtureReply reply;
            QVERIFY(validator->validate(reply));
        }
    }

    void checksumValidatorRejectsDeterministicMismatch()
    {
        const QByteArray actual("fixture-bytes");
        const QByteArray expected = QCryptographicHash::hash(
            QByteArray("different-bytes"), QCryptographicHash::Sha256);
        Net::ChecksumValidator validator(QCryptographicHash::Sha256, expected);
        QNetworkRequest request(QUrl(QStringLiteral("https://fixture.invalid/file")));
        QVERIFY(validator.init(request));
        QByteArray chunk = actual;
        QVERIFY(validator.write(chunk));
        FixtureReply reply;
        QVERIFY(!validator.validate(reply));
    }
};

QTEST_GUILESS_MAIN(ServerPackCompatibilityTest)

#include "ServerPackCompatibility_test.moc"
