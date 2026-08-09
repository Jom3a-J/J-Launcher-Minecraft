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
#include "ServerPackImportTransaction.h"
#include "ServerDownloader.h"
#include "ServerProperties.h"
#include "Application.h"
#include "archive/ArchiveReader.h"
#include "settings/SettingsObject.h"
#include "java/JavaUtils.h"
#include "java/JavaRuntimeInstallTask.h"
#include "logs/Privacy.h"
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

ServerInstance::ServerInstance(const QString &id, const QString &name, QObject *parent)
    : QObject(parent)
    , m_id(id)
    , m_name(name)
{
    m_startupTimeoutTimer.setSingleShot(true);
    m_startupTimeoutTimer.setInterval(120000);
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
        setStatus(ServerStatus::Error);
        if (m_process && m_process->state() != QProcess::NotRunning) {
            m_process->write("stop\n");
            QTimer::singleShot(5000, this, [this]() {
                if (m_process && m_process->state() != QProcess::NotRunning) {
                    m_process->terminate();
                }
            });
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
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        m_process->waitForFinished(5000);
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

void ServerInstance::setServerDirectory(const QString &dir)
{
    m_serverDirectory = dir;
    syncPortFromServerProperties();
}

bool ServerInstance::start()
{
    if (m_status == ServerStatus::Running || m_status == ServerStatus::Starting ||
        m_status == ServerStatus::Stopping || m_status == ServerStatus::Downloading) {
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

    const int requiredJava = qMax(requiredJavaVersion(), minimumJavaForMinecraftVersion());
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
    m_process.reset(new QProcess());
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

    QStringList jvmArgs;
    jvmArgs << QString("-Xmx%1M").arg(m_maxMemory);
    jvmArgs << QString("-Xms%1M").arg(m_minMemory);
    if (!m_extraJvmArguments.isEmpty()) {
        jvmArgs << QProcess::splitCommand(m_extraJvmArguments);
    }

    if ((loader == QStringLiteral("forge") || loader == QStringLiteral("neoforge")) &&
        QFileInfo(loaderScriptPath()).isFile()) {
        const QString loaderArgsPath = loaderArgumentsFile();
        if (!loaderArgsPath.isEmpty()) {
            QStringList args;
            if (QFile::exists(QDir(m_serverDirectory).filePath("user_jvm_args.txt"))) {
                args << QStringLiteral("@user_jvm_args.txt");
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
            // with a custom wrapper. Preserve that wrapper, but still pass the
            // settings owned by the server UI to the Java process it starts.
            QString environmentJvmArgs = QStringLiteral("-Xmx%1M -Xms%2M")
                                             .arg(m_maxMemory)
                                             .arg(m_minMemory);
            if (!m_extraJvmArguments.isEmpty()) {
                environmentJvmArgs += ' ' + m_extraJvmArguments;
            }
            serverEnvironment.insert(QStringLiteral("JAVA_TOOL_OPTIONS"), environmentJvmArgs);
            m_process->setProcessEnvironment(serverEnvironment);
            appendLog(tr("[WARN] This server uses a custom loader script; applying memory and JVM settings through the process environment."));
#ifdef Q_OS_WIN
            m_process->start("cmd.exe", QStringList() << "/d" << "/c" << "run.bat" << "nogui");
#else
            m_process->start(QStringLiteral("/bin/sh"),
                             QStringList() << loaderScriptPath() << QStringLiteral("nogui"));
#endif
        }
    } else {
        QStringList args = jvmArgs;
        args << "-jar" << serverJarPath() << "nogui";
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

    // Request a clean shutdown without blocking the launcher UI. If the
    // process ignores the command, terminate it after a grace period.
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->write("stop\n");
        const int gracePeriodMs = m_gracefulStopTimeoutSeconds * 1000;
        QTimer::singleShot(gracePeriodMs, this, [this]() {
            if (m_process && m_process->state() != QProcess::NotRunning) {
                m_process->terminate();
                QTimer::singleShot(5000, this, [this]() {
                    if (m_process && m_process->state() != QProcess::NotRunning) {
                        m_process->kill();
                    }
                });
            }
        });
    }

    return true;
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

QString ServerInstance::serverJarPath() const
{
    const QDir dir(m_serverDirectory);
    const QString directJar = dir.filePath(QStringLiteral("server.jar"));
    if (QFileInfo(directJar).isFile()) {
        return directJar;
    }

    const QString loader = m_loaderType.trimmed().toLower();
    QStringList filters;
    if (loader == QStringLiteral("forge")) {
        filters << QStringLiteral("forge-*.jar");
    } else if (loader == QStringLiteral("neoforge")) {
        filters << QStringLiteral("neoforge-*.jar");
    } else if (loader == QStringLiteral("fabric")) {
        filters << QStringLiteral("fabric-server*.jar");
    } else if (loader == QStringLiteral("paper")) {
        filters << QStringLiteral("paper-*.jar");
    } else if (loader == QStringLiteral("purpur")) {
        filters << QStringLiteral("purpur-*.jar");
    }

    if (filters.isEmpty()) {
        return directJar;
    }

    const QFileInfoList jars = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const QFileInfo &jar : jars) {
        if (!jar.fileName().contains(QStringLiteral("installer"), Qt::CaseInsensitive)) {
            return jar.absoluteFilePath();
        }
    }

    // Modern Forge and NeoForge installations can launch through run scripts
    // without a standalone server JAR in the server root. Keep the conventional
    // path for diagnostics and the Java version floor in that case.
    return directJar;
}

QString ServerInstance::serverPropertiesPath() const
{
    return QDir(m_serverDirectory).filePath("server.properties");
}

QString ServerInstance::modsDirectory() const
{
    return QDir(m_serverDirectory).filePath("mods");
}

QString ServerInstance::pluginsDirectory() const
{
    return QDir(m_serverDirectory).filePath("plugins");
}

ServerContentType ServerInstance::contentTypeForLoader(const QString &loaderType)
{
    const QString loader = loaderType.trimmed().toLower();
    if (loader == QStringLiteral("fabric")
        || loader == QStringLiteral("forge")
        || loader == QStringLiteral("neoforge")) {
        return ServerContentType::Mod;
    }
    if (loader == QStringLiteral("paper") || loader == QStringLiteral("purpur")) {
        return ServerContentType::Plugin;
    }
    return ServerContentType::None;
}

ServerContentType ServerInstance::contentType() const
{
    return contentTypeForLoader(m_loaderType);
}

QString ServerInstance::contentDirectory() const
{
    switch (contentType()) {
        case ServerContentType::Mod:
            return modsDirectory();
        case ServerContentType::Plugin:
            return pluginsDirectory();
        case ServerContentType::None:
            return QString();
    }
    return QString();
}

bool ServerInstance::hasLaunchTarget() const
{
    const QString loader = m_loaderType.trimmed().toLower();
    if ((loader == QStringLiteral("forge") || loader == QStringLiteral("neoforge")) &&
        QFileInfo(loaderScriptPath()).isFile()) {
        return true;
    }
    return QFileInfo(serverJarPath()).isFile();
}

QString ServerInstance::loaderScriptPath() const
{
#ifdef Q_OS_WIN
    const QString scriptName = QStringLiteral("run.bat");
#else
    const QString scriptName = QStringLiteral("run.sh");
#endif
    return QDir(m_serverDirectory).filePath(scriptName);
}

QString ServerInstance::loaderArgumentsFile() const
{
#ifdef Q_OS_WIN
    const QString argumentsFileName = QStringLiteral("win_args.txt");
#else
    const QString argumentsFileName = QStringLiteral("unix_args.txt");
#endif

    const QDir serverDir(m_serverDirectory);
    QFile script(loaderScriptPath());
    if (script.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString contents = QString::fromUtf8(script.readAll());
        const QRegularExpression argumentReference(
            QStringLiteral("@(?:\\\"([^\\\"\\r\\n]+%1)\\\"|([^\\s\\\"\\r\\n]+%1))")
                .arg(QRegularExpression::escape(argumentsFileName)),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = argumentReference.match(contents);
        if (match.hasMatch()) {
            const QString reference = QDir::fromNativeSeparators(
                match.captured(1).isEmpty() ? match.captured(2) : match.captured(1));
            const QString path = QFileInfo(reference).isAbsolute()
                ? reference
                : serverDir.filePath(reference);
            if (QFileInfo::exists(path)) {
                return QFileInfo(path).absoluteFilePath();
            }
        }
    }

    QStringList conventionalPaths;
    const QString loader = m_loaderType.trimmed().toLower();
    if (loader == QStringLiteral("forge")) {
        QStringList coordinates;
        if (!m_loaderVersion.isEmpty()) {
            coordinates << m_loaderVersion;
            if (!m_version.isEmpty() && !m_loaderVersion.startsWith(m_version + '-')) {
                coordinates.prepend(m_version + '-' + m_loaderVersion);
            }
        }
        for (const QString& coordinate : coordinates) {
            conventionalPaths << serverDir.filePath(
                QStringLiteral("libraries/net/minecraftforge/forge/%1/%2")
                    .arg(coordinate, argumentsFileName));
        }
    } else if (loader == QStringLiteral("neoforge") && !m_loaderVersion.isEmpty()) {
        conventionalPaths << serverDir.filePath(
            QStringLiteral("libraries/net/neoforged/neoforge/%1/%2")
                .arg(m_loaderVersion, argumentsFileName));
    }
    for (const QString& path : conventionalPaths) {
        if (QFileInfo::exists(path)) {
            return QFileInfo(path).absoluteFilePath();
        }
    }

    // Imported packs may omit version metadata. A single generated argument
    // file is unambiguous; multiple files must be left to the pack's script.
    QStringList discoveredPaths;
    QDirIterator iterator(serverDir.filePath(QStringLiteral("libraries")),
                          { argumentsFileName }, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        discoveredPaths << QFileInfo(iterator.next()).absoluteFilePath();
    }
    return discoveredPaths.size() == 1 ? discoveredPaths.first() : QString();
}

int ServerInstance::requiredJavaVersion() const
{
    if (!QFileInfo(serverJarPath()).isFile()) {
        return 0;
    }
    MMCZip::ArchiveReader archive(serverJarPath());

    QString mainClass = "net.minecraft.bundler.Main";
    if (const auto manifestFile = archive.goToFile("META-INF/MANIFEST.MF")) {
        const QString manifest = QString::fromLatin1(manifestFile->readAll());
        const QRegularExpressionMatch match =
            QRegularExpression("(?m)^Main-Class:\\s*([^\\r\\n]+)").match(manifest);
        if (match.hasMatch()) {
            mainClass = match.captured(1).trimmed();
        }
    }

    const QString classPath = mainClass;
    const QString normalizedClassPath = QString(classPath).replace('.', '/') + ".class";
    const auto classFile = archive.goToFile(normalizedClassPath);
    if (!classFile) {
        return 0;
    }
    const QByteArray header = classFile->readAll().left(8);
    if (header.size() != 8 || static_cast<unsigned char>(header.at(0)) != 0xCA ||
        static_cast<unsigned char>(header.at(1)) != 0xFE ||
        static_cast<unsigned char>(header.at(2)) != 0xBA ||
        static_cast<unsigned char>(header.at(3)) != 0xBE) {
        return 0;
    }

    const int classVersion = (static_cast<unsigned char>(header.at(6)) << 8) |
                             static_cast<unsigned char>(header.at(7));
    // Java 8 uses class-file version 52; from Java 1.1 onward, the class
    // file major version is the Java major version plus 44.
    return classVersion >= 45 ? classVersion - 44 : 0;
}

int ServerInstance::minimumJavaForMinecraftVersion() const
{
    // Some loaders (notably Fabric) start from a small bootstrap JAR whose
    // own class version is lower than the Minecraft server it loads. Keep a
    // Minecraft-version floor in addition to inspecting the launcher JAR.
    const QRegularExpression releasePattern("^1\\.(\\d+)(?:\\.(\\d+))?$");
    const QRegularExpressionMatch release = releasePattern.match(m_version);
    if (release.hasMatch()) {
        const int minor = release.captured(1).toInt();
        const int patch = release.captured(2).toInt();
        if (minor > 20 || (minor == 20 && patch >= 5)) {
            return 21;
        }
        if (minor >= 18) {
            return 17;
        }
        if (minor == 17) {
            return 16;
        }
        return 8;
    }

    // Mojang's post-1.21 release line (26.x) requires Java 25. This also
    // covers Fabric/Forge bootstrap JARs that do not expose the game class.
    if (QRegularExpression("^2[6-9]\\.").match(m_version).hasMatch()) {
        return 25;
    }
    if (QRegularExpression("^24w(1[4-9]|[2-9]\\d)[a-z]$").match(m_version).hasMatch()) {
        return 21;
    }
    return 0;
}

int ServerInstance::javaMajorVersion(const QString &path) const
{
    QProcess probe;
    probe.setProcessEnvironment(CleanEnviroment());
    probe.start(path, QStringList() << "-version");
    if (!probe.waitForStarted(3000)) {
        return 0;
    }
    probe.waitForFinished(5000);

    const QString output = QString::fromLocal8Bit(probe.readAllStandardOutput()) +
                           QString::fromLocal8Bit(probe.readAllStandardError());
    const QRegularExpressionMatch match =
        QRegularExpression("version\\s+\\\"(?:1\\.)?(\\d+)").match(output);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QString ServerInstance::compatibleJavaPath(int requiredVersion, int *detectedVersion) const
{
    QStringList candidates;
    auto addCandidate = [&candidates](const QString &path) {
        if (!path.isEmpty() && !candidates.contains(path)) {
            candidates.append(path);
        }
    };

    addCandidate(m_javaPath);
    if (auto *application = APPLICATION_DYN) {
        addCandidate(application->settings()->get("JavaPath").toString());
        JavaUtils javaUtils;
        for (const QString &path : javaUtils.FindJavaPaths()) {
            addCandidate(path);
        }
    }
    addCandidate(QStandardPaths::findExecutable("java"));
    addCandidate(QStandardPaths::findExecutable("java.exe"));

#ifdef Q_OS_WIN
    const QStringList javaRoots = {
        "C:/Program Files/Java", "C:/Program Files/Eclipse Adoptium",
        "C:/Program Files/Microsoft", "C:/Program Files/Azul Systems"
    };
    for (const QString &root : javaRoots) {
        const QDir directory(root);
        for (const QFileInfo &entry : directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QDir javaDirectory(entry.absoluteFilePath());
            addCandidate(javaDirectory.filePath("bin/java.exe"));
            addCandidate(javaDirectory.filePath("bin/javaw.exe"));
        }
    }
#endif

    int bestVersion = 0;
    for (const QString &candidate : candidates) {
        const QString managedJavaRoot = APPLICATION_DYN
            ? APPLICATION_DYN->javaPath()
            : QString();
        if (!JavaUtils::isJavaPathSafeToProbe(candidate, managedJavaRoot)) {
            continue;
        }
        const int version = javaMajorVersion(candidate);
        if (version == 0) {
            continue;
        }
        if (isJavaMajorCompatible(requiredVersion, version)) {
            if (detectedVersion) {
                *detectedVersion = version;
            }
            return candidate;
        }
        if (version > bestVersion) {
            bestVersion = version;
        }
    }

    if (detectedVersion) {
        *detectedVersion = bestVersion;
    }
    return QString();
}

bool ServerInstance::isJavaMajorCompatible(int requiredVersion, int detectedVersion)
{
    // Minecraft and its loaders are not guaranteed to run on arbitrary newer
    // Java releases. Match the game's requested major exactly; the managed
    // runtime downloader can supply it when it is absent from the system.
    return detectedVersion > 0
        && (requiredVersion == 0 || detectedVersion == requiredVersion);
}

bool ServerInstance::installCompatibleJava(int requiredVersion)
{
    if (m_javaInstallTask || requiredVersion <= 0) {
        return false;
    }

    setStatus(ServerStatus::Downloading);
    appendLog(tr("[JAVA] Installing the managed Java %1 runtime...")
                  .arg(requiredVersion));
    auto installTask = makeShared<Java::JavaRuntimeInstallTask>(requiredVersion);
    m_javaInstallTask = installTask;

    connect(installTask.get(), &Task::status, this, [this](const QString &message) {
        const QString line = QStringLiteral("[JAVA] ") + message;
        appendLog(line);
        emit outputReceived(line);
    });
    connect(installTask.get(), &Task::progress, this,
            [this](qint64 current, qint64 total) {
                if (total <= 0) {
                    return;
                }
                const QString line = tr("[JAVA] Download progress: %1%")
                                         .arg((current * 100) / total);
                appendLog(line);
                emit outputReceived(line);
            });
    connect(installTask.get(), &Task::succeeded, this, [this, installTask] {
        m_javaPath = installTask->javaPath();
        appendLog(tr("[JAVA] Managed Java installed at %1").arg(m_javaPath));
        m_javaInstallTask.reset();
        setStatus(ServerStatus::Stopped);
        QTimer::singleShot(0, this, [this] { start(); });
    });
    connect(installTask.get(), &Task::failed, this, [this](const QString &reason) {
        const QString message = tr("Automatic Java installation failed: %1").arg(reason);
        appendLog(QStringLiteral("[JAVA ERROR] ") + message);
        m_javaInstallTask.reset();
        setStatus(ServerStatus::Error);
        emit serverError(message);
    });
    connect(installTask.get(), &Task::aborted, this, [this] {
        appendLog(tr("[JAVA] Java installation cancelled."));
        m_javaInstallTask.reset();
        setStatus(ServerStatus::Stopped);
    });
    installTask->start();
    return true;
}

bool ServerInstance::addMods(const QStringList &paths, QString *error)
{
    return addContentFiles(paths, error);
}

bool ServerInstance::addContentFiles(const QStringList &paths, QString *error)
{
    const ServerContentType type = contentType();
    const QString contentName = type == ServerContentType::Plugin ? tr("plugin") : tr("mod");
    if (type == ServerContentType::None) {
        if (error) {
            *error = tr("This server type does not support launcher-managed mods or plugins.");
        }
        return false;
    }
    if (m_status != ServerStatus::Stopped && m_status != ServerStatus::Error) {
        if (error) {
            *error = tr("Stop the server before adding %1 files.").arg(contentName);
        }
        return false;
    }
    if (paths.isEmpty()) {
        if (error) {
            *error = tr("No %1 files were selected.").arg(contentName);
        }
        return false;
    }

    for (const QString &path : paths) {
        const QFileInfo source(path);
        if (!source.exists() || !source.isFile() || source.suffix().compare("jar", Qt::CaseInsensitive) != 0) {
            if (error) {
                *error = tr("'%1' is not a readable .jar %2 file.").arg(path, contentName);
            }
            return false;
        }
    }

    const QString destinationDirectory = contentDirectory();
    if (!QDir().mkpath(destinationDirectory)) {
        if (error) {
            *error = tr("Could not create the server %1 folder.").arg(
                type == ServerContentType::Plugin ? tr("plugins") : tr("mods"));
        }
        return false;
    }

    for (const QString &path : paths) {
        const QFileInfo source(path);
        const QString destination = QDir(destinationDirectory).filePath(source.fileName());
        if (QFileInfo(destination).absoluteFilePath() == source.absoluteFilePath()) {
            continue;
        }
        QFile input(source.absoluteFilePath());
        QSaveFile output(destination);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
            if (error) {
                *error = tr("Could not prepare '%1' for installation.").arg(source.fileName());
            }
            return false;
        }
        const QByteArray data = input.readAll();
        if (output.write(data) != data.size() || !output.commit()) {
            output.cancelWriting();
            if (error) {
                *error = tr("Could not install '%1' in the server %2 folder.")
                             .arg(source.fileName(),
                                  type == ServerContentType::Plugin ? tr("plugins") : tr("mods"));
            }
            return false;
        }
    }
    return true;
}

bool ServerInstance::importServerPack(const QString &archivePath, QString *error)
{
    if (m_status != ServerStatus::Stopped && m_status != ServerStatus::Error) {
        if (error) {
            *error = tr("Stop the server before importing a server pack.");
        }
        return false;
    }
    if (!QFileInfo(archivePath).isFile()) {
        if (error) {
            *error = tr("The selected server pack does not exist.");
        }
        return false;
    }
    if (!extractServerPack(archivePath, error)) {
        return false;
    }
    syncPortFromServerProperties();
    return true;
}

bool ServerInstance::extractServerPack(const QString &archivePath, QString *error)
{
    ServerPackImportTransaction transaction(m_serverDirectory);
    if (!transaction.stage(archivePath, error)) {
        return false;
    }
    return transaction.publish(error);
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

bool ServerInstance::createServerProperties()
{
    QFile file(serverPropertiesPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    stream << "#Minecraft server properties\n";
    stream << "server-port=" << m_port << "\n";
    stream << "online-mode=true\n";
    stream << "max-players=20\n";
    stream << "level-name=world\n";
    stream << "gamemode=survival\n";
    stream << "difficulty=easy\n";
    stream << "allow-nether=true\n";
    stream << "spawn-protection=16\n";
    stream << "view-distance=10\n";
    stream << "white-list=false\n";
    stream << "enable-command-block=false\n";
    stream << "spawn-monsters=true\n";
    stream << "spawn-animals=true\n";
    stream << "spawn-npcs=true\n";
    stream << "generate-structures=true\n";
    stream << "pvp=true\n";
    stream << "allow-flight=false\n";
    stream << "max-world-size=29999984\n";
    stream << "motd=A Minecraft Server\n";

    file.close();
    return true;
}

void ServerInstance::syncPortFromServerProperties()
{
    if (m_serverDirectory.trimmed().isEmpty()) {
        return;
    }
    const QFileInfo propertiesFile(serverPropertiesPath());
    if (!propertiesFile.isFile()) {
        return;
    }
    const QMap<QString, QString> properties = ServerProperties::load(serverPropertiesPath());
    bool validPort = false;
    const int configuredPort = properties.value(QStringLiteral("server-port"))
                                   .toInt(&validPort);
    if (validPort && configuredPort >= 1 && configuredPort <= 65535) {
        m_port = configuredPort;
    }
}

bool ServerInstance::acceptEULA()
{
    QFile file(QDir(m_serverDirectory).filePath("eula.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    stream << "eula=true\n";

    file.close();
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
        QString line = QString::fromLocal8Bit(m_process->readAllStandardOutput());
        QStringList lines = line.split("\n");
        for (const QString &l : lines) {
            if (!l.trimmed().isEmpty()) {
                handleConsoleLine(l.trimmed());
            }
        }
    }
}

void ServerInstance::onProcessReadyReadStandardError()
{
    if (m_process) {
        QString line = QString::fromLocal8Bit(m_process->readAllStandardError());
        QStringList lines = line.split("\n");
        for (const QString &l : lines) {
            if (!l.trimmed().isEmpty()) {
                handleConsoleLine(l.trimmed(), true);
            }
        }
    }
}

void ServerInstance::handleConsoleLine(const QString &line, bool error)
{
    const QString safeLine = Privacy::sanitizeText(line, 8192);
    const QString formatted = error ? "[ERROR] " + safeLine : safeLine;
    appendLog(formatted);
    if (error) emit errorReceived(safeLine); else emit outputReceived(safeLine);

    if (m_status == ServerStatus::Starting && isReadyOutput(line)) {
        m_startupTimeoutTimer.stop();
        setStatus(ServerStatus::Running);
    }

    static const QRegularExpression joinedPattern("([A-Za-z0-9_]{3,16}) joined the game");
    static const QRegularExpression leftPattern("([A-Za-z0-9_]{3,16}) left the game");
    const QRegularExpressionMatch joined = joinedPattern.match(line);
    if (joined.hasMatch()) {
        emit playerActivity(joined.captured(1), true);
        return;
    }
    const QRegularExpressionMatch left = leftPattern.match(line);
    if (left.hasMatch()) emit playerActivity(left.captured(1), false);
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
    m_startupTimeoutTimer.stop();
    const bool restart = m_restartRequested;
    m_restartRequested = false;
    const bool expectedStop = m_status == ServerStatus::Stopping;
    const bool exitedBeforeReady = m_status == ServerStatus::Starting;
    const bool keepErrorState = m_status == ServerStatus::Error;
    const bool abnormalExit = exitStatus == QProcess::CrashExit || exitCode != 0;
    m_startedAt = QDateTime();
    setStatus(expectedStop || restart ? ServerStatus::Stopped
                                     : (exitedBeforeReady || keepErrorState || abnormalExit
                                            ? ServerStatus::Error
                                            : ServerStatus::Stopped));

    if (exitedBeforeReady && !m_startupTimedOut) {
        const QString details = m_consoleLog.right(6000);
        const bool incompatibleJava =
            details.contains(QStringLiteral("UnsupportedClassVersionError"), Qt::CaseInsensitive)
            || details.contains(QStringLiteral("class file version"), Qt::CaseInsensitive);
        const QString message = incompatibleJava
            ? tr("The server exited because the selected Java runtime is incompatible with this Minecraft or loader version. "
                 "Choose the required Java version or enable automatic Java downloads.")
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
            appendLog("[INFO] Automatic crash restart scheduled in 5 seconds.");
            QTimer::singleShot(5000, this, [this]() { start(); });
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

    emit serverError(errorMsg);
}

bool ServerInstance::downloadServerJar(const QString &javaPath, bool startAfterDownload)
{
    return beginServerDownload(m_version, m_loaderVersion, javaPath, startAfterDownload, false, false);
}

bool ServerInstance::downloadServerJarForVersion(const QString &targetVersion,
                                                 const QString &javaPath,
                                                 bool startAfterDownload)
{
    const QString normalizedVersion = targetVersion.trimmed();
    if (normalizedVersion.isEmpty()) {
        return false;
    }
    // Provider build identifiers belong to one Minecraft version. Let the
    // provider select a compatible build when changing Minecraft versions and
    // clear the old build metadata only after the new download succeeds.
    return beginServerDownload(normalizedVersion, QString(), javaPath, startAfterDownload, true, true);
}

bool ServerInstance::downloadServerBuild(const QString &targetBuild, const QString &javaPath,
                                         bool startAfterDownload)
{
    const QString normalizedBuild = targetBuild.trimmed();
    if (normalizedBuild.isEmpty()) return false;
    return beginServerDownload(m_version, normalizedBuild, javaPath, startAfterDownload, false, true);
}

bool ServerInstance::beginServerDownload(const QString &targetVersion,
                                         const QString &targetLoaderVersion,
                                         const QString &javaPath, bool startAfterDownload,
                                         bool commitTargetVersion, bool commitTargetLoaderVersion)
{
    if (m_status == ServerStatus::Downloading || m_status == ServerStatus::Running
        || m_status == ServerStatus::Starting || m_status == ServerStatus::Stopping) {
        return false;
    }

    setStatus(ServerStatus::Downloading);
    m_downloadCancelRequested = false;

    if (m_downloader) {
        m_downloader->deleteLater();
    }

    m_downloader = m_providerEndpoints
        ? new ServerDownloader(*m_providerEndpoints, this)
        : new ServerDownloader(this);

    connect(m_downloader, &ServerDownloader::statusMessage, this, [this](const QString &msg) {
        QString formatted = QString("[DOWNLOAD] %1").arg(msg);
        appendLog(formatted);
        emit outputReceived(formatted);
    });

    connect(m_downloader, &ServerDownloader::progress, this, [this](int pct) {
        QString formatted = QString("[DOWNLOAD] Progress: %1%").arg(pct);
        appendLog(formatted);
        emit outputReceived(formatted);
    });

    connect(m_downloader, &ServerDownloader::finished, this,
            [this, startAfterDownload, targetVersion, targetLoaderVersion,
             commitTargetVersion, commitTargetLoaderVersion](bool success, const QString &err) {
        const bool cancelled = m_downloadCancelRequested;
        m_downloadCancelRequested = false;
        if (cancelled) {
            const QString formatted = tr("[DOWNLOAD] Download cancelled.");
            appendLog(formatted);
            emit outputReceived(formatted);
            m_downloader->deleteLater();
            m_downloader = nullptr;
            setStatus(ServerStatus::Stopped);
            emit serverSoftwareDownloadFinished(targetVersion, false, true, QString());
            return;
        }
        if (success) {
            QString formatted = "[DOWNLOAD] Server jar downloaded successfully!";
            appendLog(formatted);
            emit outputReceived(formatted);
            m_downloader->deleteLater();
            m_downloader = nullptr;
            if (commitTargetVersion) {
                setVersion(targetVersion);
            }
            if (commitTargetLoaderVersion) {
                setLoaderVersion(targetLoaderVersion);
            }
            setStatus(ServerStatus::Stopped);
            emit serverSoftwareDownloadFinished(targetVersion, true, false, QString());
            if (startAfterDownload) {
                start();
            }
        } else {
            QString formatted = QString("[DOWNLOAD ERROR] %1").arg(err);
            appendLog(formatted);
            emit outputReceived(formatted);
            m_downloader->deleteLater();
            m_downloader = nullptr;
            setStatus(ServerStatus::Error);
            emit serverError(err);
            emit serverSoftwareDownloadFinished(targetVersion, false, false, err);
        }
    });

    m_downloader->startDownload(targetVersion, m_loaderType, m_serverDirectory,
                                javaPath, targetLoaderVersion);
    return true;
}

bool ServerInstance::cancelDownload()
{
    if (m_status != ServerStatus::Downloading) {
        return false;
    }
    if (m_javaInstallTask) {
        return m_javaInstallTask->abort();
    }
    if (!m_downloader) {
        return false;
    }
    m_downloadCancelRequested = true;
    m_downloader->cancel();
    return true;
}

void ServerInstance::appendLog(const QString &line)
{
    m_consoleLog.append(Privacy::sanitizeText(line, 8192) + "\n");
    // Cap log size at ~100,000 characters
    if (m_consoleLog.size() > 100000) {
        m_consoleLog = m_consoleLog.right(80000);
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
