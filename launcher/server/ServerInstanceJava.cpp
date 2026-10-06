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

// Choosing a Java runtime that matches the server, and installing one when none does.

#include "ServerInstance.h"
#include "Application.h"
#include "archive/ArchiveReader.h"
#include "settings/SettingsObject.h"
#include "java/JavaUtils.h"
#include "java/JavaRuntimeInstallTask.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace {
struct JavaProbeCacheEntry {
    QDateTime lastModified;
    qint64 size = -1;
    int majorVersion = 0;
};

QMutex javaProbeCacheMutex;
QHash<QString, JavaProbeCacheEntry> javaProbeCache;
int javaProbeCount = 0;
}

int ServerInstance::requiredJavaVersion() const
{
    if (!QFileInfo(serverJarPath()).isFile()) {
        return 0;
    }
    MMCZip::ArchiveReader archive(serverJarPath());

    QString mainClass = "net.minecraft.bundler.Main";
    if (const auto manifestFile = archive.goToFile("META-INF/MANIFEST.MF")) {
        const QString manifest = QString::fromLatin1((*manifestFile)->readAll().value_or(QByteArray()));
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
    const QByteArray header = (*classFile)->readAll().value_or(QByteArray()).left(8);
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

int ServerInstance::recommendedJavaMajor(const QString &minecraftVersion,
                                         const QString &loaderType)
{
    // Some loaders (notably Fabric) start from a small bootstrap JAR whose
    // own class version is lower than the Minecraft server it loads. Keep a
    // Minecraft-version floor in addition to inspecting the launcher JAR.
    const QRegularExpression releasePattern("^1\\.(\\d+)(?:\\.(\\d+))?$");
    const QRegularExpressionMatch release = releasePattern.match(
        minecraftVersion.trimmed());
    if (release.hasMatch()) {
        const int minor = release.captured(1).toInt();
        const int patch = release.captured(2).toInt();
        const QString loader = loaderType.trimmed().toLower();

        // Paper and Purpur publish a stricter runtime matrix than the game
        // itself. Purpur is Paper-based and follows the same Java floor.
        if (loader == QStringLiteral("paper")
            || loader == QStringLiteral("purpur")) {
            if (minor >= 20) {
                return 21;
            }
            if (minor >= 17) {
                return 17;
            }
            if (minor == 16 && patch >= 5) {
                return 16;
            }
            if (minor >= 12) {
                return 11;
            }
            return 8;
        }

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
    if (QRegularExpression("^2[6-9]\\.").match(minecraftVersion).hasMatch()) {
        return 25;
    }
    if (QRegularExpression("^24w(1[4-9]|[2-9]\\d)[a-z]$").match(minecraftVersion).hasMatch()) {
        return 21;
    }
    return 0;
}

int ServerInstance::javaMajorVersion(const QString &path) const
{
    const QFileInfo fileInfo(path);
    const QString absolutePath = fileInfo.absoluteFilePath();
    const QDateTime lastModified = fileInfo.lastModified();
    const qint64 size = fileInfo.size();
    {
        QMutexLocker locker(&javaProbeCacheMutex);
        const auto cached = javaProbeCache.constFind(absolutePath);
        if (cached != javaProbeCache.cend()
            && cached->lastModified == lastModified && cached->size == size) {
            return cached->majorVersion;
        }
        ++javaProbeCount;
    }
    QProcess probe;
    probe.setProcessEnvironment(CleanEnviroment());
    probe.start(path, QStringList() << "-version");
    int majorVersion = 0;
    if (probe.waitForStarted(3000)) {
        probe.waitForFinished(5000);
        const QString output = QString::fromLocal8Bit(probe.readAllStandardOutput()) +
                               QString::fromLocal8Bit(probe.readAllStandardError());
        const QRegularExpressionMatch match =
            QRegularExpression("version\\s+\\\"(?:1\\.)?(\\d+)").match(output);
        majorVersion = match.hasMatch() ? match.captured(1).toInt() : 0;
    }
    {
        QMutexLocker locker(&javaProbeCacheMutex);
        javaProbeCache.insert(absolutePath, { lastModified, size, majorVersion });
    }
    return majorVersion;
}

int ServerInstance::javaProbeCountForTesting()
{
    QMutexLocker locker(&javaProbeCacheMutex);
    return javaProbeCount;
}

void ServerInstance::clearJavaProbeCacheForTesting()
{
    QMutexLocker locker(&javaProbeCacheMutex);
    javaProbeCache.clear();
    javaProbeCount = 0;
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

bool ServerInstance::installCompatibleJava(int requiredVersion,
                                           bool startAfterInstall,
                                           bool prepareServerAfterInstall)
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
    connect(installTask.get(), &Task::succeeded, this,
            [this, installTask, startAfterInstall, prepareServerAfterInstall] {
        m_javaPath = installTask->javaPath();
        appendLog(tr("[JAVA] Managed Java installed at %1").arg(m_javaPath));
        m_javaInstallTask.reset();
        setStatus(ServerStatus::Stopped);
        if (prepareServerAfterInstall) {
            QTimer::singleShot(0, this, [this] { prepareServerSoftware(); });
        } else if (startAfterInstall) {
            QTimer::singleShot(0, this, [this] { start(); });
        }
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
