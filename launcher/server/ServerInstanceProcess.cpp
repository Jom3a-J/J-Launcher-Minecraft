/* Copyright 2013-2024 MultiMC Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Running the server process: start, stop, restart, crash restarts, console output and live player commands.

#include "ServerInstance.h"
#include "java/JavaUtils.h"
#include "ServerJvmArgs.h"
#include "ServerModpackInstaller.h"
#include "ServerProperties.h"
#include "Application.h"
#include "settings/SettingsObject.h"
#include "logs/Privacy.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QProcessEnvironment>

bool ServerInstance::start()
{
    if (!m_crashRestartStarting) {
        cancelPendingCrashRestart();
    }
    if (isActive()) {
        return false;
    }
    if (m_serverPackImportInProgress) {
        appendLog(tr("[INFO] The server will not start while a server pack is being imported."));
        return false;
    }

    if (m_serverDirectory.trimmed().isEmpty()) {
        const QString message = tr("A server folder must be configured before starting this server.");
        appendLog("[ERROR] " + message);
        setStatus(ServerStatus::Error);
        emit serverError(message);
        return false;
    }

    // Ensure server directory exists
    QFileInfo serverRoot(m_serverDirectory);
    if ((serverRoot.exists() || serverRoot.isSymLink())
        && (!serverRoot.isDir() || serverRoot.isSymLink())) {
        const QString message = tr("The configured server folder is not a usable directory.");
        appendLog("[ERROR] " + message);
        setStatus(ServerStatus::Error);
        emit serverError(message);
        return false;
    }
    QDir dir(m_serverDirectory);
    if (!dir.exists() && !dir.mkpath(".")) {
        const QString message = tr("The configured server folder could not be created.");
        appendLog("[ERROR] " + message);
        setStatus(ServerStatus::Error);
        emit serverError(message);
        return false;
    }

    const QString setupIssue = ServerProperties::worldSetupIssue(m_serverDirectory);
    if (!setupIssue.isEmpty()) {
        appendLog("[ERROR] " + setupIssue);
        setStatus(ServerStatus::Error);
        emit serverError(setupIssue);
        return false;
    }

    if (QFileInfo(QDir(m_serverDirectory).filePath(
                      QStringLiteral("jlauncher_derived_server.txt"))).isFile()) {
        const ServerDependencyCheckResult dependencyCheck =
            ServerModpackInstaller::checkServerDependencies(
                m_serverDirectory, m_loaderType, m_version, m_loaderVersion);
        if (dependencyCheck.state == ServerDependencyCheckState::DefiniteFailure
            || dependencyCheck.state == ServerDependencyCheckState::Unsafe) {
            const QString message = tr(
                "Setup required: server dependencies are missing or incompatible. %1")
                                        .arg(dependencyCheck.error);
            appendLog("[ERROR] " + message);
            setStatus(ServerStatus::Error);
            emit serverError(message);
            return false;
        }
        if (dependencyCheck.state == ServerDependencyCheckState::Inconclusive) {
            appendLog(tr("[WARN] Dependency inspection was inconclusive; the loader will perform the final check: %1")
                          .arg(dependencyCheck.error));
        }
    }

    if (!m_eulaAccepted) {
        const QString message = tr("Accept the Minecraft EULA before starting this server.");
        appendLog("[ERROR] " + message);
        setStatus(ServerStatus::Error);
        emit serverError(message);
        return false;
    }

    if (!isPortAvailable(static_cast<quint16>(m_port))) {
        const QString message =
            tr("Port %1 is already in use. Stop the other server or choose a different port in Server Settings.")
                .arg(m_port);
        appendLog("[ERROR] " + message);
        setStatus(ServerStatus::Error);
        emit serverError(message);
        return false;
    }

    const int requiredJava = qMax(
        requiredJavaVersion(), recommendedJavaMajor(m_version, m_loaderType));
    int detectedJava = 0;
    const QString javaPath = compatibleJavaPath(requiredJava, &detectedJava);
    if (javaPath.isEmpty()) {
        if (requiredJava > 0) {
            if (auto *application = APPLICATION_DYN;
                application && application->settings()->get("AutomaticJavaDownload").toBool()) {
                return installCompatibleJava(requiredJava);
            }
        }
        const QString message = requiredJava > 0
            ? tr("This server requires Java %1, but no compatible Java installation was found. "
                 "Enable automatic Java downloads or set its path in Server Settings.").arg(requiredJava)
            : tr("No usable Java installation was found. Set a Java path in Server Settings.");
        appendLog("[ERROR] " + message);
        setStatus(ServerStatus::Error);
        emit serverError(message);
        return false;
    }
    appendLog(tr("[INFO] Using Java %1: %2").arg(detectedJava).arg(javaPath));

    const QString loader = m_loaderType.trimmed().toLower();
    if (!m_startupTimeoutOverridden) {
        const bool modded = loader == QStringLiteral("fabric")
            || loader == QStringLiteral("forge")
            || loader == QStringLiteral("neoforge");
        m_startupTimeoutTimer.setInterval((modded ? 300 : 120) * 1000);
    }

    // Auto-download the server only when an installed launch target is absent.
    // Modern Forge and NeoForge installations use a platform launch script
    // instead of a single JAR.
    if (!hasLaunchTarget()) {
        return downloadServerJar(javaPath);
    }

    // Create server.properties if not exists
    if (!QFile::exists(serverPropertiesPath()) && !createServerProperties()) {
        const QString message = tr("The launcher could not create server.properties.");
        appendLog("[ERROR] " + message);
        setStatus(ServerStatus::Error);
        emit serverError(message);
        return false;
    }

    if (!acceptEULA()) {
        const QString message = tr("The launcher could not write the accepted EULA state to eula.txt.");
        appendLog("[ERROR] " + message);
        setStatus(ServerStatus::Error);
        emit serverError(message);
        return false;
    }

    // Start server process. Forge and NeoForge 1.17+ generate a run script
    // with the required module and argument-file setup, so launching a JAR
    // directly is not reliable for those server types.
    m_standardOutputBuffer.clear();
    m_standardErrorBuffer.clear();
    m_process.reset(new QProcess());
    ++m_processGeneration;
    connect(m_process.get(), &QProcess::started, this, &ServerInstance::onProcessStarted);
    connect(m_process.get(), &QProcess::readyReadStandardOutput, this, &ServerInstance::onProcessReadyReadStandardOutput);
    connect(m_process.get(), &QProcess::readyReadStandardError, this, &ServerInstance::onProcessReadyReadStandardError);
    connect(m_process.get(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ServerInstance::onProcessFinished);
    connect(m_process.get(), &QProcess::errorOccurred, this, &ServerInstance::onProcessError);

    m_process->setWorkingDirectory(m_serverDirectory);
    QProcessEnvironment serverEnvironment = CleanEnviroment();
    const QFileInfo javaInfo(javaPath);
    if (javaInfo.isAbsolute()) {
        const QString javaBin = javaInfo.absoluteDir().absolutePath();
        const QString javaHome = QDir::cleanPath(QDir(javaBin).absoluteFilePath(".."));
        serverEnvironment.insert("JAVA_HOME", javaHome);
        serverEnvironment.insert("PATH", javaBin + QDir::listSeparator() +
                                 serverEnvironment.value("PATH"));
    }
    m_process->setProcessEnvironment(serverEnvironment);
    m_startupTimedOut = false;
    setStatus(ServerStatus::Starting);

    // Launcher-owned JVM settings. Memory stays authoritative and the locale
    // is pinned to en_US (including the FORMAT category) so pack-supplied
    // options cannot reintroduce host-locale digits into resource paths.
    // Client launches are untouched; only dedicated-server paths use this.
    const QStringList serverLocaleArgs = ServerJvmArgs::serverLocaleJvmArgs();
    QStringList jvmArgs;
    jvmArgs << QString("-Xmx%1M").arg(m_maxMemory);
    jvmArgs << QString("-Xms%1M").arg(m_minMemory);
    if (!m_extraJvmArguments.isEmpty()) {
        jvmArgs << QProcess::splitCommand(m_extraJvmArguments);
    }
    jvmArgs << serverLocaleArgs;
    const QString localeLogLine =
        tr("[JVM] Using dedicated-server locale en_US (language=en country=US) on Java %1.")
            .arg(detectedJava);

    if ((loader == QStringLiteral("forge") || loader == QStringLiteral("neoforge")) &&
        QFileInfo(loaderScriptPath()).isFile()) {
        const QString loaderArgsPath = loaderArgumentsFile();
        if (!loaderArgsPath.isEmpty()) {
            QStringList args;
            const QDir serverDir(m_serverDirectory);
            const QString suppliedArgsPath = serverDir.filePath(QStringLiteral("user_jvm_args.txt"));
            if (QFile::exists(suppliedArgsPath)) {
                // Preserve the original pack file byte-for-byte and launch
                // with a separate launcher-owned effective argfile. Never
                // pass supplied text through a shell; QProcess argv plus
                // Java @-files carry the tokens verbatim.
                const QString effectivePath =
                    serverDir.filePath(ServerJvmArgs::effectiveFileName());
                ServerJvmFilterResult prepared;
                if (!ServerJvmArgs::writeEffectiveFileForSource(suppliedArgsPath, effectivePath,
                                                                 detectedJava, &prepared)) {
                    const QString message =
                        tr("The server's supplied JVM arguments could not be prepared safely: %1 "
                           "Check user_jvm_args.txt for unreadable content, oversized files, or unclosed quotes.")
                            .arg(prepared.errorMessage);
                    appendLog("[ERROR] " + message);
                    setStatus(ServerStatus::Error);
                    emit serverError(message);
                    return false;
                }
                for (const QString &removed : prepared.removedMemoryOptions) {
                    const QString warning =
                        tr("[JVM] Removed pack memory option '%1'; using the launcher memory settings "
                           "(%2 MiB min / %3 MiB max).")
                            .arg(removed)
                            .arg(m_minMemory)
                            .arg(m_maxMemory);
                    appendLog(warning);
                    emit outputReceived(warning);
                }
                for (const QString &removed : prepared.removedUnsupportedOptions) {
                    const QString warning =
                        tr("[JVM] Removed option '%1' which is not supported by Java %2.")
                            .arg(removed)
                            .arg(detectedJava);
                    appendLog(warning);
                    emit outputReceived(warning);
                }
                appendLog(localeLogLine);
                emit outputReceived(localeLogLine);
                if (!prepared.keptTokens.isEmpty()) {
                    appendLog(tr("[JVM] Kept %1 pack option(s) in %2; original user_jvm_args.txt left unchanged.")
                                  .arg(prepared.keptTokens.size())
                                  .arg(ServerJvmArgs::effectiveFileName()));
                } else {
                    appendLog(tr("[JVM] No pack JVM options remained after filtering; using launcher defaults. "
                                 "Original user_jvm_args.txt left unchanged."));
                }
                args << (QStringLiteral("@") + ServerJvmArgs::effectiveFileName());
            } else {
                appendLog(localeLogLine);
                emit outputReceived(localeLogLine);
            }
            args << jvmArgs;
            args << (QStringLiteral("@") +
                     QDir::fromNativeSeparators(
                         QDir(m_serverDirectory).relativeFilePath(loaderArgsPath)));
            args << QStringLiteral("nogui");
            appendLog(tr("[INFO] Launching %1 with the configured Java and memory settings.")
                          .arg(loader == QStringLiteral("forge") ? QStringLiteral("Forge")
                                                                    : QStringLiteral("NeoForge")));
            m_process->start(javaPath, args);
        } else {
            // Some published server packs replace the generated Forge script
            // with a custom wrapper. The wrapper's hardcoded argfile cannot
            // be parsed or rewritten safely, so preserve the wrapper and use
            // only generic JVM environment protection. Pack memory flags
            // inside the wrapper still override JAVA_TOOL_OPTIONS because
            // JVM command-line options win over the environment.
            const QString environmentJvmArgs =
                ServerJvmArgs::wrapperEnvironmentArgs(m_minMemory, m_maxMemory, m_extraJvmArguments);
            serverEnvironment.insert(QStringLiteral("JAVA_TOOL_OPTIONS"), environmentJvmArgs);
            m_process->setProcessEnvironment(serverEnvironment);
            appendLog(tr("[WARN] This server uses a custom loader script; applying memory, locale, and "
                         "IgnoreUnrecognizedVMOptions through the process environment."));
            appendLog(localeLogLine);
            emit outputReceived(localeLogLine);
            appendLog(tr("[JVM] JAVA_TOOL_OPTIONS carries launcher memory/locale plus "
                         "-XX:+IgnoreUnrecognizedVMOptions so unknown pack options do not abort startup. "
                         "Pack -Xms/-Xmx inside the opaque wrapper still take precedence and cannot be filtered safely."));
            emit outputReceived(tr("[JVM] Opaque wrapper limit: launcher memory is a fallback; the wrapper's own memory flags win."));
#ifdef Q_OS_WIN
            m_process->start("cmd.exe",
                             QStringList() << "/d" << "/c"
                                           << (QStringLiteral(".\\")
                                               + QFileInfo(loaderScriptPath()).fileName())
                                           << "nogui");
#else
            m_process->start(QStringLiteral("/bin/sh"),
                             QStringList() << loaderScriptPath() << QStringLiteral("nogui"));
#endif
        }
    } else {
        QStringList args = jvmArgs;
        args << "-jar" << serverJarPath() << "nogui";
        appendLog(localeLogLine);
        m_process->start(javaPath, args);
    }

    return true;
}

bool ServerInstance::stop()
{
    if (m_status != ServerStatus::Running && m_status != ServerStatus::Starting) {
        return false;
    }

    setStatus(ServerStatus::Stopping);

    // Request a clean shutdown without blocking the launcher UI.
    if (m_process && m_process->state() != QProcess::NotRunning) {
        writeStdin(QStringLiteral("stop"));
        scheduleStopEscalation(m_gracefulStopTimeoutSeconds * 1000);
    }

    return true;
}

void ServerInstance::requestShutdownForExit()
{
    cancelPendingCrashRestart();
    if (m_status == ServerStatus::Downloading) {
        cancelDownload();
    }
    if (m_process && m_process->state() != QProcess::NotRunning
        && m_status != ServerStatus::Stopping) {
        setStatus(ServerStatus::Stopping);
        writeStdin(QStringLiteral("stop"));
        appendLog(tr("[INFO] Stopping server because J Launcher is closing."));
    }
}

bool ServerInstance::waitForShutdown(int timeoutMs)
{
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return true;
    }
    m_process->waitForFinished(qMax(0, timeoutMs));
    if (m_process->state() != QProcess::NotRunning) {
        forceKillProcessTree();
        m_process->waitForFinished(5000);
    }
    return m_process->state() == QProcess::NotRunning;
}

bool ServerInstance::cancelPendingCrashRestart()
{
    if (!m_crashRestartTimer.isActive()) {
        return false;
    }
    m_crashRestartTimer.stop();
    appendLog(tr("[INFO] Automatic restart cancelled."));
    emit crashRestartPendingChanged(false);
    return true;
}

void ServerInstance::scheduleStopEscalation(int graceMs)
{
    const quint64 generation = m_processGeneration;
    QTimer::singleShot(graceMs, this, [this, generation]() {
        if (generation != m_processGeneration || !m_process
            || m_process->state() == QProcess::NotRunning) {
            return;
        }
        m_process->terminate();
        QTimer::singleShot(5000, this, [this, generation]() {
            if (generation == m_processGeneration && m_process
                && m_process->state() != QProcess::NotRunning) {
                forceKillProcessTree();
            }
        });
    });
}

void ServerInstance::forceKillProcessTree()
{
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return;
    }
#ifdef Q_OS_WIN
    const QString systemRoot = qEnvironmentVariable("SystemRoot");
    QString taskkillPath;
    if (!systemRoot.isEmpty()) {
        taskkillPath = QDir(systemRoot).filePath(QStringLiteral("System32/taskkill.exe"));
        if (!QFileInfo::exists(taskkillPath)) {
            taskkillPath.clear();
        }
    }
    if (taskkillPath.isEmpty()) {
        taskkillPath = QStandardPaths::findExecutable(QStringLiteral("taskkill"));
    }
    const qint64 pid = m_process->processId();
    if (!taskkillPath.isEmpty() && pid > 0) {
        QProcess taskkill;
        taskkill.start(taskkillPath,
                       { QStringLiteral("/PID"), QString::number(pid),
                         QStringLiteral("/T"), QStringLiteral("/F") });
        taskkill.waitForFinished(5000);
    }
#endif
    m_process->kill();
}

bool ServerInstance::restart()
{
    if (m_status == ServerStatus::Stopped || m_status == ServerStatus::Error) {
        return start();
    }
    if (m_status == ServerStatus::Stopping || m_status == ServerStatus::Downloading) {
        return false;
    }

    m_restartRequested = true;
    if (!stop()) {
        m_restartRequested = false;
        return false;
    }
    return true;
}

void ServerInstance::onProcessStarted()
{
    m_startedAt = QDateTime::currentDateTime();
    appendLog(tr("[INFO] Java process started; waiting for the Minecraft server to become ready."));
    m_startupTimeoutTimer.start();
}

void ServerInstance::onProcessReadyReadStandardOutput()
{
    if (m_process) {
        processOutputBuffer(m_standardOutputBuffer, false);
    }
}

void ServerInstance::onProcessReadyReadStandardError()
{
    if (m_process) {
        processOutputBuffer(m_standardErrorBuffer, true);
    }
}

QStringList ServerInstance::takeCompleteLines(QByteArray &buffer)
{
    QStringList lines;
    qsizetype newline = buffer.indexOf('\n');
    while (newline >= 0) {
        QByteArray line = buffer.left(newline);
        buffer.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        lines.append(QString::fromLocal8Bit(line));
        newline = buffer.indexOf('\n');
    }
    if (buffer.size() > 1024 * 1024) {
        if (buffer.endsWith('\r')) {
            buffer.chop(1);
        }
        lines.append(QString::fromLocal8Bit(buffer));
        buffer.clear();
    }
    return lines;
}

void ServerInstance::processOutputBuffer(QByteArray &buffer, bool error)
{
    if (!m_process) {
        return;
    }
    buffer.append(error ? m_process->readAllStandardError()
                        : m_process->readAllStandardOutput());
    const QStringList lines = takeCompleteLines(buffer);
    for (const QString &line : lines) {
        if (!line.trimmed().isEmpty()) {
            handleConsoleLine(line.trimmed(), error);
        }
    }
}

void ServerInstance::handleConsoleLine(const QString &line, bool error)
{
    const QString safeLine = Privacy::sanitizeText(line, 8192);
    const QString formatted = error ? "[ERROR] " + safeLine : safeLine;
    // Console lines arrive constantly; the redaction regexes are costly, so run them once.
    appendSanitizedLog(formatted);
    if (error) emit errorReceived(safeLine); else emit outputReceived(safeLine);

    if (m_status == ServerStatus::Starting && isReadyOutput(line)) {
        m_startupTimeoutTimer.stop();
        setStatus(ServerStatus::Running);
    }

    QString player;
    bool joined = false;
    if (parsePlayerActivity(line, &player, &joined)) {
        emit playerActivity(player, joined);
    }
}

bool ServerInstance::parsePlayerActivity(const QString &line, QString *player, bool *joined)
{
    static const QRegularExpression activityPattern(
        QStringLiteral(R"(^(?:\[[^\]]*\]\s*)+:\s*([A-Za-z0-9_]{3,16}) (joined|left) the game\s*$)"));
    const QRegularExpressionMatch match = activityPattern.match(line);
    if (!match.hasMatch()) {
        return false;
    }
    if (player) {
        *player = match.captured(1);
    }
    if (joined) {
        *joined = match.captured(2) == QStringLiteral("joined");
    }
    return true;
}

bool ServerInstance::isReadyOutput(const QString &line)
{
    static const QRegularExpression donePattern(
        R"((?:^|\]\s*):?\s*Done\s*\([^)]+\)!\s*(?:For help|Type help))",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression helpPattern(
        R"(For help,\s*type\s*["']?help["']?)",
        QRegularExpression::CaseInsensitiveOption);
    return donePattern.match(line).hasMatch() || helpPattern.match(line).hasMatch();
}

bool ServerInstance::isPortAvailable(quint16 port)
{
    QTcpSocket connectionProbe;
    connectionProbe.connectToHost(QHostAddress::LocalHost, port);
    if (connectionProbe.waitForConnected(150)) {
        connectionProbe.abort();
        return false;
    }

    QTcpServer probe;
    if (!probe.listen(QHostAddress::AnyIPv4, port)) {
        return false;
    }
    probe.close();
    return true;
}

void ServerInstance::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    processOutputBuffer(m_standardOutputBuffer, false);
    processOutputBuffer(m_standardErrorBuffer, true);
    const auto flushTail = [this](QByteArray &buffer, bool error) {
        if (buffer.isEmpty()) {
            return;
        }
        QByteArray tail = buffer;
        buffer.clear();
        if (tail.endsWith('\r')) {
            tail.chop(1);
        }
        const QString line = QString::fromLocal8Bit(tail).trimmed();
        if (!line.isEmpty()) {
            handleConsoleLine(line, error);
        }
    };
    flushTail(m_standardOutputBuffer, false);
    flushTail(m_standardErrorBuffer, true);
    m_startupTimeoutTimer.stop();
    const bool restart = m_restartRequested;
    m_restartRequested = false;
    const bool expectedStop = m_status == ServerStatus::Stopping;
    const bool exitedBeforeReady = m_status == ServerStatus::Starting;
    const bool keepErrorState = m_status == ServerStatus::Error;
    const bool abnormalExit = exitStatus == QProcess::CrashExit || exitCode != 0;
    m_startedAt = QDateTime();
    setStatus(m_startupTimedOut ? ServerStatus::Error
             : expectedStop || restart ? ServerStatus::Stopped
                                     : (exitedBeforeReady || keepErrorState || abnormalExit
                                            ? ServerStatus::Error
                                            : ServerStatus::Stopped));

    if (exitedBeforeReady && !m_startupTimedOut) {
        const QString details = m_consoleLog.right(6000);
        const bool incompatibleJava =
            details.contains(QStringLiteral("UnsupportedClassVersionError"), Qt::CaseInsensitive)
            || details.contains(QStringLiteral("class file version"), Qt::CaseInsensitive);
        const bool unrecognizedJvmOption =
            details.contains(QStringLiteral("Unrecognized VM option"), Qt::CaseInsensitive)
            || details.contains(QStringLiteral("Unrecognized option"), Qt::CaseInsensitive);
        const QString message = incompatibleJava
            ? tr("The server exited because the selected Java runtime is incompatible with this Minecraft or loader version. "
                 "Choose the required Java version or enable automatic Java downloads.")
            : unrecognizedJvmOption
            ? tr("The server exited because a supplied JVM option is not supported by the selected Java runtime. "
                 "Check user_jvm_args.txt and the [JVM] launch log lines for the filtered option.")
            : tr("The server process exited before reporting that it was ready (exit code %1).").arg(exitCode);
        appendLog("[ERROR] " + message);
        emit serverError(message);
        emit serverCrashed(message, details);
    } else if (abnormalExit && !expectedStop && !m_startupTimedOut) {
        const QString message = QString("Server crashed with exit code %1").arg(exitCode);
        const QString details = m_consoleLog.right(6000);
        appendLog("[CRASH] " + message);
        emit serverError(message);
        emit serverCrashed(message, details);
        if (m_autoRestartOnCrash && !restart) {
            const QDateTime now = QDateTime::currentDateTime();
            const QDateTime cutoff = now.addSecs(-600);
            for (qsizetype index = m_crashRestartTimestamps.size(); index > 0; --index) {
                if (m_crashRestartTimestamps.at(index - 1) < cutoff) {
                    m_crashRestartTimestamps.removeAt(index - 1);
                }
            }
            if (m_crashRestartTimestamps.size() >= 3) {
                const QString restartLimitMessage = tr(
                    "Automatic restart paused after 3 crashes in 10 minutes. Fix the cause and start the server manually.");
                appendLog("[ERROR] " + restartLimitMessage);
                emit serverError(restartLimitMessage);
            } else {
                if (m_crashRestartDelayMs == 5000) {
                    appendLog("[INFO] Automatic crash restart scheduled in 5 seconds.");
                } else {
                    appendLog(tr("[INFO] Automatic crash restart scheduled in %1 ms.")
                                  .arg(m_crashRestartDelayMs));
                }
                m_crashRestartTimer.setInterval(m_crashRestartDelayMs);
                m_crashRestartTimer.start();
                emit crashRestartPendingChanged(true);
            }
        }
    }
    m_startupTimedOut = false;

    if (restart) {
        QTimer::singleShot(0, this, [this]() { start(); });
    }
}

void ServerInstance::onProcessError(QProcess::ProcessError error)
{
    m_startupTimeoutTimer.stop();

    // QProcess can report a process error while a server is already being
    // stopped (for example when the JVM closes its pipes before the wrapper
    // process finishes). That is part of the requested shutdown, not a server
    // crash, and must not briefly put the server into the Error state.
    if (m_status == ServerStatus::Stopping) {
        appendLog(tr("[INFO] Server process reported an expected shutdown-time process event."));
        return;
    }

    setStatus(ServerStatus::Error);

    QString errorMsg;
    switch (error) {
        case QProcess::FailedToStart:
            errorMsg = "Failed to start server. Is Java installed?";
            break;
        case QProcess::Crashed:
            errorMsg = "Server process crashed.";
            break;
        case QProcess::Timedout:
            errorMsg = "Server process timed out.";
            break;
        case QProcess::WriteError:
            errorMsg = "Error writing to server process.";
            break;
        case QProcess::ReadError:
            errorMsg = "Error reading from server process.";
            break;
        default:
            errorMsg = "Unknown error occurred.";
            break;
    }

    appendLog("[ERROR] " + errorMsg);
    emit serverError(errorMsg);
    if (error == QProcess::FailedToStart) {
        emit serverCrashed(errorMsg, m_consoleLog.right(6000));
    }
}

bool ServerInstance::kickPlayer(const QString &name, const QString &reason, QString *error)
{
    if (m_status != ServerStatus::Running || !m_process
        || m_process->state() != QProcess::Running) {
        if (error) {
            *error = tr("The server must be running before a player can be kicked.");
        }
        return false;
    }

    const QString safeName = name.trimmed();
    static const QRegularExpression namePattern(QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
    if (!namePattern.match(safeName).hasMatch()) {
        if (error) {
            *error = tr("The selected player name is invalid.");
        }
        return false;
    }

    QString safeReason = reason.simplified();
    if (safeReason.isEmpty()) {
        safeReason = tr("Removed by server operator");
    }
    if (safeReason.size() > 256 || safeReason.contains('\n') || safeReason.contains('\r')) {
        if (error) {
            *error = tr("The kick reason is invalid or too long.");
        }
        return false;
    }

    const QString command = QStringLiteral("kick %1 %2").arg(safeName, safeReason);
    writeStdin(command);
    appendLog(QStringLiteral("> %1").arg(command));
    return true;
}

bool ServerInstance::sendPlayerAdministrationCommand(const QString &verb, const QString &name,
                                                     const QString &reason, QString *error)
{
    if (m_status != ServerStatus::Running || !m_process
        || m_process->state() != QProcess::Running) {
        if (error) {
            *error = tr("The server must be running before sending a live player command.");
        }
        return false;
    }

    static const QRegularExpression namePattern(QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
    const QString safeName = name.trimmed();
    if (!namePattern.match(safeName).hasMatch()) {
        if (error) {
            *error = tr("The selected player name is invalid.");
        }
        return false;
    }

    static const QSet<QString> allowedVerbs = {
        QStringLiteral("whitelist add"), QStringLiteral("whitelist remove"),
        QStringLiteral("op"), QStringLiteral("deop"),
        QStringLiteral("ban"), QStringLiteral("pardon")
    };
    if (!allowedVerbs.contains(verb)) {
        if (error) {
            *error = tr("The requested player administration command is not supported.");
        }
        return false;
    }

    QString safeReason = reason.simplified();
    if (safeReason.size() > 256 || safeReason.contains('\n') || safeReason.contains('\r')) {
        if (error) {
            *error = tr("The player administration reason is invalid or too long.");
        }
        return false;
    }
    const QString command = safeReason.isEmpty()
        ? QStringLiteral("%1 %2").arg(verb, safeName)
        : QStringLiteral("%1 %2 %3").arg(verb, safeName, safeReason);
    writeStdin(command);
    appendLog(QStringLiteral("> %1").arg(command));
    emit outputReceived(QStringLiteral("> %1").arg(command));
    return true;
}

bool ServerInstance::setPlayerWhitelistedLive(const QString &name, bool enabled, QString *error)
{
    return sendPlayerAdministrationCommand(
        enabled ? QStringLiteral("whitelist add") : QStringLiteral("whitelist remove"),
        name, QString(), error);
}

bool ServerInstance::setPlayerOperatorLive(const QString &name, bool enabled, QString *error)
{
    return sendPlayerAdministrationCommand(
        enabled ? QStringLiteral("op") : QStringLiteral("deop"), name, QString(), error);
}

bool ServerInstance::setPlayerBannedLive(const QString &name, bool enabled,
                                         const QString &reason, QString *error)
{
    return sendPlayerAdministrationCommand(
        enabled ? QStringLiteral("ban") : QStringLiteral("pardon"), name,
        enabled ? reason : QString(), error);
}

bool ServerInstance::clearPlayerAccessLive(const QString &name, QString *error)
{
    if (!setPlayerWhitelistedLive(name, false, error)) return false;
    if (!setPlayerOperatorLive(name, false, error)) return false;
    return setPlayerBannedLive(name, false, QString(), error);
}

void ServerInstance::writeStdin(const QString &command)
{
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->write((command + "\n").toLocal8Bit());
    }
}
