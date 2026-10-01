// SPDX-License-Identifier: GPL-3.0-only

// Helpers moved out of the Server Manager page into launcher/server.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <archive/ArchiveWriter.h>
#include <server/ServerDiagnostics.h>
#include <server/ServerDownloaderShared.h>
#include <server/ServerFiles.h>
#include <server/ServerPaths.h>
#include <server/ServerProcessStats.h>

namespace {
bool writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

bool writeJar(const QString& path, const QString& metadataPath, const QByteArray& metadata)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    MMCZip::ArchiveWriter archive(path);
    return archive.open() && archive.addFile(metadataPath, metadata) && archive.close();
}
}  // namespace

class ServerToolsTest : public QObject {
    Q_OBJECT

private slots:
    void describesContentFileNames()
    {
        const auto sodium = ServerFiles::describeContentFile(QStringLiteral("sodium-fabric-0.5.8.jar"));
        QCOMPARE(sodium.name, QStringLiteral("sodium fabric"));
        QCOMPARE(sodium.version, QStringLiteral("0.5.8"));
        QVERIFY(sodium.enabled);

        const auto disabled = ServerFiles::describeContentFile(QStringLiteral("Jade_1.21-15.1.jar.disabled"));
        QCOMPARE(disabled.name, QStringLiteral("Jade"));
        QCOMPARE(disabled.version, QStringLiteral("1.21-15.1"));
        QVERIFY(!disabled.enabled);

        const auto unversioned = ServerFiles::describeContentFile(QStringLiteral("worldedit.jar"));
        QCOMPARE(unversioned.name, QStringLiteral("worldedit"));
        QVERIFY(unversioned.version.isEmpty());
    }

    void formatsByteSizes()
    {
        QCOMPARE(ServerFiles::formatByteSize(512), QStringLiteral("512 B"));
        QCOMPARE(ServerFiles::formatByteSize(1536), QStringLiteral("1.5 KB"));
        QCOMPARE(ServerFiles::formatByteSize(5ll * 1024 * 1024), QStringLiteral("5.0 MB"));
        QCOMPARE(ServerFiles::formatByteSize(3ll * 1024 * 1024 * 1024), QStringLiteral("3.0 GB"));
    }

    void measuresFolderSizesIncludingSubfolders()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(writeFile(root.filePath("a.txt"), QByteArray(10, 'a')));
        QVERIFY(writeFile(root.filePath("world/region/r.0.0.mca"), QByteArray(90, 'b')));
        QCOMPARE(ServerFiles::directorySize(root.path()), qint64(100));
        QCOMPARE(ServerFiles::directorySize(root.filePath("missing")), qint64(0));
    }

    void readsModIdsFromEveryLoaderFormat()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString fabric = root.filePath("fabric.jar");
        QVERIFY(writeJar(fabric, QStringLiteral("fabric.mod.json"),
                         R"({"id":"Sodium","provides":["embeddium"]})"));
        QCOMPARE(ServerDiagnostics::modIdsFromJar(fabric),
                 (QStringList{ QStringLiteral("sodium"), QStringLiteral("embeddium") }));

        const QString forge = root.filePath("forge.jar");
        QVERIFY(writeJar(forge, QStringLiteral("META-INF/mods.toml"),
                         "[[mods]]\nmodId=\"create\"\n[[mods]]\n  modId = 'flywheel'\n"));
        QCOMPARE(ServerDiagnostics::modIdsFromJar(forge),
                 (QStringList{ QStringLiteral("create"), QStringLiteral("flywheel") }));

        const QString quilt = root.filePath("quilt.jar");
        QVERIFY(writeJar(quilt, QStringLiteral("quilt.mod.json"), R"({"quilt_loader":{"id":"qsl"}})"));
        QCOMPARE(ServerDiagnostics::modIdsFromJar(quilt), QStringList{ QStringLiteral("qsl") });

        QVERIFY(writeFile(root.filePath("plain.jar"), "not a zip"));
        QVERIFY(ServerDiagnostics::modIdsFromJar(root.filePath("plain.jar")).isEmpty());
    }

    void findsTheModFileACrashNames()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString mods = root.filePath("mods");
        QVERIFY(writeJar(QDir(mods).filePath("broken-1.0.jar"), QStringLiteral("META-INF/mods.toml"),
                         "modId=\"brokenmod\"\n"));
        QVERIFY(writeJar(QDir(mods).filePath("fine-1.0.jar"), QStringLiteral("META-INF/mods.toml"),
                         "modId=\"finemod\"\n"));

        const QString log = QStringLiteral("Crash report\n-- MOD brokenmod --\nDetails: failed\n");
        const QStringList suspects = ServerDiagnostics::suspectedModFiles(mods, log);
        QCOMPARE(suspects.size(), 1);
        QCOMPARE(QFileInfo(suspects.first()).fileName(), QStringLiteral("broken-1.0.jar"));
        QVERIFY(ServerDiagnostics::suspectedModFiles(mods, QStringLiteral("no mod named")).isEmpty());
    }

    void checksRelativePathsFromOutsideTheLauncher()
    {
        QCOMPARE(ServerPaths::normalizedRelativePath(QStringLiteral("  ./mods\\a.jar ")), QStringLiteral("mods/a.jar"));
        QCOMPARE(ServerPaths::normalizedRelativePath(QStringLiteral("config/../mods/a.jar")), QStringLiteral("mods/a.jar"));
        QVERIFY(ServerPaths::isSafeRelativePath(QStringLiteral("mods/a.jar")));
        QVERIFY(ServerPaths::isSafeRelativePath(QStringLiteral("mods/Mod..Extras.jar")));
        QVERIFY(ServerPaths::isSafeRelativePath(QStringLiteral("config/../mods/a.jar")));
        QVERIFY(!ServerPaths::isSafeRelativePath(QStringLiteral("../outside.txt")));
        QVERIFY(!ServerPaths::isSafeRelativePath(QStringLiteral("mods/../../outside.txt")));
        QVERIFY(!ServerPaths::isSafeRelativePath(QStringLiteral("..")));
        QVERIFY(!ServerPaths::isSafeRelativePath(QStringLiteral("C:/Windows/system.ini")));
        QVERIFY(!ServerPaths::isSafeRelativePath(QStringLiteral("/etc/passwd")));
        QVERIFY(!ServerPaths::isSafeRelativePath(QStringLiteral("   ")));

        QString bad;
        QVERIFY(ServerPaths::hasValidWindowsNames(QStringLiteral("mods//config/server.properties"), &bad));
        for (const QString &path : { QStringLiteral("mods/CON.jar"), QStringLiteral("lpt9/a"),
                                     QStringLiteral("mods/a?.jar"), QStringLiteral("mods/trailing."),
                                     QStringLiteral("mods/space "), QStringLiteral("./a"),
                                     QStringLiteral("mods/a\x01.jar") }) {
            bad.clear();
            QVERIFY2(!ServerPaths::hasValidWindowsNames(path, &bad), qPrintable(path));
            QVERIFY(!bad.isEmpty());
        }
    }

    void recognizesAndChecksSha1Checksums()
    {
        const QByteArray valid("da39a3ee5e6b4b0d3255bfef95601890afd80709");  // SHA-1 of nothing
        QVERIFY(ServerDownloaderDetail::isSha1(valid));
        QVERIFY(ServerDownloaderDetail::isSha1(valid.toUpper()));
        QVERIFY(!ServerDownloaderDetail::isSha1(valid.left(39)));
        QVERIFY(!ServerDownloaderDetail::isSha1(valid + "0"));
        QVERIFY(!ServerDownloaderDetail::isSha1(QByteArray("zz39a3ee5e6b4b0d3255bfef95601890afd80709")));
        QVERIFY(!ServerDownloaderDetail::isSha1(QByteArray("<html>not found</html>")));
        QVERIFY(!ServerDownloaderDetail::isSha1(QByteArray()));

        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(writeFile(root.filePath("empty.bin"), QByteArray()));
        QVERIFY(ServerDownloaderDetail::fileMatchesSha1(root.filePath("empty.bin"), valid.toUpper()));
        QVERIFY(!ServerDownloaderDetail::fileMatchesSha1(root.filePath("empty.bin"), QByteArray()));
        QVERIFY(!ServerDownloaderDetail::fileMatchesSha1(root.filePath("missing.bin"), valid));
    }

    void readsTheLoadOfARunningProcess()
    {
#ifdef Q_OS_WIN
        const qint64 self = QCoreApplication::applicationPid();
        ServerProcessSnapshot snapshot;
        QVERIFY(ServerProcessStats::read(self, &snapshot));
        QVERIFY(snapshot.workingSetBytes > 0);
        // This test process is not Java and starts no Java child, so it is its own work process.
        QCOMPARE(ServerProcessStats::workProcessId(self), self);
        QVERIFY(!ServerProcessStats::read(0, &snapshot));
        QCOMPARE(ServerProcessStats::workProcessId(0), qint64(0));
#else
        QSKIP("Process statistics are only read on Windows.");
#endif
    }
};

QTEST_GUILESS_MAIN(ServerToolsTest)

#include "ServerTools_test.moc"
