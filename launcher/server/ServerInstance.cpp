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

#include "ServerInstance.h"
#include "ServerDownloader.h"
#include "ServerProperties.h"
#include "logs/Privacy.h"
#include <QFile>
#include <QDir>
#include <QJsonObject>
#include <QTimer>

ServerInstance::ServerInstance(const QString &id, const QString &name, QObject *parent)
    : QObject(parent)
    , m_id(id)
    , m_name(name)
{
    m_startupTimeoutTimer.setSingleShot(true);
    m_startupTimeoutTimer.setInterval(120000);
    m_crashRestartTimer.setSingleShot(true);
    connect(&m_crashRestartTimer, &QTimer::timeout, this, [this]() {
        m_crashRestartTimestamps.append(QDateTime::currentDateTime());
        emit crashRestartPendingChanged(false);
        m_crashRestartStarting = true;
        start();
        m_crashRestartStarting = false;
    });
    connect(&m_startupTimeoutTimer, &QTimer::timeout, this, [this]() {
        if (m_status != ServerStatus::Starting) {
            return;
        }
        const QString message =
            tr("The Java process is still running, but the server did not report readiness within %1 seconds.")
                .arg(m_startupTimeoutTimer.interval() / 1000);
        m_startupTimedOut = true;
        appendLog("[ERROR] " + message);
        emit errorReceived(message);
        emit serverError(message);
        emit serverCrashed(message, m_consoleLog.right(6000));
        if (m_process && m_process->state() != QProcess::NotRunning) {
            setStatus(ServerStatus::Stopping);
            writeStdin(QStringLiteral("stop"));
            scheduleStopEscalation(5000);
        } else {
            setStatus(ServerStatus::Error);
        }
    });
}

ServerInstance::ServerInstance(const QString &id, const QString &name,
                               const ServerProviderEndpoints &providerEndpoints,
                               QObject *parent)
    : ServerInstance(id, name, parent)
{
    m_providerEndpoints = std::make_shared<ServerProviderEndpoints>(providerEndpoints);
}

ServerInstance::~ServerInstance()
{
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            if (m_status != ServerStatus::Stopping) {
                writeStdin(QStringLiteral("stop"));
            }
            waitForShutdown(m_gracefulStopTimeoutSeconds * 1000);
        }
    }
}

void ServerInstance::setName(const QString &name)
{
    m_name = name;
}

void ServerInstance::setVersion(const QString &version)
{
    m_version = version;
}

void ServerInstance::setLoaderType(const QString &type)
{
    m_loaderType = type;
}

void ServerInstance::setLoaderVersion(const QString &version)
{
    m_loaderVersion = version;
}

void ServerInstance::setPort(int port)
{
    m_port = qBound(1, port, 65535);

    if (QFile::exists(serverPropertiesPath())) {
        QMap<QString, QString> properties = ServerProperties::load(serverPropertiesPath());
        properties.insert(QStringLiteral("server-port"), QString::number(m_port));
        ServerProperties::save(serverPropertiesPath(), properties);
    }
}

void ServerInstance::setMaxMemory(int memory)
{
    m_maxMemory = memory;
}

void ServerInstance::setMinMemory(int memory)
{
    m_minMemory = memory;
}

void ServerInstance::setJavaPath(const QString &path)
{
    m_javaPath = path;
}

void ServerInstance::setExtraJvmArguments(const QString &arguments)
{
    m_extraJvmArguments = arguments.trimmed();
}

void ServerInstance::setAutoRestartOnCrash(bool enabled)
{
    m_autoRestartOnCrash = enabled;
}

void ServerInstance::setEulaAccepted(bool accepted)
{
    m_eulaAccepted = accepted;
}

void ServerInstance::setGracefulStopTimeoutSeconds(int seconds)
{
    m_gracefulStopTimeoutSeconds = qBound(5, seconds, 120);
}

void ServerInstance::setStartupTimeoutSeconds(int seconds)
{
    m_startupTimeoutTimer.setInterval(qBound(1, seconds, 600) * 1000);
    m_startupTimeoutOverridden = true;
}

void ServerInstance::setCrashRestartDelayMs(int ms)
{
    m_crashRestartDelayMs = qBound(100, ms, 60000);
}

void ServerInstance::setServerDirectory(const QString &dir)
{
    m_serverDirectory = dir;
    syncPortFromServerProperties();
}

bool ServerInstance::isRunning() const
{
    return m_status == ServerStatus::Running;
}

QJsonObject ServerInstance::toJson() const
{
    QJsonObject json;
    json["id"] = m_id;
    json["name"] = m_name;
    json["version"] = m_version;
    json["loaderType"] = m_loaderType;
    json["loaderVersion"] = m_loaderVersion;
    json["port"] = m_port;
    json["maxMemory"] = m_maxMemory;
    json["minMemory"] = m_minMemory;
    json["javaPath"] = m_javaPath;
    json["extraJvmArguments"] = m_extraJvmArguments;
    json["autoRestartOnCrash"] = m_autoRestartOnCrash;
    json["eulaAccepted"] = m_eulaAccepted;
    json["gracefulStopTimeoutSeconds"] = m_gracefulStopTimeoutSeconds;
    json["serverDirectory"] = m_serverDirectory;
    return json;
}

std::shared_ptr<ServerInstance> ServerInstance::fromJson(const QJsonObject &json, const QString &dataDir)
{
    QString id = json["id"].toString();
    QString name = json["name"].toString();

    auto server = std::make_shared<ServerInstance>(id, name);
    server->m_version = json["version"].toString();
    server->m_loaderType = json["loaderType"].toString();
    server->m_loaderVersion = json["loaderVersion"].toString();
    server->m_port = qBound(1, json["port"].toInt(25565), 65535);
    server->m_maxMemory = json["maxMemory"].toInt(2048);
    server->m_minMemory = json["minMemory"].toInt(1024);
    server->m_javaPath = json["javaPath"].toString();
    server->m_extraJvmArguments = json["extraJvmArguments"].toString();
    server->m_autoRestartOnCrash = json["autoRestartOnCrash"].toBool(false);
    server->m_eulaAccepted = json["eulaAccepted"].toBool(false);
    server->m_gracefulStopTimeoutSeconds = qBound(5, json["gracefulStopTimeoutSeconds"].toInt(10), 120);
    server->m_serverDirectory = json["serverDirectory"].toString();

    if (server->m_serverDirectory.isEmpty()) {
        server->m_serverDirectory = QDir(dataDir).filePath("servers/" + id);
    }
    server->syncPortFromServerProperties();

    return server;
}

void ServerInstance::setStatus(ServerStatus status)
{
    if (m_status != status) {
        m_status = status;
        emit statusChanged(status);

        if (status == ServerStatus::Running) {
            emit started();
        } else if (status == ServerStatus::Stopped) {
            emit stopped();
        }
    }
}

void ServerInstance::appendLog(const QString &line)
{
    appendSanitizedLog(Privacy::sanitizeText(line, 8192));
}

void ServerInstance::appendSanitizedLog(const QString &line)
{
    m_consoleLog.append(line + "\n");
    // Cap log size at ~100,000 characters
    if (m_consoleLog.size() > 100000) {
        m_consoleLog = m_consoleLog.right(80000);
    }
}
