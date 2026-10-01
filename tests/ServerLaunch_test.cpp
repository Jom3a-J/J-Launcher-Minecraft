// SPDX-License-Identifier: GPL-3.0-only

// Starting and stopping servers: launch commands, JVM options, restarts, timeouts and Java choice.

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <utility>

#include <archive/ArchiveWriter.h>
#include <server/ServerDownloader.h>
#include <server/ServerContentUpdater.h>
#include <server/ServerInstance.h>
#include <server/ServerDiagnostics.h>
#include <server/ServerJvmArgs.h>
#include <java/JavaRuntimeInstallTask.h>
#include <java/JavaUtils.h>
#include <server/ServerManager.h>
#include <server/ServerProperties.h>
#include <net/HostScheduler.h>
#include <net/PartFile.h>
#include <net/SegmentedDownload.h>

#include "RangeHttpServer.h"

#include "ServerTestSupport.h"

using namespace ServerTestSupport;

class ServerLaunchTest : public QObject {
    Q_OBJECT

   private slots:
    void init()
    {
        ServerInstance::clearJavaProbeCacheForTesting();
    }

    void rejectsAnOccupiedPortBeforeStartingJava()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        QTcpServer occupiedPort;
        QVERIFY(occupiedPort.listen(QHostAddress::LocalHost, 0));

        ServerInstance server("occupied-port", "Occupied port");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        server.setEulaAccepted(true);
        server.setPort(occupiedPort.serverPort());
        server.setJavaPath(temporaryRoot.filePath("java-that-must-not-run.exe"));
        QSignalSpy errors(&server, &ServerInstance::serverError);

        QVERIFY(!server.start());
        QCOMPARE(server.status(), ServerStatus::Error);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains(QString::number(occupiedPort.serverPort())));
        QVERIFY(server.consoleLog().contains("already in use"));
    }

    void runsCommandsRestartsAndStopsSyntheticServer()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("lifecycle", "Lifecycle");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server")));
        QSignalSpy started(&server, &ServerInstance::started);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QCOMPARE(started.size(), 1);

        server.writeStdin("say phase-four");
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:say phase-four"), 5000);

        QVERIFY(server.restart());
        QTRY_VERIFY_WITH_TIMEOUT(started.size() >= 2, 5000);
        QCOMPARE(server.status(), ServerStatus::Running);

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void sendsValidatedPlayerAdministrationCommandsWhileRunning()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("live-player-admin", "Live player admin");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server")));

        QString error;
        QVERIFY(!server.setPlayerWhitelistedLive("TestPlayer", true, &error));
        QVERIFY(error.contains("running"));
        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);

        error.clear();
        QVERIFY2(server.setPlayerWhitelistedLive("TestPlayer", true, &error), qPrintable(error));
        QVERIFY2(server.setPlayerOperatorLive("TestPlayer", true, &error), qPrintable(error));
        QVERIFY2(server.setPlayerBannedLive("TestPlayer", true, "Testing ban", &error), qPrintable(error));
        QVERIFY2(server.clearPlayerAccessLive("TestPlayer", &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:whitelist add TestPlayer"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:op TestPlayer"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:ban TestPlayer Testing ban"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:whitelist remove TestPlayer"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:deop TestPlayer"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("COMMAND:pardon TestPlayer"), 5000);

        error.clear();
        QVERIFY(!server.setPlayerOperatorLive("bad player", true, &error));
        QVERIFY(error.contains("invalid"));
        QVERIFY(!server.consoleLog().contains("COMMAND:op bad player"));

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void appliesConfiguredJavaMemoryAndArgumentsToGeneratedNeoForgeLaunch()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString serverDirectory = temporaryRoot.filePath("server");
#ifdef Q_OS_WIN
        const QString scriptName = QStringLiteral("run.bat");
        const QString loaderArgumentsRelative = QStringLiteral(
            "libraries/net/neoforged/neoforge/21.1.233/win_args.txt");
        const QByteArray launchScript =
            "@echo off\r\njava @user_jvm_args.txt "
            "@libraries/net/neoforged/neoforge/21.1.233/win_args.txt %*\r\n";
#else
        const QString scriptName = QStringLiteral("run.sh");
        const QString loaderArgumentsRelative = QStringLiteral(
            "libraries/net/neoforged/neoforge/21.1.233/unix_args.txt");
        const QByteArray launchScript =
            "#!/bin/sh\njava @user_jvm_args.txt "
            "@libraries/net/neoforged/neoforge/21.1.233/unix_args.txt \"$@\"\n";
#endif
        const QString loaderArguments =
            QDir(serverDirectory).filePath(loaderArgumentsRelative);

        ServerInstance server("neoforge-launch", "NeoForge launch");
        server.setServerDirectory(serverDirectory);
        server.setVersion("1.21.1");
        server.setLoaderType("neoforge");
        server.setLoaderVersion("21.1.233");
        server.setPort(unusedPort());
        server.setEulaAccepted(true);
        server.setJavaPath(fakeMinecraftServerPath());
        server.setMinMemory(1536);
        server.setMaxMemory(6144);
        server.setExtraJvmArguments("-Dexample=true -XX:+UseG1GC");

        QVERIFY(writeFile(QDir(serverDirectory).filePath(scriptName), launchScript));
        QVERIFY(writeFile(QDir(serverDirectory).filePath("user_jvm_args.txt"), "# pack options\n"));
        QVERIFY(writeFile(loaderArguments, "# synthetic loader arguments\n"));
        QVERIFY(writeArchive(QDir(serverDirectory).filePath("server.jar"), {
            { "META-INF/MANIFEST.MF", "Main-Class: net.minecraft.bundler.Main\n" },
        }));

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        // The supplied file is preserved and never passed directly; the
        // launcher-owned effective file carries the filtered options.
        QTRY_VERIFY_WITH_TIMEOUT(
            server.consoleLog().contains("ARG:@" + ServerJvmArgs::effectiveFileName()), 5000);
        QVERIFY(!server.consoleLog().contains("ARG:@user_jvm_args.txt"));
        QVERIFY(server.consoleLog().contains("ARG:-Xmx6144M"));
        QVERIFY(server.consoleLog().contains("ARG:-Xms1536M"));
        QVERIFY(server.consoleLog().contains("ARG:-Dexample=true"));
        QVERIFY(server.consoleLog().contains("ARG:-XX:+UseG1GC"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.language=en"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.country=US"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.language.format=en"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.country.format=US"));
        QVERIFY(server.consoleLog().contains("ARG:@" + loaderArgumentsRelative));
        QVERIFY(server.consoleLog().contains("[JVM] Using dedicated-server locale en_US"));
        QCOMPARE(readFile(QDir(serverDirectory).filePath("user_jvm_args.txt")),
                 QByteArray("# pack options\n"));
        QVERIFY(QFileInfo::exists(
            QDir(serverDirectory).filePath(ServerJvmArgs::effectiveFileName())));

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void preservesCustomForgeScriptAndInjectsConfiguredJvmSettings_data()
    {
        QTest::addColumn<bool>("providerStartScript");
        QTest::newRow("generated run script") << false;
        QTest::newRow("ServerPackCreator start script") << true;
    }

    void preservesCustomForgeScriptAndInjectsConfiguredJvmSettings()
    {
        QFETCH(bool, providerStartScript);
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString serverDirectory = temporaryRoot.filePath("server");
        const QString fakeServer = QDir::toNativeSeparators(fakeMinecraftServerPath());

        ServerInstance server("custom-forge-launch", "Custom Forge launch");
        server.setServerDirectory(serverDirectory);
        server.setVersion("1.21.1");
        server.setLoaderType("forge");
        server.setPort(unusedPort());
        server.setEulaAccepted(true);
        server.setJavaPath(fakeMinecraftServerPath());
        server.setMinMemory(2048);
        server.setMaxMemory(4096);
        server.setExtraJvmArguments("-Dcustom=true");

        const QByteArray script = QString(
#ifdef Q_OS_WIN
            "@echo off\r\necho JVM_OPTIONS:%JAVA_TOOL_OPTIONS%\r\n\"%1\" %*\r\n")
#else
            "#!/bin/sh\nprintf 'JVM_OPTIONS:%s\\n' \"$JAVA_TOOL_OPTIONS\"\n\"%1\" \"$@\"\n")
#endif
                                      .arg(fakeServer)
                                      .toUtf8();
#ifdef Q_OS_WIN
        const QString scriptName = providerStartScript
            ? QStringLiteral("start.bat") : QStringLiteral("run.bat");
#else
        const QString scriptName = providerStartScript
            ? QStringLiteral("start.sh") : QStringLiteral("run.sh");
#endif
        QVERIFY(writeFile(QDir(serverDirectory).filePath(scriptName), script));

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("JVM_OPTIONS:"), 5000);
        QVERIFY(server.consoleLog().contains("-Xmx4096M"));
        QVERIFY(server.consoleLog().contains("-Xms2048M"));
        QVERIFY(server.consoleLog().contains("-Dcustom=true"));
        // Opaque wrappers cannot be filtered safely, so locale plus the
        // narrow IgnoreUnrecognizedVMOptions fallback travel via the
        // environment. The wrapper script itself must remain untouched.
        QVERIFY(server.consoleLog().contains("-Duser.language=en"));
        QVERIFY(server.consoleLog().contains("-Duser.country=US"));
        QVERIFY(server.consoleLog().contains("-Duser.language.format=en"));
        QVERIFY(server.consoleLog().contains("-XX:+IgnoreUnrecognizedVMOptions"));
        QVERIFY(server.consoleLog().contains("[JVM] Using dedicated-server locale en_US"));
        QVERIFY(server.consoleLog().contains("opaque wrapper"));
        QCOMPARE(readFile(QDir(serverDirectory).filePath(scriptName)), script);

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void prefersGeneratedRunScriptWhenBothWrapperNamesExist()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString serverDirectory = temporaryRoot.filePath("server");
        const QString fakeServer = QDir::toNativeSeparators(fakeMinecraftServerPath());

        ServerInstance server("preferred-forge-wrapper", "Preferred Forge wrapper");
        server.setServerDirectory(serverDirectory);
        server.setVersion("1.21.1");
        server.setLoaderType("forge");
        server.setPort(unusedPort());
        server.setEulaAccepted(true);
        server.setJavaPath(fakeMinecraftServerPath());

#ifdef Q_OS_WIN
        const QString runName = QStringLiteral("run.bat");
        const QString startName = QStringLiteral("start.bat");
        const QByteArray runScript = QString(
            "@echo off\r\necho SELECTED:run\r\n\"%1\" %*\r\n").arg(fakeServer).toUtf8();
        const QByteArray startScript = QString(
            "@echo off\r\necho SELECTED:start\r\n\"%1\" %*\r\n").arg(fakeServer).toUtf8();
#else
        const QString runName = QStringLiteral("run.sh");
        const QString startName = QStringLiteral("start.sh");
        const QByteArray runScript = QString(
            "#!/bin/sh\nprintf 'SELECTED:run\\n'\n\"%1\" \"$@\"\n").arg(fakeServer).toUtf8();
        const QByteArray startScript = QString(
            "#!/bin/sh\nprintf 'SELECTED:start\\n'\n\"%1\" \"$@\"\n").arg(fakeServer).toUtf8();
#endif
        QVERIFY(writeFile(QDir(serverDirectory).filePath(runName), runScript));
        QVERIFY(writeFile(QDir(serverDirectory).filePath(startName), startScript));

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.consoleLog().contains("SELECTED:run"), 5000);
        QVERIFY(!server.consoleLog().contains("SELECTED:start"));
        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void startsCustomForgeWrapperWithExplicitRelativePath()
    {
#ifndef Q_OS_WIN
        QSKIP("The cmd.exe custom-wrapper path is Windows-specific.");
#else
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString serverDirectory = temporaryRoot.filePath("server");
        QVERIFY(writeFile(QDir(serverDirectory).filePath(QStringLiteral("run.bat")),
                          "@echo off\r\necho [Server thread/INFO]: Done (0.1s)! For help, type \"help\"\r\nset /p line=\r\n"));

        ServerInstance server("custom-forge-wrapper-env", "Custom Forge wrapper environment");
        server.setServerDirectory(serverDirectory);
        server.setVersion("1.21.1");
        server.setLoaderType("forge");
        server.setPort(unusedPort());
        server.setEulaAccepted(true);
        server.setJavaPath(fakeMinecraftServerPath());

        ScopedEnvironmentVariable currentDirectorySearch(
            "NoDefaultCurrentDirectoryInExePath", "1");
        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QVERIFY(server.consoleLog().contains(
            "[Server thread/INFO]: Done (0.1s)! For help, type \"help\""));
        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
#endif
    }

    void filtersJava21OnlyOptionsByDetectedRuntime()
    {
        const QString content = QStringLiteral(
            "# Requiem-style supplied options\n"
            "-Xms9G\n"
            "-XX:+ZGenerational\n"
            "-XX:+UseG1GC\n"
            "-Dfile.encoding=UTF-8\n");
        const ServerJvmFilterResult java17 = ServerJvmArgs::prepareContent(content, 17);
        QVERIFY(java17.ok);
        QVERIFY(java17.keptTokens.contains("-XX:+UseG1GC"));
        QVERIFY(java17.keptTokens.contains("-Dfile.encoding=UTF-8"));
        QVERIFY(!java17.keptTokens.contains("-Xms9G"));
        QVERIFY(!java17.keptTokens.contains("-XX:+ZGenerational"));
        QVERIFY(java17.removedMemoryOptions.contains("-Xms9G"));
        QVERIFY(java17.removedUnsupportedOptions.contains("-XX:+ZGenerational"));

        const ServerJvmFilterResult java21 = ServerJvmArgs::prepareContent(content, 21);
        QVERIFY(java21.ok);
        QVERIFY(java21.keptTokens.contains("-XX:+ZGenerational"));
        QVERIFY(java21.keptTokens.contains("-XX:+UseG1GC"));
        QVERIFY(java21.removedMemoryOptions.contains("-Xms9G"));
        QVERIFY(java21.removedUnsupportedOptions.isEmpty());
    }

    void removesMemoryOptionsInAllAcceptedSpellings()
    {
        const QStringList tokens{
            QStringLiteral("-Xms9G"),           QStringLiteral("-Xmx4G"),
            QStringLiteral("-Xms512M"),         QStringLiteral("-Xmx2048m"),
            QStringLiteral("-Xms1024k"),        QStringLiteral("-Xmx1G"),
            QStringLiteral("-Xms"),             QStringLiteral("2G"),
            QStringLiteral("-Xmx"),             QStringLiteral("4G"),
            QStringLiteral("-XX:InitialHeapSize=1G"), QStringLiteral("-XX:MaxHeapSize=2G"),
            QStringLiteral("-XX:+UseG1GC"),     QStringLiteral("-Dexample=true"),
        };
        for (int javaMajor : { 17, 21 }) {
            const ServerJvmFilterResult filtered = ServerJvmArgs::filterTokens(tokens, javaMajor);
            QVERIFY(filtered.ok);
            QCOMPARE(filtered.keptTokens,
                     QStringList({ "-XX:+UseG1GC", "-Dexample=true" }));
            QVERIFY(!filtered.keptTokens.join(' ').contains("-Xms"));
            QVERIFY(!filtered.keptTokens.join(' ').contains("-Xmx"));
            QVERIFY(!filtered.keptTokens.join(' ').contains("HeapSize"));
            QVERIFY(filtered.removedMemoryOptions.contains("-Xms9G"));
            QVERIFY(filtered.removedMemoryOptions.contains("-Xmx4G"));
            // Bare flags with a valid size are recorded as one combined
            // expression, not as two separate removed options.
            QVERIFY(filtered.removedMemoryOptions.contains("-Xms 2G"));
            QVERIFY(filtered.removedMemoryOptions.contains("-Xmx 4G"));
            QVERIFY(!filtered.removedMemoryOptions.contains("2G"));
            QVERIFY(!filtered.removedMemoryOptions.contains("4G"));
            QVERIFY(filtered.removedMemoryOptions.contains("-XX:InitialHeapSize=1G"));
            QVERIFY(filtered.removedMemoryOptions.contains("-XX:MaxHeapSize=2G"));
        }
    }

    void bareMemoryFlagsPreserveFollowingJvmOptions()
    {
        QVERIFY(ServerJvmArgs::isHeapSizeValue("2G"));
        QVERIFY(ServerJvmArgs::isHeapSizeValue("2048M"));
        QVERIFY(ServerJvmArgs::isHeapSizeValue("512k"));
        QVERIFY(ServerJvmArgs::isHeapSizeValue("1024"));
        QVERIFY(!ServerJvmArgs::isHeapSizeValue("-XX:+UseG1GC"));
        QVERIFY(!ServerJvmArgs::isHeapSizeValue("-Dfoo=bar"));
        QVERIFY(!ServerJvmArgs::isHeapSizeValue(""));

        // "-Xms" followed by another option must not swallow that option.
        const ServerJvmFilterResult followedByOption = ServerJvmArgs::filterTokens(
            { "-Xms", "-XX:+UseG1GC", "-Dfoo=bar" }, 17);
        QVERIFY(followedByOption.ok);
        QVERIFY(followedByOption.removedMemoryOptions.contains("-Xms"));
        QVERIFY(!followedByOption.removedMemoryOptions.join('|').contains("UseG1GC"));
        QVERIFY(followedByOption.keptTokens.contains("-XX:+UseG1GC"));
        QVERIFY(followedByOption.keptTokens.contains("-Dfoo=bar"));

        const ServerJvmFilterResult trailingBare = ServerJvmArgs::filterTokens({ "-Xmx" }, 17);
        QVERIFY(trailingBare.ok);
        QVERIFY(trailingBare.keptTokens.isEmpty());
        QCOMPARE(trailingBare.removedMemoryOptions, QStringList({ "-Xmx" }));

        const ServerJvmFilterResult validPair = ServerJvmArgs::filterTokens(
            { "-Xms", "2048M", "-Dkeep=true" }, 17);
        QVERIFY(validPair.ok);
        QCOMPARE(validPair.removedMemoryOptions, QStringList({ "-Xms 2048M" }));
        QCOMPARE(validPair.keptTokens, QStringList({ "-Dkeep=true" }));
    }

    void preservesWindowsPathBackslashesExactly()
    {
        QString error;
        QStringList tokens;
        // Unquoted Windows separators stay literal (no escape consumption).
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "-Dpath=C:\\mods -Dother=X", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dpath=C:\\mods", "-Dother=X" }));

        // Java requires escaped backslashes inside quotes.
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "\"-Dpath=C:\\\\Program Files\\\\Server\"", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dpath=C:\\Program Files\\Server" }));

        // Doubled backslashes inside double quotes collapse to one (the only
        // way to represent them); outside quotes they stay literal.
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "\"-Dpath=C:\\\\mods\"", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dpath=C:\\mods" }));

        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "-Dpath=C:\\\\share", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dpath=C:\\\\share" }));

        // Embedded quotes via supported \" escape.
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "\"-Dmsg=a\\\"b\"", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dmsg=a\"b" }));

        // Backslash before ordinary characters (\m, \n in C:\new) is preserved.
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "-Dpath=C:\\new\\temp", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dpath=C:\\new\\temp" }));

        // Unquoted backslashes do not join lines in Java argument files.
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "-Dfoo=bar\\\nBaz -Dkeep=true", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dfoo=bar\\", "Baz", "-Dkeep=true" }));
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "-Dfoo=bar\\\r\nBaz", &tokens, &error),
                 qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dfoo=bar\\", "Baz" }));

        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     "\"-Dfoo=bar\\\r\n  \tBaz\"", &tokens, &error), qPrintable(error));
        QCOMPARE(tokens, QStringList({ "-Dfoo=barBaz" }));
    }

    void roundTripsWindowsPathsAndQuotesThroughEffectiveFile()
    {
        const QStringList original{
            QStringLiteral("-Dpath=C:\\mods"),
            QStringLiteral("-Dpath=C:\\Program Files\\Server"),
            QStringLiteral("-Dpath=C:\\\\share"),
            QStringLiteral("-Dmsg=a\"b"),
            QStringLiteral("-XX:+UseG1GC"),
        };
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString effective = temporaryRoot.filePath("effective.txt");
        QString error;
        QVERIFY2(ServerJvmArgs::writeEffectiveArgfile(effective, original, &error),
                 qPrintable(error));
        QStringList reparsed;
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(
                     QString::fromUtf8(readFile(effective)), &reparsed, &error),
                 qPrintable(error));
        QCOMPARE(reparsed, original);
    }

    void preservesCompatibleOptionsCommentsQuotesAndSpacing()
    {
        const QString content = QStringLiteral(
            "# pack header\n"
            "\n"
            "-XX:+UseG1GC\n"
            "   # inline comment\n"
            "-Dfile.encoding=UTF-8\n"
            "\"-Dquoted=value with spaces\"\n"
            "'-Dsingle=value with spaces'\n"
            "\"-Dhash=bar#baz\"\n");
        const ServerJvmFilterResult filtered = ServerJvmArgs::prepareContent(content, 17);
        QVERIFY2(filtered.ok, qPrintable(filtered.errorMessage));
        QVERIFY(filtered.keptTokens.contains("-XX:+UseG1GC"));
        QVERIFY(filtered.keptTokens.contains("-Dfile.encoding=UTF-8"));
        QVERIFY(filtered.keptTokens.contains("-Dquoted=value with spaces"));
        QVERIFY(filtered.keptTokens.contains("-Dsingle=value with spaces"));
        QVERIFY(filtered.keptTokens.contains("-Dhash=bar#baz"));
        QVERIFY(filtered.removedMemoryOptions.isEmpty());
        QVERIFY(filtered.removedUnsupportedOptions.isEmpty());

        // Quoted tokens with spaces must round-trip through the effective
        // file without being split or executed.
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString effective = temporaryRoot.filePath("effective.txt");
        QString error;
        QVERIFY2(ServerJvmArgs::writeEffectiveArgfile(effective, filtered.keptTokens, &error),
                 qPrintable(error));
        const QByteArray written = readFile(effective);
        QVERIFY(written.contains("\"-Dquoted=value with spaces\""));
        QStringList reparsed;
        QVERIFY2(ServerJvmArgs::tokenizeArgfile(QString::fromUtf8(written), &reparsed, &error),
                 qPrintable(error));
        // Header comment is skipped; quoted values survive intact.
        QVERIFY(reparsed.contains("-Dquoted=value with spaces"));
        QVERIFY(reparsed.contains("-Dsingle=value with spaces"));
    }

    void failsSafelyOnMalformedOrUnreadableArgfiles()
    {
        QString error;
        QStringList tokens;
        QVERIFY(!ServerJvmArgs::tokenizeArgfile("-XX:+UseG1GC \"unclosed\n", &tokens, &error));
        QVERIFY(error.contains("Unclosed quote"));

        const ServerJvmFilterResult unclosed =
            ServerJvmArgs::prepareContent("-Dfoo=\"unclosed\n", 17);
        QVERIFY(!unclosed.ok);
        QVERIFY(unclosed.errorMessage.contains("Unclosed quote"));

        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const ServerJvmFilterResult missing =
            ServerJvmArgs::prepareFile(temporaryRoot.filePath("missing/user_jvm_args.txt"), 17);
        QVERIFY(!missing.ok);
        QVERIFY(missing.errorMessage.contains("Could not read"));

        // NUL (including NUL.txt) is a reserved Windows device name.
        const QString nulPath = temporaryRoot.filePath("embedded-nul-args.txt");
        QVERIFY(writeFile(nulPath, QByteArray("ok\0bad", 6)));
        const ServerJvmFilterResult nul = ServerJvmArgs::prepareFile(nulPath, 17);
        QVERIFY(!nul.ok);
        QVERIFY(nul.errorMessage.contains("NUL"));
    }

    void preservesArgumentsInRealJava()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString java = qEnvironmentVariable(
            "JLAUNCHER_TEST_JAVA", QStringLiteral(JLAUNCHER_TEST_JAVA_PATH));
        QVERIFY2(QFileInfo(java).isFile(), qPrintable(java));
        const QString probe = root.filePath("ArgumentProbe.java");
        QVERIFY(writeFile(probe, R"(import java.nio.charset.StandardCharsets;
import java.util.Base64;
class ArgumentProbe {
    public static void main(String[] keys) {
        for (String key : keys) {
            String value = String.valueOf(System.getProperty(key));
            System.out.println(Base64.getEncoder().encodeToString(value.getBytes(StandardCharsets.UTF_8)));
        }
    }
}
)"));
        QString diagnostics;
        auto run = [&](const QStringList &options, const QStringList &keys,
                       const QString &environmentOptions, QByteArray *output) {
            QProcess process;
            auto environment = QProcessEnvironment::systemEnvironment();
            for (const QString &key : { QStringLiteral("JAVA_TOOL_OPTIONS"),
                                       QStringLiteral("_JAVA_OPTIONS"), QStringLiteral("JDK_JAVA_OPTIONS") }) {
                environment.remove(key);
            }
            if (!environmentOptions.isEmpty()) {
                environment.insert("JAVA_TOOL_OPTIONS", environmentOptions);
            }
            process.setProcessEnvironment(environment);
            process.start(java, options + QStringList{ probe } + keys);
            const bool finished = process.waitForFinished(30000);
            if (!finished) {
                process.kill();
                process.waitForFinished();
            }
            diagnostics = process.errorString() + '\n' + QString::fromUtf8(process.readAllStandardError());
            *output = process.readAllStandardOutput().replace("\r\n", "\n");
            return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        };
        auto encodedValues = [](const QStringList &values) {
            QByteArray output;
            for (const QString &value : values) {
                output += value.toUtf8().toBase64() + '\n';
            }
            return output;
        };

        const QString original = root.filePath("provider-args.txt");
        const QString effective = root.filePath("effective-args.txt");
        QByteArray provider =
            "-Dprobe.path=C:\\mods\n"
            "\"-Dprobe.spaces=C:\\\\Program Files\\\\Server\"\n"
            "'-Dprobe.single=C:\\\\mods'\n"
            "\"-Dprobe.control=a\\nb\\rc\\td\\fe\"\n"
            "\"-Dprobe.join=first\\\r\n  \tsecond\"\n"
            "-Dprobe.dropped=ignored#comment\n"
            "\"-Dprobe.hash=kept#hash\"\n"
            "# CR-only comment\r-Dprobe.after=present\n";
        provider += QStringLiteral("-Dprobe.native=a\u00a0b\n").toLocal8Bit();
        const QStringList keys{ "probe.path", "probe.spaces", "probe.single", "probe.control",
                                "probe.join", "probe.dropped", "probe.hash", "probe.after", "probe.native" };
        const QByteArray expected = encodedValues({ "C:\\mods", "C:\\Program Files\\Server", "C:\\mods",
                                                     "a\nb\rc\td\fe", "firstsecond", "null", "kept#hash", "present",
                                                     QStringLiteral("a\u00a0b") });
        QVERIFY(writeFile(original, provider));
        ServerJvmFilterResult prepared;
        QVERIFY2(ServerJvmArgs::writeEffectiveFileForSource(original, effective, 17, &prepared),
                 qPrintable(prepared.errorMessage));
        QByteArray actual;
        QVERIFY2(run({ "@" + original }, keys, {}, &actual), qPrintable(diagnostics));
        QCOMPARE(actual, expected);
        QVERIFY2(run({ "@" + effective }, keys, {}, &actual), qPrintable(diagnostics));
        QCOMPARE(actual, expected);
        QCOMPARE(readFile(original), provider);

        // Check the serializer independently of our parser, using values that
        // require different escaping in @-files and JAVA_TOOL_OPTIONS.
        const QStringList values{ "C:\\Program Files\\Server\\", "a\"b'c", "line\nrow\rcol\tend\f",
                                  "a#b", QStringLiteral("a\u00a0b") };
        QStringList valueKeys;
        QStringList tokens;
        QStringList environmentTokens;
        for (int i = 0; i < values.size(); ++i) {
            const QString key = QStringLiteral("probe.value%1").arg(i);
            valueKeys << key;
            tokens << ("-D" + key + '=' + values.at(i));
            environmentTokens << ServerJvmArgs::quoteEnvToken(tokens.constLast());
        }
        QString error;
        QVERIFY2(ServerJvmArgs::writeEffectiveArgfile(effective, tokens, &error), qPrintable(error));
        QVERIFY2(run({ "@" + effective }, valueKeys, {}, &actual), qPrintable(diagnostics));
        QCOMPARE(actual, encodedValues(values));
        QVERIFY2(run({}, valueKeys, environmentTokens.join(' '), &actual), qPrintable(diagnostics));
        QCOMPARE(actual, encodedValues(values));

        const QString wrapper = ServerJvmArgs::wrapperEnvironmentArgs(
            32, 64, QStringLiteral("\"-Dprobe.path=C:\\Program Files\\Server\" -Dprobe.quote=a\"\"\"b'c"));
        QVERIFY2(run({}, { "probe.path", "probe.quote", "user.language", "user.country.format" },
                     wrapper, &actual), qPrintable(diagnostics));
        QCOMPARE(actual, encodedValues({ "C:\\Program Files\\Server", "a\"b'c", "en", "US" }));
    }

    void generatesEffectiveFileWithoutMutatingOriginal()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QByteArray original(
            "# Requiem 1.20.1 supplied options\n-Xms9G\n-XX:+ZGenerational\n"
            "-XX:+UseG1GC\n-Dfile.encoding=UTF-8\n");
        const QString source = temporaryRoot.filePath("user_jvm_args.txt");
        const QString dest = temporaryRoot.filePath("jlauncher_effective_jvm_args.txt");
        QVERIFY(writeFile(source, original));

        ServerJvmFilterResult prepared;
        QVERIFY2(ServerJvmArgs::writeEffectiveFileForSource(source, dest, 17, &prepared),
                 qPrintable(prepared.errorMessage));
        QVERIFY(prepared.ok);
        QCOMPARE(readFile(source), original);
        QVERIFY(prepared.keptTokens.contains("-XX:+UseG1GC"));
        QVERIFY(!prepared.keptTokens.contains("-Xms9G"));
        QVERIFY(!prepared.keptTokens.contains("-XX:+ZGenerational"));
        const QByteArray effective = readFile(dest);
        QVERIFY(!effective.isEmpty());
        QVERIFY(effective.contains("-XX:+UseG1GC"));
        QVERIFY(!effective.contains("-Xms9G"));
        QVERIFY(!effective.contains("ZGenerational"));
        QVERIFY(effective.contains("Generated by J Launcher"));
    }

    void pinsStableLocaleOnDirectJarLaunch()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("direct-locale", "Direct locale");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server")));
        server.setMinMemory(1024);
        server.setMaxMemory(2048);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            server.consoleLog().contains("ARG:-Duser.language=en"), 5000);
        QVERIFY(server.consoleLog().contains("ARG:-Duser.country=US"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.language.format=en"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.country.format=US"));
        QVERIFY(server.consoleLog().contains("ARG:-Xmx2048M"));
        QVERIFY(server.consoleLog().contains("ARG:-Xms1024M"));
        QVERIFY(server.consoleLog().contains("[JVM] Using dedicated-server locale en_US"));

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void filtersRequiemStylePackOnJava17Launch()
    {
        ScopedEnvironmentVariable fakeJava("JLAUNCHER_FAKE_JAVA_MAJOR", "17");
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QString serverDirectory = temporaryRoot.filePath("server");
#ifdef Q_OS_WIN
        const QString scriptName = QStringLiteral("run.bat");
        const QString loaderRelative = QStringLiteral(
            "libraries/net/minecraftforge/forge/1.20.1-47.4.20/win_args.txt");
        const QByteArray launchScript =
            "@echo off\r\njava @user_jvm_args.txt "
            "@libraries/net/minecraftforge/forge/1.20.1-47.4.20/win_args.txt %*\r\n";
#else
        const QString scriptName = QStringLiteral("run.sh");
        const QString loaderRelative = QStringLiteral(
            "libraries/net/minecraftforge/forge/1.20.1-47.4.20/unix_args.txt");
        const QByteArray launchScript =
            "#!/bin/sh\njava @user_jvm_args.txt "
            "@libraries/net/minecraftforge/forge/1.20.1-47.4.20/unix_args.txt \"$@\"\n";
#endif
        const QByteArray supplied =
            "# Requiem 1.20.1 / Forge 47.4.20 supplied options\n"
            "-Xms9G\n"
            "-XX:+ZGenerational\n"
            "-XX:+UseG1GC\n"
            "-Dfile.encoding=UTF-8\n";

        ServerInstance server("requiem-style", "Requiem style");
        server.setServerDirectory(serverDirectory);
        server.setVersion("1.20.1");
        server.setLoaderType("forge");
        server.setLoaderVersion("47.4.20");
        server.setPort(unusedPort());
        server.setEulaAccepted(true);
        server.setJavaPath(fakeMinecraftServerPath());
        server.setMinMemory(2048);
        server.setMaxMemory(4096);

        QVERIFY(writeFile(QDir(serverDirectory).filePath(scriptName), launchScript));
        QVERIFY(writeFile(QDir(serverDirectory).filePath("user_jvm_args.txt"), supplied));
        QVERIFY(writeFile(QDir(serverDirectory).filePath(loaderRelative),
                          "# synthetic loader arguments\n"));
        QVERIFY(writeArchive(QDir(serverDirectory).filePath("server.jar"), {
            { "META-INF/MANIFEST.MF", "Main-Class: net.minecraft.bundler.Main\n" },
        }));

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            server.consoleLog().contains("ARG:@" + ServerJvmArgs::effectiveFileName()), 5000);
        QVERIFY(!server.consoleLog().contains("ARG:@user_jvm_args.txt"));
        // Launcher memory stays authoritative after pack -Xms removal.
        QVERIFY(server.consoleLog().contains("ARG:-Xmx4096M"));
        QVERIFY(server.consoleLog().contains("ARG:-Xms2048M"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.language=en"));
        QVERIFY(server.consoleLog().contains("ARG:-Duser.country.format=US"));
        QVERIFY(server.consoleLog().contains("[JVM] Removed pack memory option '-Xms9G'"));
        QVERIFY(server.consoleLog().contains("[JVM] Removed option '-XX:+ZGenerational'"));
        QCOMPARE(readFile(QDir(serverDirectory).filePath("user_jvm_args.txt")), supplied);
        const QByteArray effective =
            readFile(QDir(serverDirectory).filePath(ServerJvmArgs::effectiveFileName()));
        QVERIFY(effective.contains("-XX:+UseG1GC"));
        QVERIFY(effective.contains("-Dfile.encoding=UTF-8"));
        QVERIFY(!effective.contains("-Xms9G"));
        QVERIFY(!effective.contains("ZGenerational"));

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void classifiesUnrecognizedVmOptionsAsJavaRuntimeCause()
    {
        QCOMPARE(ServerDiagnostics::classifyCrash("Error: Unrecognized VM option 'ZGenerational'"),
                 ServerCrashCause::JavaVersion);
        QCOMPARE(ServerDiagnostics::crashCauseExplanation(ServerCrashCause::JavaVersion),
                 QString("The selected Java version, or a Java option, does not match what this server needs. It may be too old or too new."));
    }

    void reportsProcessExitBeforeReadiness()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("startup-failure", "Startup failure");
        QVERIFY(prepareSyntheticServer(
            server, temporaryRoot.filePath("server"), "-Dfake.exit-before-ready"));
        QSignalSpy errors(&server, &ServerInstance::serverError);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Error, 5000);
        QVERIFY(!errors.isEmpty());
        QVERIFY(errors.last().first().toString().contains("before reporting"));
    }

    void emitsControlledCrashDetailsAfterReadiness()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("controlled-crash", "Controlled crash");
        QVERIFY(prepareSyntheticServer(
            server, temporaryRoot.filePath("server"), "-Dfake.crash-memory"));
        QSignalSpy crashes(&server, &ServerInstance::serverCrashed);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Error, 5000);
        QCOMPARE(crashes.size(), 1);
        QVERIFY(crashes.first().at(0).toString().contains("exit code 1"));
        QVERIFY(crashes.first().at(1).toString().contains("OutOfMemoryError"));
    }

    void cachesJavaProbesUntilBinaryMetadataChanges()
    {
        ScopedEnvironmentVariable fakeJava("JLAUNCHER_FAKE_JAVA_MAJOR", "21");
        ServerInstance::clearJavaProbeCacheForTesting();
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("java-probe-cache", "Java probe cache");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server")));
        const QString javaPath = temporaryRoot.filePath(
            QFileInfo(fakeMinecraftServerPath()).fileName());
        QVERIFY(QFile::copy(fakeMinecraftServerPath(), javaPath));
        server.setJavaPath(javaPath);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
        const int firstProbeCount = ServerInstance::javaProbeCountForTesting();
        QVERIFY(firstProbeCount > 0);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
        QCOMPARE(ServerInstance::javaProbeCountForTesting(), firstProbeCount);

        QFile javaBinary(javaPath);
        QVERIFY(javaBinary.open(QIODevice::ReadWrite));
        QVERIFY(javaBinary.setFileTime(QDateTime::currentDateTime().addSecs(120),
                                       QFileDevice::FileModificationTime));
        javaBinary.close();
        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
        QVERIFY(ServerInstance::javaProbeCountForTesting() > firstProbeCount);
        ServerInstance::clearJavaProbeCacheForTesting();
    }

    void reportsReadinessTimeoutAndStopsProcess()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("startup-timeout", "Startup timeout");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server"), "-Dfake.no-ready"));
        server.setStartupTimeoutSeconds(1);
        QSignalSpy errors(&server, &ServerInstance::serverError);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Error, 3000);
        QVERIFY(!errors.isEmpty());
        QVERIFY(errors.last().first().toString().contains("did not report readiness"));
        QTRY_COMPARE_WITH_TIMEOUT(server.processId(), qint64(0), 5000);
    }

    void restartDoesNotKillTheNewProcess()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("restart-generation", "Restart generation");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server")));
        // The shortest grace period puts the stale kill() 10 s after the stop,
        // inside the wait below.
        server.setGracefulStopTimeoutSeconds(5);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        QVERIFY(server.restart());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        const qint64 restartedProcessId = server.processId();
        QVERIFY(restartedProcessId > 0);
        QTest::qWait(11000);
        QCOMPARE(server.status(), ServerStatus::Running);
        QCOMPARE(server.processId(), restartedProcessId);

        QVERIFY(server.stop());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopped, 5000);
    }

    void startupTimeoutKillsAServerThatIgnoresStop()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("startup-timeout-ignore-stop", "Startup timeout ignore stop");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server"),
                                       "-Dfake.no-ready -Dfake.ignore-stop"));
        server.setStartupTimeoutSeconds(1);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Stopping, 5000);
        QVERIFY(server.processId() > 0);
        QVERIFY(!server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Error, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(server.processId(), qint64(0), 5000);
    }

    void shutdownForExitSendsStopAndWaits()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("shutdown-for-exit", "Shutdown for exit");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server")));

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        server.requestShutdownForExit();
        QVERIFY(server.waitForShutdown(10000));
        QCOMPARE(server.processId(), qint64(0));
        QVERIFY(server.consoleLog().contains("[Server thread/INFO]: Stopping server"));
        QVERIFY(server.consoleLog().contains("Stopping server because J Launcher is closing."));
    }

    void shutdownForExitForcesAProcessThatIgnoresStop()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("shutdown-for-exit-ignore-stop", "Shutdown ignore stop");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server"),
                                       "-Dfake.ignore-stop"));

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(server.status(), ServerStatus::Running, 5000);
        server.requestShutdownForExit();
        QVERIFY(server.waitForShutdown(1000));
        QCOMPARE(server.processId(), qint64(0));
    }

    void cancelsPendingCrashRestart()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("cancel-crash-restart", "Cancel crash restart");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server"),
                                       "-Dfake.crash-memory"));
        server.setAutoRestartOnCrash(true);
        QSignalSpy started(&server, &ServerInstance::started);

        QVERIFY(server.start());
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingCrashRestart(), 5000);
        QCOMPARE(started.size(), 1);
        QVERIFY(server.cancelPendingCrashRestart());
        QVERIFY(!server.hasPendingCrashRestart());
        QTest::qWait(6000);
        QCOMPARE(server.status(), ServerStatus::Error);
        QCOMPARE(started.size(), 1);
    }

    void pausesCrashRestartsAfterThreeAutomaticRestarts()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("crash-restart-limit", "Crash restart limit");
        QVERIFY(prepareSyntheticServer(server, temporaryRoot.filePath("server"),
                                       "-Dfake.crash-memory"));
        server.setAutoRestartOnCrash(true);
        server.setCrashRestartDelayMs(100);
        QSignalSpy crashes(&server, &ServerInstance::serverCrashed);
        QSignalSpy errors(&server, &ServerInstance::serverError);

        QVERIFY(server.start());
        QTRY_COMPARE_WITH_TIMEOUT(crashes.size(), 4, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!server.hasPendingCrashRestart(), 5000);
        QCOMPARE(server.status(), ServerStatus::Error);
        bool foundRestartLimitMessage = false;
        for (const QList<QVariant> &arguments : errors) {
            if (arguments.first().toString().contains("Automatic restart paused")) {
                foundRestartLimitMessage = true;
                break;
            }
        }
        QVERIFY(foundRestartLimitMessage);
    }

    void reportsIncompatibleJavaRequirement()
    {
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        ServerInstance server("java-requirement", "Java requirement");
        server.setServerDirectory(temporaryRoot.filePath("server"));
        server.setVersion("1.21.8");
        server.setLoaderType("vanilla");
        server.setPort(unusedPort());
        server.setEulaAccepted(true);
        server.setJavaPath(fakeMinecraftServerPath());

        const QByteArray classHeader = QByteArray::fromHex("cafebabe0000008f");
        QVERIFY(QDir().mkpath(server.serverDirectory()));
        QVERIFY(writeArchive(server.serverJarPath(), {
            { "META-INF/MANIFEST.MF", "Main-Class: net.minecraft.bundler.Main\n" },
            { "net/minecraft/bundler/Main.class", classHeader },
        }));
        QSignalSpy errors(&server, &ServerInstance::serverError);

        QVERIFY(!server.start());
        QCOMPARE(server.status(), ServerStatus::Error);
        QVERIFY(!errors.isEmpty());
        QVERIFY(errors.last().first().toString().contains("Java 99"));
    }

    void requiresTheCompatibleJavaMajorInsteadOfAnyNewerRuntime()
    {
        QVERIFY(ServerInstance::isJavaMajorCompatible(8, 8));
        QVERIFY(ServerInstance::isJavaMajorCompatible(17, 17));
        QVERIFY(!ServerInstance::isJavaMajorCompatible(8, 25));
        QVERIFY(!ServerInstance::isJavaMajorCompatible(17, 21));
        QVERIFY(ServerInstance::isJavaMajorCompatible(0, 25));
    }

    void acceptsLegacyAndModularManagedJavaLayouts()
    {
#ifndef Q_OS_WIN
        QSKIP("Managed Java layout validation is Windows-specific.");
#endif
        QTemporaryDir temporaryRoot;
        QVERIFY(temporaryRoot.isValid());
        const QDir root(temporaryRoot.path());

        const QString legacyJava = root.filePath("legacy/bin/java.exe");
        QVERIFY(writeFile(legacyJava, "java"));
        QVERIFY(writeFile(root.filePath("legacy/lib/amd64/jvm.cfg"), "-server KNOWN"));
        QVERIFY(!Java::JavaRuntimeInstallTask::isUsableJava(legacyJava));
        QVERIFY(!JavaUtils::isJavaPathSafeToProbe(legacyJava, root.path()));
        QVERIFY(writeFile(root.filePath("legacy/lib/rt.jar"), "runtime"));
        QVERIFY(writeFile(root.filePath("legacy/bin/server/jvm.dll"), "vm"));
        QVERIFY(Java::JavaRuntimeInstallTask::isUsableJava(legacyJava));
        QVERIFY(JavaUtils::isJavaPathSafeToProbe(legacyJava, root.path()));

        const QString modularJava = root.filePath("modular/bin/java.exe");
        QVERIFY(writeFile(modularJava, "java"));
        QVERIFY(writeFile(root.filePath("modular/lib/jvm.cfg"), "-server KNOWN"));
        QVERIFY(writeFile(root.filePath("modular/lib/modules"), "runtime"));
        QVERIFY(writeFile(root.filePath("modular/bin/server/jvm.dll"), "vm"));
        QVERIFY(Java::JavaRuntimeInstallTask::isUsableJava(modularJava));
        QVERIFY(JavaUtils::isJavaPathSafeToProbe(modularJava, root.path()));
        QVERIFY(JavaUtils::isJavaPathSafeToProbe(QStringLiteral("javaw"), root.path()));
        QVERIFY(!JavaUtils::isJavaPathSafeToProbe(root.filePath("missing/bin/javaw.exe"), root.path()));
    }
};

QTEST_GUILESS_MAIN(ServerLaunchTest)

#include "ServerLaunch_test.moc"
