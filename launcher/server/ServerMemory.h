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

#pragma once

#include <QString>
#include <cstdint>

// Automatic initial memory sizing for newly created servers.
//
// This policy is shared by both server creation paths (the manual Create
// Server dialog and Create Server from Modpack) so the rules live in exactly
// one place. The core is deterministic and unit-testable: callers inject the
// total physical RAM, the loader type, the deployed server-side jar count,
// and an optional trusted provider recommendation. Host detection
// (HardwareInfo) and instance-setting reads stay with the callers.
//
// Demand tiers (maximum heap before host clamping):
// - Vanilla and unknown loaders: about 2 GiB.
// - Optimized plugin servers (Paper, Purpur and kin): about 3 GiB.
// - Modded servers (Fabric, Forge, NeoForge, Quilt): 4 GiB baseline, then
//   5/6/8/10 GiB as the deployed server-side mod count grows (roughly
//   178 mods, e.g. RLCraft, lands around 6 GiB).
// An explicit provider recommendation is used as the demand only when the
// caller has already established it is trustworthy (see
// resolveProviderRecommendation); it is still clamped to the host.
//
// Host safety: the result reserves at least 2 GiB for Windows and other
// programs, stays at or below 75% of total physical RAM, stays on 256 MiB
// boundaries, keeps min <= max, and degrades gracefully on low-memory
// machines instead of producing invalid values.
struct ServerMemoryRecommendation {
    int minMemoryMiB = 1024;
    int maxMemoryMiB = 2048;
    // What the tier or provider recommendation asked for before host clamping.
    int demandBeforeClampMiB = 2048;
    bool fromProviderRecommendation = false;
    bool clampedToHost = false;
};

namespace ServerMemory {
constexpr int stepMiB = 256;
constexpr int minimumMiB = 256;
constexpr int maximumMiB = 32768;
constexpr int reservedForHostMiB = 2048;

bool isModdedLoader(const QString &loaderType);
bool isPluginLoader(const QString &loaderType);

// Round down to a 256 MiB boundary.
int roundDownToStep(int mebibytes);
// Keep an operator- or provider-supplied value inside the spin-box range.
int sanitizeDemand(int mebibytes);
// Tier demand before host clamping. A providerRecommendationMiB > 0 wins.
int demandFor(const QString &loaderType, int serverSideJarCount, int providerRecommendationMiB = 0);
// Clamp a demand to what the host can safely spare.
int clampMaxToHost(int demandMiB, quint64 totalRamMiB);
// Full recommendation: demand tier, host clamp, and a min derived from max.
ServerMemoryRecommendation recommend(const QString &loaderType, int serverSideJarCount, quint64 totalRamMiB,
                                     int providerRecommendationMiB = 0);
// Count deployed server-side jars: top-level *.jar files under mods/ for
// modded loaders, or under plugins/ for plugin loaders. Returns 0 for
// vanilla/unknown loaders and missing folders. Disabled files (*.disabled)
// are not counted because the loader does not load them.
int countDeployedServerJars(const QString &serverDirectory, const QString &loaderType);
// Trust rule for an explicit pack recommendation: use the instance's memory
// override when it truly overrides memory, else the exported recommendation
// some providers publish, else 0 (infer from loader family and jar count).
// Pure so it can be unit-tested without constructing an instance.
int resolveProviderRecommendation(bool overrideMemory, int maxMemAllocMiB, int exportRecommendedRamMiB);
}  // namespace ServerMemory
