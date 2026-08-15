# Phase 11 — Final Performance and Responsiveness Optimization

**Status:** Complete — 13 August 2026

**Baseline commit:** `8a0daa2a7`

**Reference configuration:** Windows MSVC `RelWithDebInfo` and `Release`

## Measurement rule

Performance changes must answer a measured failure or material delay. Debug-build
timings are diagnostic only and do not certify this phase. Each accepted change
must be compared against the same scenario before functional and visual
regression checks run.

The repository benchmark is `scripts/phase11-performance.ps1`. It launches only
disposable data profiles, uses native Windows UI Automation to reach the main
window and open Settings and Servers, samples settled process resources, removes
its temporary profiles, and can fail when a median exceeds its budget. It does
not send telemetry or retain user data.

## Initial optimized baseline

Measured on 13 August 2026 from a freshly built `RelWithDebInfo` launcher. One
warm-up run was excluded; the table contains medians from seven measured runs.

| Metric | Median | Initial budget | Result |
| --- | ---: | ---: | --- |
| Quick Setup window | 267.9 ms | 750 ms | Pass |
| Main window after finishing setup | 629.0 ms | 1,500 ms | Pass |
| Settings native window | 136.3 ms | 500 ms | Pass |
| Servers native window | 116.3 ms | 500 ms | Pass |
| Settled idle CPU | 0.000% | 0.5% | Pass |
| Private memory | 53.5 MiB | 160 MiB | Pass |
| Working set | 124.0 MiB | Recorded, no initial gate | — |
| Handles | 1,223 | Recorded, no initial gate | — |
| Threads | 20 | Recorded, no initial gate | — |

The budgets deliberately leave headroom for machine variance while catching a
large regression. They are starting guardrails, not universal hardware claims.

Reproduction from a Visual Studio developer shell, adjusting the Qt path when
needed:

```powershell
cmake --build build-migration-build2 --config RelWithDebInfo --target JLauncher --parallel 4
./scripts/phase11-performance.ps1 `
    -Executable ./build-migration-build2/RelWithDebInfo/jlauncher.exe `
    -DependencyPath C:/Qt/6.10.2/msvc2022_64/bin `
    -Runs 7 `
    -WarmupRuns 1 `
    -IdleSampleSeconds 3 `
    -EnforceBudgets
```

## What the baseline establishes

- Optimized empty-profile startup and the two targeted navigation surfaces are
  within their initial responsiveness budgets.
- The process remained idle during every sampled interval.
- The previously identified translation-refresh concern is already addressed in
  the migrated source by a single-shot debounce, unchanged-model detection, and
  unchanged-catalog detection. Reimplementing it would not be measurement-driven.
- No runtime code change is justified by this baseline alone.

## Representative populated profile

The disposable populated scenario contains 24 minimal valid launcher instances
and 12 local servers spanning the supported loader families. Each server has
representative configuration, log, world, player, mod/plugin, and backup data.
The benchmark verifies that the first generated server is visible before timing
Console, Mods, Players, Files, Backups, Tools, Settings, and Home selection.

One warm-up was excluded from each table below; each value is the median of
seven measured runs. All measurements were taken on 13 August 2026.

| Build / scenario | Setup | Main | Settings | Servers | Idle CPU | Private memory |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `RelWithDebInfo`, populated | 428.1 ms | 816.5 ms | 110.8 ms | 103.8 ms | 0.000% | 28.1 MiB |
| `Release`, empty | 247.3 ms | 567.7 ms | 103.0 ms | 73.8 ms | 0.000% | 45.1 MiB |
| `Release`, populated | 450.7 ms | 837.8 ms | 108.4 ms | 97.8 ms | 0.000% | 28.9 MiB |

The populated `Release` server-destination medians were 0.6 ms for Console,
Mods, Players, Backups, Tools, and Home, and 0.7 ms for Files and Settings. All
startup, navigation, idle-CPU, private-memory, and server-destination budgets
passed. Working set, handle count, and thread count remain recorded diagnostics,
not gates. The lower private-memory readings in the populated scenario should
not be interpreted as a profile-size improvement because window/process state
differs between samples.

No measured runtime bottleneck justified an application-code optimization.

## Release package gate

The MSVC `Release` launcher and file-link helper built successfully. Staging the
standalone bundle exposed and fixed two packaging defects:

- `qtlogging.ini` used the configure-time install prefix, which could direct it
  outside an explicitly selected staging prefix.
- The installed artifact omitted the repository `LICENSE` and combined
  `COPYING.md` notices.

The corrected local bundle contains the launcher, file-link helper, Java helper
JARs, Qt libraries/plugins, `portable.txt`, `qt.conf`, `qtlogging.ini`,
`LICENSE`, `COPYING.md`, and the supported `vc_redist.x64.exe` prerequisite.
Empty and populated one-run smoke tests launched the staged executable without a
Qt development path and passed every budget. A final populated smoke reduced
`PATH` to Windows system directories and still passed: main 1,008.5 ms, Settings
135.8 ms, Servers 95.9 ms, and idle CPU 0.000%. CMake reported non-fatal CMP0207
path-normalization policy warnings while resolving Windows runtime dependencies;
installation still completed successfully.

The current HEAD also completed a full MSVC Debug build and passed all 34 native
CTest targets. The Qt runtime directory must be present on `PATH` when invoking
the Debug test executables; without it Windows exits before test code with
status `0xc0000135`.

## Visual and data-safety regression

The benchmark can optionally capture the rendered main, Settings, and Servers
windows. A populated Release capture showed complete painting, readable controls,
consistent dark theming, and no blocking clipping or overlap. The capture run
also passed every performance budget.

A final populated run used an explicit disposable profile root. The harness
removed the complete generated root, left no packaged launcher process running,
and did not target the user's normal launcher data.

## Cache-control limitation

The benchmark distinguishes excluded warm-up runs from measured repeats, and
its JSON report now records that the operating-system file cache is not evicted.
The first-process samples are observational; they are not certified controlled
cold starts. A rebooted or otherwise approved cache-controlled environment is
still required to close that gate.

## Closure

Phase 11 is officially complete. Final user validation was received on
13 August 2026. Empty and representative populated profiles pass every
performance budget in optimized builds, all server destinations remain
responsive, settled CPU remains idle, the full native suite passes, visible UI
and disposable-data regressions pass, and the dependency-isolated Release bundle
passes with its required redistributable included. No measured runtime
bottleneck justified speculative launcher/UI changes.

Certified post-reboot cold-start measurement and execution on a genuinely fresh
Windows machine cannot be produced inside the current development session. They
remain explicit Phase 9 release-qualification checks rather than being
misrepresented by warm OS-cache evidence. Stable publication remains a separate
maintainer decision.
