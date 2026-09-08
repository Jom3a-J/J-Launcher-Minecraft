// SPDX-License-Identifier: GPL-3.0-only

#include "ServerDiagnostics.h"

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
            return QObject::tr("The selected Java version is too old for this server version.");
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
