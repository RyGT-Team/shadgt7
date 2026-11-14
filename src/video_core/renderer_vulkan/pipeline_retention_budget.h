// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>

namespace Vulkan {

// FIX-GT7-186 (from shadGT FIX-GTS-468): driver pipeline memory is mostly system RAM. Free
// physical memory below an eighth of it, between 1 and 2 GB (2 GB at 16 GB), counts as pressure.
// Unknown totals (0) never do.
constexpr bool SystemRamUnderPressure(std::uint64_t available, std::uint64_t total) {
    constexpr std::uint64_t GiB = 1ull << 30;
    if (total == 0) {
        return false;
    }
    const std::uint64_t reserve = std::clamp<std::uint64_t>(total / 8, GiB, 2 * GiB);
    return available < reserve;
}

} // namespace Vulkan
