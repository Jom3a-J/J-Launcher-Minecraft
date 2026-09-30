// SPDX-License-Identifier: GPL-3.0-only

#include "ServerDiagnostics.h"
#include "ServerInstance.h"
#include "archive/ArchiveReader.h"
#include "logs/Privacy.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace {
ServerMetricLevel percentLevel(double value, int warning)
{
    if (value < 0.0 || warning <= 0) {
        return ServerMetricLevel::Unavailable;
    }
    if (value >= warning) {
        return ServerMetricLevel::Warning;
    }
    return value >= warning - 15
        ? ServerMetricLevel::Approaching
        : ServerMetricLevel::Normal;
}
}

ServerHealthAssessment ServerDiagnostics::assessHealth(const ServerHealthInput& input)
{
    ServerHealthAssessment assessment;
    if (!input.running) {
        return assessment;
    }
    if (!input.processMetricsAvailable) {
        assessment.state = ServerHealthState::Unavailable;
        return assessment;
    }

    assessment.cpu = percentLevel(input.cpuPercent, input.cpuWarningPercent);
    if (input.workingSetBytes >= 0 && input.configuredRamMiB > 0) {
        assessment.ramPercent = input.workingSetBytes * 100.0
            / (input.configuredRamMiB * 1024.0 * 1024.0);
        assessment.ram = percentLevel(assessment.ramPercent, input.ramWarningPercent);
    }
    if (input.diskAvailable && input.diskBytesAvailable >= 0
        && input.diskWarningGiB > 0) {
        const qint64 threshold = qint64(input.diskWarningGiB) * 1024 * 1024 * 1024;
        assessment.disk = input.diskBytesAvailable <= threshold
            ? ServerMetricLevel::Warning
            : ServerMetricLevel::Normal;
    }

    if (assessment.cpu == ServerMetricLevel::Warning
        || assessment.ram == ServerMetricLevel::Warning
        || assessment.disk == ServerMetricLevel::Warning) {
        assessment.state = ServerHealthState::Warning;
    } else if (assessment.cpu == ServerMetricLevel::Unavailable) {
        assessment.state = ServerHealthState::Measuring;
    } else {
        assessment.state = ServerHealthState::Healthy;
    }
    return assessment;
}

ServerCrashCause ServerDiagnostics::classifyCrash(const QString& log)
{
    const QString lower = log.toLower();
    if (lower.contains("unsupportedclassversionerror")
        || lower.contains("requires the use of java")
        || lower.contains("class file version")
        || lower.contains("unrecognized vm option")
        || lower.contains("unrecognized option")
        || lower.contains("failed to start server. is java installed")) {
        return ServerCrashCause::JavaVersion;
    }
    if (lower.contains("unable to access jarfile")
        || lower.contains("could not find or load main class")) {
        return ServerCrashCause::LaunchFiles;
    }
    if (lower.contains("failed to bind to port")
        || lower.contains("address already in use")) {
        return ServerCrashCause::PortConflict;
    }
    if (lower.contains("you need to agree to the eula")) {
        return ServerCrashCause::Eula;
    }
    if (lower.contains("nosuchmethoderror")
        || lower.contains("classnotfoundexception")
        || lower.contains("mod resolution encountered")
        || lower.contains("mod resolution failed")
        || lower.contains("incompatible mods found")
        || lower.contains("missing mandatory dependenc")
        || lower.contains("modloadingexception")
        || lower.contains("duplicate mods found")
        || (lower.contains("cannot load class")
            && lower.contains("environment type server"))
        || (lower.contains("attempted to load class")
            && lower.contains("invalid dist dedicated_server"))
        || (lower.contains("requires version") && lower.contains("missing"))) {
        return ServerCrashCause::Content;
    }
    if (lower.contains("outofmemoryerror")
        || lower.contains("could not reserve enough space")) {
        return ServerCrashCause::Memory;
    }
    return ServerCrashCause::Unknown;
}

QString ServerDiagnostics::crashCauseExplanation(ServerCrashCause cause)
{
    switch (cause) {
        case ServerCrashCause::JavaVersion:
            return QObject::tr("The selected Java version, or a Java option, does not match what this server needs. It may be too old or too new.");
        case ServerCrashCause::LaunchFiles:
            return QObject::tr("The server JAR or a required launch file is missing or cannot be opened.");
        case ServerCrashCause::PortConflict:
            return QObject::tr("Another application is already using the configured server port.");
        case ServerCrashCause::Eula:
            return QObject::tr("The Minecraft EULA has not been accepted in eula.txt.");
        case ServerCrashCause::Content:
            return QObject::tr("A mod, plugin, loader, or dependency is missing or incompatible. Review the reported error and make the server content versions agree.");
        case ServerCrashCause::Memory:
            return QObject::tr("The Java process ran out of memory or could not reserve the configured amount.");
        case ServerCrashCause::Unknown:
            return QObject::tr("Check the final log lines below for the server's reported cause.");
    }
    return {};
}

QString ServerDiagnostics::crashRelevantLine(const QString& log)
{
    const QStringList lines = log.split('\n', Qt::SkipEmptyParts);
    const QStringList priorityMarkers = {
        QStringLiteral("which is missing"),
        QStringLiteral("missing mandatory dependenc"),
        QStringLiteral("unsupportedclassversionerror"),
        QStringLiteral("could not reserve enough space"),
        QStringLiteral("outofmemoryerror"),
        QStringLiteral("unable to access jarfile"),
        QStringLiteral("could not find or load main class"),
        QStringLiteral("failed to bind to port"),
        QStringLiteral("address already in use"),
        QStringLiteral("you need to agree to the eula"),
        QStringLiteral("incompatible mods found"),
        QStringLiteral("mod resolution failed"),
        QStringLiteral("modloadingexception"),
        QStringLiteral("duplicate mods found"),
        QStringLiteral("environment type server"),
        QStringLiteral("caused by:"),
        QStringLiteral("[error]")
    };

    for (const QString& marker : priorityMarkers) {
        for (auto iterator = lines.crbegin(); iterator != lines.crend(); ++iterator) {
            if (!iterator->contains(marker, Qt::CaseInsensitive)) {
                continue;
            }
            QString relevant = iterator->trimmed();
            if (relevant.size() > 1000) {
                relevant = relevant.left(997) + QStringLiteral("...");
            }
            return relevant;
        }
    }

    for (auto iterator = lines.crbegin(); iterator != lines.crend(); ++iterator) {
        const QString relevant = iterator->trimmed();
        if (!relevant.isEmpty() && !relevant.startsWith(QStringLiteral("at "))) {
            return relevant.size() > 1000
                ? relevant.left(997) + QStringLiteral("...")
                : relevant;
        }
    }
    return {};
}

QStringList ServerDiagnostics::suspectedModIds(const QString& log)
{
    QSet<QString> identifiers;
    const QList<QRegularExpression> patterns{
        // Fabric mixin handler names embed the responsible mod id between
        // dollar signs, for example handler$abc$my_mod$method.
        QRegularExpression(
            QStringLiteral(R"(handler\$[^\s$]*\$([a-z0-9_.-]+)\$)"),
            QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(
            QStringLiteral(R"(mixin[^\r\n]*?\bfrom mod\s+['\"]?([a-z0-9_.-]+))"),
            QRegularExpression::CaseInsensitiveOption),
        // Forge mod-loading crash reports identify the failing mod in a
        // section heading such as "-- MOD ruokmod --".
        QRegularExpression(
            QStringLiteral(R"((?:^|[\r\n])--\s*MOD\s+([a-z0-9_.-]+)\s*--(?:[\r\n]|$))"),
            QRegularExpression::CaseInsensitiveOption),
    };
    for (const QRegularExpression& pattern : patterns) {
        auto matches = pattern.globalMatch(log);
        while (matches.hasNext()) {
            const QString identifier = matches.next().captured(1).toLower();
            if (!identifier.isEmpty()) identifiers.insert(identifier);
        }
    }
    QStringList result(identifiers.cbegin(), identifiers.cend());
    result.sort(Qt::CaseInsensitive);
    return result;
}

QString ServerDiagnostics::crashSummary(const QString &message, const QString &rawLog)
{
    const QString relevantLine = Privacy::sanitizeText(
        crashRelevantLine(rawLog), 1000);
    QString summary = QObject::tr("%1 — %2\nLikely cause: %3")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
             Privacy::sanitizeText(message),
             crashCauseExplanation(classifyCrash(rawLog)));
    if (!relevantLine.isEmpty()) {
        summary += QObject::tr("\nServer reported: %1").arg(relevantLine);
    }
    return summary;
}

QString ServerDiagnostics::structuredCrashDetails(const ServerInstance &server,
                                                   const QString &message,
                                                   const QString &rawLog)
{
    const QString loader = server.loaderVersion().isEmpty()
        ? server.loaderType()
        : server.loaderType() + " " + server.loaderVersion();
    const QString serverContentDirectory = server.contentDirectory();
    const QStringList content = serverContentDirectory.isEmpty()
        ? QStringList()
        : QDir(serverContentDirectory).entryList(
              QStringList() << "*.jar" << "*.jar.disabled",
              QDir::Files, QDir::Name | QDir::IgnoreCase);
    QStringList finalLines;
    const QStringList allLines = rawLog.split('\n', Qt::SkipEmptyParts);
    for (int index = qMax(0, allLines.size() - 25); index < allLines.size(); ++index) {
        finalLines << Privacy::sanitizeText(allLines.at(index), 8192);
    }

    QStringList report;
    const QString relevantLine = Privacy::sanitizeText(crashRelevantLine(rawLog), 1000);
    report << QObject::tr("Crash summary")
           << QObject::tr("Time: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate))
           << QObject::tr("Message: %1").arg(Privacy::sanitizeText(message))
           << QObject::tr("Likely cause: %1").arg(crashCauseExplanation(classifyCrash(rawLog)))
           << QObject::tr("Reported error: %1").arg(relevantLine.isEmpty()
                  ? QObject::tr("No specific error line was found.") : relevantLine)
           << QObject::tr("Minecraft: %1").arg(server.version())
           << QObject::tr("Server type: %1").arg(loader)
           << QObject::tr("Java: %1").arg(server.javaPath().isEmpty()
                  ? QObject::tr("system default") : Privacy::sanitizePath(server.javaPath()))
           << QObject::tr("Memory: %1 MiB minimum / %2 MiB maximum")
                  .arg(server.minMemory()).arg(server.maxMemory())
           << QObject::tr("Installed content (%1): %2")
                  .arg(content.size()).arg(content.isEmpty()
                      ? QObject::tr("none") : content.join(", "))
           << QString()
           << QObject::tr("Final server log lines:")
           << (finalLines.isEmpty() ? QObject::tr("No server output was captured.")
                                    : finalLines.join('\n'));
    return report.join('\n');
}

QStringList ServerDiagnostics::modIdsFromJar(const QString& path)
{
    QStringList identifiers;
    MMCZip::ArchiveReader fabricArchive(path);
    if (const auto metadata = fabricArchive.goToFile(QStringLiteral("fabric.mod.json"))) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            metadata->readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            const QJsonObject object = document.object();
            identifiers << object.value(QStringLiteral("id")).toString().toLower();
            for (const QJsonValue& provided :
                 object.value(QStringLiteral("provides")).toArray()) {
                identifiers << provided.toString().toLower();
            }
        }
    }

    MMCZip::ArchiveReader quiltArchive(path);
    if (const auto metadata = quiltArchive.goToFile(QStringLiteral("quilt.mod.json"))) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            metadata->readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            identifiers << document.object()
                               .value(QStringLiteral("quilt_loader"))
                               .toObject()
                               .value(QStringLiteral("id"))
                               .toString()
                               .toLower();
        }
    }

    for (const QString& metadataPath : {
             QStringLiteral("META-INF/mods.toml"),
             QStringLiteral("META-INF/neoforge.mods.toml") }) {
        MMCZip::ArchiveReader forgeArchive(path);
        if (const auto metadata = forgeArchive.goToFile(metadataPath)) {
            const QString contents = QString::fromUtf8(metadata->readAll());
            static const QRegularExpression modIdExpression(
                QStringLiteral(R"((?im)^\s*modId\s*=\s*[\"']([a-z0-9_.-]+)[\"'])"));
            auto matches = modIdExpression.globalMatch(contents);
            while (matches.hasNext()) {
                identifiers << matches.next().captured(1).toLower();
            }
        }
    }
    identifiers.removeAll(QString());
    identifiers.removeDuplicates();
    return identifiers;
}

QStringList ServerDiagnostics::suspectedModFiles(const QString& modsDirectory, const QString& log)
{
    const QStringList suspectedIds = suspectedModIds(log);
    if (suspectedIds.isEmpty()) return {};

    QStringList matches;
    const QDir directory(modsDirectory);
    for (const QFileInfo& jar : directory.entryInfoList(
             QStringList() << QStringLiteral("*.jar"), QDir::Files)) {
        const QStringList ids = modIdsFromJar(jar.absoluteFilePath());
        for (const QString& suspectedId : suspectedIds) {
            if (ids.contains(suspectedId, Qt::CaseInsensitive)) {
                matches << jar.absoluteFilePath();
                break;
            }
        }
    }
    return matches;
}
