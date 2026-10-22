// Optional read-only SDDS exports. See ../../LICENSE.
#pragma once
#include "core/analytics.h"

namespace alh {
bool sddsAvailable();
// Throws std::runtime_error on failure; existing destinations are preserved.
void saveAlarmSnapshotSdds(const QString& path, const Engine&);
// Rows retain supplied order. Standing tables omit nonstanding channels.
// For a chatter chart, pass only the selected channel's report row.
void saveAnalyticsSdds(const QString& path, const AnalyticsReport&, int tab, bool chart = false);
} // namespace alh
