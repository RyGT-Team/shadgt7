// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/kept_shadow_map.h"

using Vulkan::IsKeptShadowMapShape;

TEST(KeptShadowMap, WholeCircuitMapsMatch) {
    EXPECT_TRUE(IsKeptShadowMapShape(true, 9216, 9216, 1, 1)); // Le Mans
    EXPECT_TRUE(IsKeptShadowMapShape(true, 6400, 6400, 1, 1)); // Nurburgring
    EXPECT_TRUE(IsKeptShadowMapShape(true, 2048, 2048, 1, 1)); // the smallest learned
}

TEST(KeptShadowMap, OtherDepthPicturesDoNotMatch) {
    EXPECT_FALSE(IsKeptShadowMapShape(true, 1024, 1024, 1, 1));  // the cars' shadows
    EXPECT_FALSE(IsKeptShadowMapShape(false, 1024, 1024, 4, 1)); // the cascades (32-bit, 4 layers)
    EXPECT_FALSE(IsKeptShadowMapShape(false, 9216, 9216, 1, 1)); // 32-bit depth
    EXPECT_FALSE(IsKeptShadowMapShape(true, 1792, 1792, 1, 1));  // placeholders at the map slots
    EXPECT_FALSE(IsKeptShadowMapShape(true, 2048, 1024, 1, 1));  // not square
    EXPECT_FALSE(IsKeptShadowMapShape(true, 4096, 4096, 2, 1));  // an array
    EXPECT_FALSE(IsKeptShadowMapShape(true, 4096, 4096, 1, 2));  // multisampled
}
