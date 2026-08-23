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

#pragma once

#include <QObject>
#include <QString>
#include <QProcess>
#include <QTimer>
#include <QDateTime>
#include <QJsonObject>
#include <memory>

#include "tasks/Task.h"

class ServerDownloader;
struct ServerProviderEndpoints;

enum class ServerStatus {
    Stopped,
    Starting,
    Running,
    Stopping,
    Error,
    Downloading
};

enum class ServerContentType {
    None,
    Mod,
    Plugin
};

class ServerInstance : public QObject
{
    Q_OBJECT

public:
    explicit ServerInstance(const QString &id, const QString &name, QObject *parent = nullptr);
    ServerInstance(const QString &id, const QString &name,
                   const ServerProviderEndpoints &providerEndpoints,
                   QObject *parent = nullptr);
    ~ServerInstance();

    // Getters
    QString id() const { return m_id; }
    QString name() const { return m_name; }
    QString version() const { return m_version; }
    QString loaderType() const { return m_loaderType; }
    QString loaderVersion() const { return m_loaderVersion; }
    int port() const { return m_port; }
    int maxMemory() const { return m_maxMemory; }
    int minMemory() const { return m_minMemory; }
    QString javaPath() const { return m_javaPath; }
    QString extraJvmArguments() const { return m_extraJvmArguments; }
    bool autoRestartOnCrash() const { return m_autoRestartOnCrash; }
    bool eulaAccepted() const { return m_eulaAccepted; }
    int gracefulStopTimeoutSeconds() const { return m_gracefulStopTimeoutSeconds; }
    QString serverDirectory() const { return m_serverDirectory; }
    ServerStatus status() const { return m_status; }
    bool isOnline() const { return m_status == ServerStatus::Running; }
    QString consoleLog() const { return m_consoleLog; }
    qint64 processId() const { return m_process ? m_process->processId() : 0; }
    QDateTime startedAt() const { return m_startedAt; }

    // Setters
    void setName(const QString &name);
    void setVersion(const QString &version);
    void setLoaderType(const QString &type);
    void setLoaderVersion(const QString &version);
    void setPort(int port);
    void setMaxMemory(int memory);
    void setMinMemory(int memory);
    void setJavaPath(const QString &path);
    void setExtraJvmArguments(const QString &arguments);
    void setAutoRestartOnCrash(bool enabled);
    void setEulaAccepted(bool accepted);
    void setGracefulStopTimeoutSeconds(int seconds);
    void setStartupTimeoutSeconds(int seconds);
    void setServerDirectory(const QString &dir);

    // Server operations
    bool start();
    bool stop();
    bool restart();
    bool isRunning() const;
    bool downloadServerJar(const QString &javaPath = QString(), bool startAfterDownload = true);
    bool downloadServerJarForVersion(const QString &targetVersion,
                                     const QString &javaPath = QString(),
                                     bool startAfterDownload = false);
    bool downloadServerBuild(const QString &targetBuild,
                             const QString &javaPath = QString(),
                             bool startAfterDownload = false);
    bool cancelDownload();
    bool addMods(const QStringList &paths, QString *error = nullptr);
    bool importServerPack(const QString &archivePath, QString *error = nullptr);
    bool kickPlayer(const QString &name, const QString &reason = QString(),
                    QString *error = nullptr);
    bool setPlayerWhitelistedLive(const QString &name, bool enabled,
                                  QString *error = nullptr);
    bool setPlayerOperatorLive(const QString &name, bool enabled,
                               QString *error = nullptr);
    bool setPlayerBannedLive(const QString &name, bool enabled,
                             const QString &reason = QString(),
                             QString *error = nullptr);
    bool clearPlayerAccessLive(const QString &name, QString *error = nullptr);
    void writeStdin(const QString &command);
    void appendLog(const QString &line);
    static bool isReadyOutput(const QString &line);
    static bool isPortAvailable(quint16 port);
    static bool isJavaMajorCompatible(int requiredVersion, int detectedVersion);
    static int recommendedJavaMajor(const QString &minecraftVersion,
                                    const QString &loaderType);

    // Save/Load
    QJsonObject toJson() const;
    static std::shared_ptr<ServerInstance> fromJson(const QJsonObject &json, const QString &dataDir);

    // Get server JAR path
    QString serverJarPath() const;

    // Get server properties path
    QString serverPropertiesPath() const;

    // Get the directory where server-side mod JARs live.
    QString modsDirectory() const;

    // Get the directory where Paper and Purpur plugin JARs live.
    QString pluginsDirectory() const;
    QString contentDirectory() const;
    ServerContentType contentType() const;
    static ServerContentType contentTypeForLoader(const QString &loaderType);
    bool addContentFiles(const QStringList &paths, QString *error = nullptr);

signals:
    void statusChanged(ServerStatus status);
    void outputReceived(const QString &line);
    void errorReceived(const QString &line);
    void started();
    void stopped();
    void serverError(const QString &message);
    void serverSoftwareDownloadFinished(const QString &targetVersion, bool success,
                                        bool cancelled, const QString &errorMessage);
    void serverCrashed(const QString &message, const QString &details);
    void playerActivity(const QString &playerName, bool joined);

private slots:
    void onProcessStarted();
    void onProcessReadyReadStandardOutput();
    void onProcessReadyReadStandardError();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

private:
    void setStatus(ServerStatus status);
    bool createServerProperties();
    void syncPortFromServerProperties();
    bool acceptEULA();
    bool hasLaunchTarget() const;
    QString loaderScriptPath() const;
    QString loaderArgumentsFile() const;
    int requiredJavaVersion() const;
    int javaMajorVersion(const QString &path) const;
    QString compatibleJavaPath(int requiredVersion, int *detectedVersion) const;
    bool installCompatibleJava(int requiredVersion);
    bool extractServerPack(const QString &archivePath, QString *error);
    void handleConsoleLine(const QString &line, bool error = false);
    bool beginServerDownload(const QString &targetVersion, const QString &targetLoaderVersion,
                             const QString &javaPath, bool startAfterDownload,
                             bool commitTargetVersion, bool commitTargetLoaderVersion);
    bool sendPlayerAdministrationCommand(const QString &verb, const QString &name,
                                         const QString &reason, QString *error);

    QString m_id;
    QString m_name;
    QString m_version;
    QString m_loaderType;
    QString m_loaderVersion;
    int m_port = 25565;
    int m_maxMemory = 2048;
    int m_minMemory = 1024;
    QString m_javaPath;
    QString m_extraJvmArguments;
    bool m_autoRestartOnCrash = false;
    bool m_eulaAccepted = false;
    int m_gracefulStopTimeoutSeconds = 10;
    QString m_serverDirectory;
    ServerStatus m_status = ServerStatus::Stopped;

    std::unique_ptr<QProcess> m_process;
    ServerDownloader *m_downloader = nullptr;
    std::shared_ptr<const ServerProviderEndpoints> m_providerEndpoints;
    Task::Ptr m_javaInstallTask;
    QString m_consoleLog;
    bool m_restartRequested = false;
    bool m_downloadCancelRequested = false;
    bool m_startupTimedOut = false;
    bool m_startupTimeoutOverridden = false;
    QDateTime m_startedAt;
    QTimer m_startupTimeoutTimer;
};
