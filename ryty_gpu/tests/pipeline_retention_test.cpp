// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

// FIX-GT7-186 (from shadGT FIX-GTS-468): boundary cases of SystemRamUnderPressure. Not wired into
// CMake; build with: clang-cl /std:c++20 /EHsc /UNDEBUG /Isrc ryty_gpu/tests/pipeline_retention_test.cpp

#include <cassert>
#include <cstdio>
#include "video_core/renderer_vulkan/pipeline_retention_budget.h"

int main() {
    constexpr unsigned long long GiB = 1ull << 30;
    using Vulkan::SystemRamUnderPressure;
    unsigned cases = 0;
    const auto check = [&](bool expected, unsigned long long available, unsigned long long total) {
        assert(SystemRamUnderPressure(available, total) == expected);
        ++cases;
    };
    // The reserve is total/8, clamped to 1..2 GiB; an unknown total is never pressure.
    check(false, 0, 0);
    check(true, 2 * GiB - 1, 16 * GiB);
    check(false, 2 * GiB, 16 * GiB);
    check(true, 2 * GiB - 1, 64 * GiB);
    check(false, 2 * GiB, 64 * GiB);
    check(true, GiB - 1, 4 * GiB);
    check(false, GiB, 4 * GiB);
    check(true, GiB + GiB / 2 - 1, 12 * GiB);
    check(false, GiB + GiB / 2, 12 * GiB);
    static_assert(SystemRamUnderPressure(0, 16 * GiB));
    static_assert(!SystemRamUnderPressure(UINT64_MAX, UINT64_MAX));
    printf("PASS %u system RAM pressure boundary cases\n", cases + 2);
    return 0;
}
