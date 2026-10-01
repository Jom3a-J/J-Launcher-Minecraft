// SPDX-License-Identifier: GPL-3.0-only

#include "ServerProcessStats.h"

#ifdef Q_OS_WIN
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace ServerProcessStats {

#ifdef Q_OS_WIN
namespace {
quint64 fileTimeMilliseconds(const FILETIME &fileTime)
{
    ULARGE_INTEGER value;
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart / 10000;
}
}  // namespace

bool read(qint64 processId, ServerProcessSnapshot *snapshot)
{
    if (!snapshot || processId <= 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(processId));
    if (!process) return false;

    FILETIME creation, exitTime, kernel, user;
    const bool haveTimes = GetProcessTimes(process, &creation, &exitTime, &kernel, &user);
    using GetProcessMemoryInfoFunction = BOOL (WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    static GetProcessMemoryInfoFunction getProcessMemoryInfo = nullptr;
    static bool memoryFunctionLoaded = false;
    if (!memoryFunctionLoaded) {
        HMODULE psapi = GetModuleHandleW(L"psapi.dll");
        if (!psapi) psapi = LoadLibraryW(L"psapi.dll");
        if (psapi) getProcessMemoryInfo = reinterpret_cast<GetProcessMemoryInfoFunction>(GetProcAddress(psapi, "GetProcessMemoryInfo"));
        memoryFunctionLoaded = true;
    }

    PROCESS_MEMORY_COUNTERS_EX memoryCounters = {};
    memoryCounters.cb = sizeof(memoryCounters);
    const bool haveMemory = getProcessMemoryInfo
        && getProcessMemoryInfo(process, reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&memoryCounters), sizeof(memoryCounters));
    CloseHandle(process);
    if (!haveTimes || !haveMemory) return false;

    snapshot->cpuMilliseconds = fileTimeMilliseconds(kernel) + fileTimeMilliseconds(user);
    snapshot->workingSetBytes = static_cast<qint64>(memoryCounters.WorkingSetSize);
    return true;
}

qint64 workProcessId(qint64 launchedProcessId)
{
    if (launchedProcessId <= 0) return launchedProcessId;
    HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE) return launchedProcessId;

    QHash<DWORD, QList<QPair<DWORD, QString>>> children;
    QString launchedName;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(processes, &entry); more; more = Process32NextW(processes, &entry)) {
        const QString name = QString::fromWCharArray(entry.szExeFile).toLower();
        if (entry.th32ProcessID == static_cast<DWORD>(launchedProcessId)) {
            launchedName = name;
        }
        children[entry.th32ParentProcessID].append({ entry.th32ProcessID, name });
    }
    CloseHandle(processes);

    const auto isJava = [](const QString &name) {
        return name == QStringLiteral("java.exe") || name == QStringLiteral("javaw.exe");
    };
    if (isJava(launchedName)) return launchedProcessId;

    QList<DWORD> level{ static_cast<DWORD>(launchedProcessId) };
    for (int depth = 0; depth < 3 && !level.isEmpty(); ++depth) {
        QList<DWORD> next;
        for (const DWORD parent : level) {
            for (const auto &child : children.value(parent)) {
                if (isJava(child.second)) return child.first;
                next.append(child.first);
            }
        }
        level = next;
    }
    return launchedProcessId;
}
#else
bool read(qint64, ServerProcessSnapshot *)
{
    return false;
}

qint64 workProcessId(qint64 launchedProcessId)
{
    return launchedProcessId;
}
#endif

}  // namespace ServerProcessStats
