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

#include "server/ServerMemory.h"

#include <QDir>
#include <QtGlobal>

namespace ServerMemory {

bool isModdedLoader(const QString &loaderType)
{
    const QString loader = loaderType.trimmed().toLower();
    return loader == QStringLiteral("fabric") || loader == QStringLiteral("forge") ||
           loader == QStringLiteral("neoforge") || loader == QStringLiteral("quilt");
}

bool isPluginLoader(const QString &loaderType)
{
    const QString loader = loaderType.trimmed().toLower();
    return loader == QStringLiteral("paper") || loader == QStringLiteral("purpur") ||
           loader == QStringLiteral("spigot") || loader == QStringLiteral("bukkit");
}

int roundDownToStep(int mebibytes)
{
    if (mebibytes < 0) {
        return 0;
    }
    return (mebibytes / stepMiB) * stepMiB;
}

int sanitizeDemand(int mebibytes)
{
    if (mebibytes <= 0) {
        return 0;
    }
    return qBound(minimumMiB, roundDownToStep(mebibytes), maximumMiB);
}

int demandFor(const QString &loaderType, int serverSideJarCount, int providerRecommendationMiB)
{
    const int trusted = sanitizeDemand(providerRecommendationMiB);
    if (trusted > 0) {
        return trusted;
    }
    if (isPluginLoader(loaderType)) {
        return 3072;
    }
    if (isModdedLoader(loaderType)) {
        const int mods = qMax(0, serverSideJarCount);
        if (mods <= 20) {
            return 4096;
        }
        if (mods <= 60) {
            return 5120;
        }
        if (mods <= 200) {
            // Covers large packs such as RLCraft (~178 mods) at ~6 GiB.
            return 6144;
        }
        if (mods <= 300) {
            return 8192;
        }
        return 10240;
    }
    return 2048;
}

int clampMaxToHost(int demandMiB, quint64 totalRamMiB)
{
    const int demand = sanitizeDemand(demandMiB);
    if (demand <= 0) {
        return 1024;
    }
    if (totalRamMiB < 1024) {
        // Unknown or implausible host size: do not pretend to clamp, just
        // keep the demand inside the valid range.
        return qBound(512, demand, maximumMiB);
    }
    const int fractionCap = roundDownToStep(static_cast<int>((totalRamMiB * 3) / 4));
    const int reserveCap = roundDownToStep(static_cast<int>(totalRamMiB) - reservedForHostMiB);
    int upper = fractionCap;
    if (reserveCap >= 1024) {
        upper = qMin(upper, reserveCap);
    }
    upper = qBound(512, upper, maximumMiB);
    return qMin(demand, upper);
}

ServerMemoryRecommendation recommend(const QString &loaderType, int serverSideJarCount, quint64 totalRamMiB,
                                     int providerRecommendationMiB)
{
    ServerMemoryRecommendation result;
    result.demandBeforeClampMiB = demandFor(loaderType, serverSideJarCount, providerRecommendationMiB);
    result.fromProviderRecommendation = sanitizeDemand(providerRecommendationMiB) > 0;
    result.maxMemoryMiB = clampMaxToHost(result.demandBeforeClampMiB, totalRamMiB);
    result.clampedToHost = result.maxMemoryMiB < result.demandBeforeClampMiB;
    // Minimum tracks half the maximum so stop-the-world pauses and heap
    // headroom scale with the chosen heap; always 256 MiB aligned and valid.
    result.minMemoryMiB = qMax(minimumMiB, roundDownToStep(result.maxMemoryMiB / 2));
    if (result.minMemoryMiB > result.maxMemoryMiB) {
        result.minMemoryMiB = result.maxMemoryMiB;
    }
    return result;
}

int countDeployedServerJars(const QString &serverDirectory, const QString &loaderType)
{
    QString contentFolder;
    if (isModdedLoader(loaderType)) {
        contentFolder = QStringLiteral("mods");
    } else if (isPluginLoader(loaderType)) {
        contentFolder = QStringLiteral("plugins");
    } else {
        return 0;
    }
    const QDir directory(QDir(serverDirectory).filePath(contentFolder));
    if (!directory.exists()) {
        return 0;
    }
    return directory.entryList(QStringList() << QStringLiteral("*.jar"), QDir::Files).size();
}

int resolveProviderRecommendation(bool overrideMemory, int maxMemAllocMiB, int exportRecommendedRamMiB)
{
    // Only a true per-instance memory override reflects the operator's or
    // pack author's intent for this install; the global default must not
    // count. Otherwise fall back to the exported recommendation some
    // providers publish with the pack.
    if (overrideMemory && maxMemAllocMiB > 0) {
        return maxMemAllocMiB;
    }
    if (exportRecommendedRamMiB > 0) {
        return exportRecommendedRamMiB;
    }
    return 0;
}

}  // namespace ServerMemory
