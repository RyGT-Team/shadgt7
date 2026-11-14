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
 * progress), set by the pipeline cache only while it learns the ceiling (not under
 * RYGT_COMPILE_ADAPTIVE=0 or RYGT_COMPILE_MAX). Runs on the perf monitor
 * thread, so the ceiling is lowered even while the command thread waits on the hung GPU.
 */
inline std::atomic<void (*)()> g_gpu_stall_hook{nullptr};

/**
 * PERF-GT7-160: called once on device loss (Instance::ReportDeviceLost), set by the pipeline
 * cache while its build workers learn the ceiling. Never throws.
 */
inline std::atomic<void (*)()> g_device_lost_hook{nullptr};

/// PERF-GT7-160: highest width known to be safe for one GPU + driver, kept across runs.
struct CeilingRecord {
    u32 ceiling{};
    u32 clean_runs{};
    bool operator==(const CeilingRecord&) const = default;
};

/// PERF-GT7-160: the ceiling file holds one `<key> <ceiling> <clean_runs>` line per GPU + driver.
namespace CompileCeiling {
/// PERF-GT7-160: seconds bound by builds that make a run count.
inline constexpr u32 MinBoundSeconds = 10;
/// Key as `<vendor hex>-<device hex>-<driver hex>`, without spaces.
std::string Key(u32 vendor, u32 device, u32 driver);
/// Never throws: a missing file or a corrupt line yields no record.
std::optional<CeilingRecord> Load(const std::filesystem::path& file, std::string_view key);
/// Rewrites the line of `key` and keeps the others. Best effort: failures are swallowed.
void Store(const std::filesystem::path& file, std::string_view key, CeilingRecord record);
/// PERF-GT7-160: ceiling of a GPU + driver with no record: 6 on AMD proprietary, else `widest`.
u32 FirstCeiling(bool amd_proprietary, u32 widest);
/// PERF-GT7-160: a stall at `peak_width` builds lowers the ceiling to half of it, not below
/// `floor`. A peak at or below `floor` keeps the record; the ceiling never rises here.
CeilingRecord AfterStall(CeilingRecord current, u32 peak_width, u32 floor);
/// PERF-GT7-160: a run with enough seconds bound by builds counts; the third counted run raises
/// the ceiling by one, up to `widest`.
CeilingRecord AfterCleanRun(CeilingRecord current, u32 bound_seconds, u32 widest);
} // namespace CompileCeiling

} // namespace Vulkan
