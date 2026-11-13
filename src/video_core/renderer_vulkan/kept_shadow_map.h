// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Vulkan {

// FIX-GT7-153: the shape of GT7's whole-circuit shadow map, one per track: a square 16-bit depth
// picture of 2048 to 9216 texels, one layer, one sample (9216x9216 at Le Mans). The real-time
// cascades (1024x1024 32-bit, four layers) and the cars' shadows (1024x1024) do not match. Square
// 16-bit depth pictures of 256 to 1792 seen at the same addresses look like placeholders: the 2048
// cut-off is a heuristic, read from one PC's learned pictures.
constexpr std::uint32_t KeptShadowMapMinSize = 2048;

constexpr bool IsKeptShadowMapShape(bool depth_z16, std::uint32_t pitch, std::uint32_t height,
                                    std::uint32_t layers, std::uint32_t samples) {
    return depth_z16 && layers == 1 && samples == 1 && height >= KeptShadowMapMinSize &&
           pitch == height;
}

} // namespace Vulkan
