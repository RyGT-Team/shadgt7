// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "common/types.h"

namespace Vulkan {

/**
 * PERF-GT7-160: called by the perf monitor once per second of GPU stall (busy, no GPU tick
 * progress), set by the pipeline cache while its build workers exist. Runs on the perf monitor
 * thread, so the width backs off even while the command thread waits on the hung GPU.
 */
inline std::atomic<void (*)()> g_gpu_stall_hook{nullptr};

/**
 * PERF-GT7-160: called once on device loss (Instance::ReportDeviceLost), set by the pipeline
 * cache while its build workers learn the ceiling. Never throws.
 */
inline std::atomic<void (*)()> g_device_lost_hook{nullptr};

/**
 * PERF-GT7-160: how many pipeline builds the driver runs at once, tuned at run time from the
 * game's frame time. Each second reports the average frame time and whether it was saturated
 * (as many builds pending as the width): a lagging saturated second halves the limit, two
 * smooth saturated seconds in a row step it up, and a load second (80 ms or more) is ignored. A
 * GPU stall halves it and blocks step ups for 10 seconds. No locking: callers hold their mutex.
 */
class AdaptiveWidth {
public:
    AdaptiveWidth(u32 start, u32 ceiling);

    u32 Limit() const {
        return limit;
    }

    void SetCeiling(u32 new_ceiling);
    void OnSecond(double frame_ms, bool saturated);
    void OnGpuStall();

private:
    u32 limit;
    u32 ceiling;
    u32 smooth_seconds{};  ///< Consecutive smooth, saturated seconds since the last change.
    u32 blocked_seconds{}; ///< Seconds left without step ups after a GPU stall.
};

/// PERF-GT7-160: highest width known to be safe for one GPU + driver, kept across runs.
struct CeilingRecord {
    u32 ceiling{};
    u32 clean_runs{};
    bool operator==(const CeilingRecord&) const = default;
};

/// PERF-GT7-160: the ceiling file holds one `<key> <ceiling> <clean_runs>` line per GPU + driver.
namespace CompileCeiling {
/// Key as `<vendor hex>-<device hex>-<driver hex>`, without spaces.
std::string Key(u32 vendor, u32 device, u32 driver);
/// Never throws: a missing file or a corrupt line yields no record.
std::optional<CeilingRecord> Load(const std::filesystem::path& file, std::string_view key);
/// Rewrites the line of `key` and keeps the others. Best effort: failures are swallowed.
void Store(const std::filesystem::path& file, std::string_view key, CeilingRecord record);
/// A device loss halves the peak width in use over the last seconds.
CeilingRecord AfterDeviceLost(u32 peak_width);
/// A run with enough builds counts; the third counted run raises the ceiling by one.
CeilingRecord AfterCleanRun(CeilingRecord current, u32 builds, u32 widest);
} // namespace CompileCeiling

} // namespace Vulkan
