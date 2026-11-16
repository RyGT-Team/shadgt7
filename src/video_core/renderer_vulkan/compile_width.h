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
 * PERF-GT7-160: called by the perf monitor once per GPU stall (busy, no GPU tick progress for
 * StallSeconds), set by the pipeline cache only while it learns the ceiling (not under
 * RYGT_COMPILE_ADAPTIVE=0 or RYGT_COMPILE_MAX). Runs on the perf monitor
 * thread, so the ceiling is lowered even while the command thread waits on the hung GPU.
 */
inline std::atomic<void (*)()> g_gpu_stall_hook{nullptr};

/// PERF-GT7-160: as DIAG-103, longer than the game's own pauses (2.5-2.8 s at start-up).
inline constexpr u32 StallSeconds = 10;

/**
 * PERF-GT7-160: called once on device loss (Instance::ReportDeviceLost), set by the pipeline
 * cache while its build workers learn the ceiling. Never throws.
 */
inline std::atomic<void (*)()> g_device_lost_hook{nullptr};

/// PERF-GT7-160: the ceiling file holds one `<key> <ceiling>` line per GPU + driver: the highest
/// width known to be safe for it, kept across runs.
namespace CompileCeiling {
/// Key as `<vendor hex>-<device hex>-<driver hex>`, without spaces.
std::string Key(u32 vendor, u32 device, u32 driver);
/// Never throws: a missing file or a corrupt line yields no ceiling.
std::optional<u32> Load(const std::filesystem::path& file, std::string_view key);
/// Rewrites the line of `key` and keeps the others. Best effort: failures are swallowed.
void Store(const std::filesystem::path& file, std::string_view key, u32 ceiling);
/// PERF-GT7-160: ceiling of a GPU + driver with no record: 6 on AMD proprietary, else `widest`.
u32 FirstCeiling(bool amd_proprietary, u32 widest);
/// PERF-GT7-160: a stall at `peak_width` builds lowers the ceiling to half of it, not below
/// `floor`. A peak at or below `floor` keeps the ceiling; it never rises.
u32 AfterStall(u32 ceiling, u32 peak_width, u32 floor);
} // namespace CompileCeiling

} // namespace Vulkan
