// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QtGlobal>

/// CPU time and memory of one process at one moment.
struct ServerProcessSnapshot {
    /// Kernel plus user CPU time used since the process started.
    quint64 cpuMilliseconds = 0;
    qint64 workingSetBytes = 0;
};

/// Reads the live load of a running server for the Server Manager's overview.
namespace ServerProcessStats {

/// False when the process is gone or cannot be read (and always outside Windows).
bool read(qint64 processId, ServerProcessSnapshot *snapshot);

/*! The process whose load should be shown for a server.
 *
 *  Forge and NeoForge servers start through run.bat, so the launched process is cmd.exe,
 *  which uses almost no CPU or memory. The Java process it starts is the real server; returns
 *  that (searching a couple of levels down), or launchedProcessId itself when there is none.
 */
qint64 workProcessId(qint64 launchedProcessId);

}  // namespace ServerProcessStats
