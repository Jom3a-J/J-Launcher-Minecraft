// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QtGlobal>
#include <QString>
#include <QStringList>

enum class ServerHealthState {
    Inactive,
    Unavailable,
    Measuring,
    Healthy,
    Warning
};

enum class ServerMetricLevel {
    Unavailable,
    Normal,
    Approaching,
    Warning
};

enum class ServerCrashCause {
    JavaVersion,
    LaunchFiles,
    PortConflict,
    Eula,
    Content,
    Memory,
    Unknown
};

struct ServerHealthInput {
    bool running = false;
    bool processMetricsAvailable = false;
    double cpuPercent = -1.0;
    qint64 workingSetBytes = -1;
    int configuredRamMiB = 0;
    bool diskAvailable = false;
    qint64 diskBytesAvailable = -1;
    int cpuWarningPercent = 85;
    int ramWarningPercent = 90;
    int diskWarningGiB = 2;
};

struct ServerHealthAssessment {
    ServerHealthState state = ServerHealthState::Inactive;
    ServerMetricLevel cpu = ServerMetricLevel::Unavailable;
    ServerMetricLevel ram = ServerMetricLevel::Unavailable;
    ServerMetricLevel disk = ServerMetricLevel::Unavailable;
    double ramPercent = -1.0;
};

class ServerDiagnostics final
{
public:
    static ServerHealthAssessment assessHealth(const ServerHealthInput& input);
    static ServerCrashCause classifyCrash(const QString& log);
    static QString crashCauseExplanation(ServerCrashCause cause);
    static QString crashRelevantLine(const QString& log);
    static QStringList suspectedModIds(const QString& log);
};
