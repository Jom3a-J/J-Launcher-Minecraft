// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace Flame {

inline constexpr int MaximumAutomaticSearchPages = 5;

inline bool isVisibleWithServerReadyFilter(bool hasServerPack, bool serverReadyOnly)
{
    return !serverReadyOnly || hasServerPack;
}

inline bool shouldAutomaticallyFetchServerReadyPage(bool serverReadyOnly,
                                                     int visibleServerReadyRows,
                                                     int minimumVisibleRows,
                                                     int automaticPagesFetched,
                                                     bool hasMoreResults)
{
    return serverReadyOnly && hasMoreResults && visibleServerReadyRows < minimumVisibleRows
        && automaticPagesFetched < MaximumAutomaticSearchPages;
}

}  // namespace Flame
