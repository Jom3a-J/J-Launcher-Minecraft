// SPDX-License-Identifier: GPL-3.0-only

#include "ServerDiagnostics.h"

#include <QObject>

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
        || lower.contains("requires the use of java")) {
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
        || lower.contains("mod resolution encountered")) {
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
            return QObject::tr("A mod, plugin, loader, or dependency is incompatible or missing.");
        case ServerCrashCause::Memory:
            return QObject::tr("The Java process ran out of memory or could not reserve the configured amount.");
        case ServerCrashCause::Unknown:
            return QObject::tr("Check the final log lines below for the server's reported cause.");
    }
    return {};
}
