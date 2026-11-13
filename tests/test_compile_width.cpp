// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/compile_width.h"

using Vulkan::AdaptiveWidth;
using Vulkan::CeilingRecord;
namespace CompileCeiling = Vulkan::CompileCeiling;

namespace {
constexpr double Smooth = 34.0; // Below 36 ms: a 30 fps cap with room to spare.
constexpr double Lag = 50.0;    // Above 40 ms.
} // namespace

TEST(CompileWidth, StartsAtStartClampedToCeiling) {
    EXPECT_EQ(AdaptiveWidth(6, 2).Limit(), 2u);
    EXPECT_EQ(AdaptiveWidth(6, 8).Limit(), 6u);
}

TEST(CompileWidth, LagStepsDown) {
    AdaptiveWidth width(6, 8);
    width.OnSecond(Lag, true);
    EXPECT_EQ(width.Limit(), 3u);
    width.OnSecond(Lag, true);
    EXPECT_EQ(width.Limit(), 1u);
}

TEST(CompileWidth, LagHalvesFromTen) {
    AdaptiveWidth width(10, 12);
    width.OnSecond(Lag, true);
    EXPECT_EQ(width.Limit(), 5u);
    width.OnSecond(Lag, true);
    EXPECT_EQ(width.Limit(), 2u);
    width.OnSecond(Lag, true);
    EXPECT_EQ(width.Limit(), 1u);
    width.OnSecond(Lag, true);
    EXPECT_EQ(width.Limit(), 1u);
}

TEST(CompileWidth, LagWhileIdleKeepsLimit) {
    AdaptiveWidth width(6, 8);
    for (int i = 0; i < 5; ++i) {
        width.OnSecond(Lag, false);
    }
    EXPECT_EQ(width.Limit(), 6u);
    // An unsaturated lagging second breaks the smooth streak.
    width.OnSecond(Smooth, true);
    width.OnSecond(Lag, false);
    width.OnSecond(Smooth, true);
    EXPECT_EQ(width.Limit(), 6u);
}

TEST(CompileWidth, TwoSmoothSecondsStepUp) {
    AdaptiveWidth width(6, 8);
    width.OnSecond(Smooth, true);
    EXPECT_EQ(width.Limit(), 6u);
    width.OnSecond(Smooth, true);
    EXPECT_EQ(width.Limit(), 7u);
    // The streak restarts after a step, and a borderline second breaks it.
    width.OnSecond(Smooth, true);
    width.OnSecond(40.0, true);
    width.OnSecond(Smooth, true);
    EXPECT_EQ(width.Limit(), 7u);
    width.OnSecond(Smooth, true);
    EXPECT_EQ(width.Limit(), 8u);
}

TEST(CompileWidth, NotBusyNeverStepsUp) {
    AdaptiveWidth width(6, 8);
    for (int i = 0; i < 10; ++i) {
        width.OnSecond(Smooth, false);
    }
    EXPECT_EQ(width.Limit(), 6u);
    // An unsaturated second breaks the streak too.
    width.OnSecond(Smooth, true);
    width.OnSecond(Smooth, false);
    width.OnSecond(Smooth, true);
    EXPECT_EQ(width.Limit(), 6u);
}

TEST(CompileWidth, ThresholdEdges) {
    AdaptiveWidth width(6, 8);
    width.OnSecond(39.9, true);
    EXPECT_EQ(width.Limit(), 6u);
    width.OnSecond(40.1, true);
    EXPECT_EQ(width.Limit(), 3u);
    for (int i = 0; i < 4; ++i) {
        width.OnSecond(36.0, true);
    }
    EXPECT_EQ(width.Limit(), 3u);
    width.OnSecond(35.9, true);
    width.OnSecond(35.9, true);
    EXPECT_EQ(width.Limit(), 4u);
}

TEST(CompileWidth, LoadSecondIgnoredAndResetsStreak) {
    AdaptiveWidth width(6, 8);
    width.OnSecond(80.0, true);
    width.OnSecond(500.0, true);
    EXPECT_EQ(width.Limit(), 6u);
    width.OnSecond(Smooth, true);
    width.OnSecond(80.0, true);
    width.OnSecond(Smooth, true);
    EXPECT_EQ(width.Limit(), 6u);
    width.OnSecond(Smooth, true);
    EXPECT_EQ(width.Limit(), 7u);
}

TEST(CompileWidth, StallHalvesAndBlocks10Seconds) {
    AdaptiveWidth width(7, 8);
    width.OnGpuStall();
    EXPECT_EQ(width.Limit(), 3u);
    for (int i = 0; i < 10; ++i) {
        width.OnSecond(Smooth, true);
    }
    EXPECT_EQ(width.Limit(), 3u);
    width.OnSecond(Smooth, true);
    width.OnSecond(Smooth, true);
    EXPECT_EQ(width.Limit(), 4u);
    // Lag still steps down during the block.
    width.OnGpuStall();
    ASSERT_EQ(width.Limit(), 2u);
    width.OnSecond(Lag, true);
    EXPECT_EQ(width.Limit(), 1u);
}

TEST(CompileWidth, BoundsOneAndCeiling) {
    AdaptiveWidth width(2, 8);
    for (int i = 0; i < 5; ++i) {
        width.OnSecond(Lag, true);
        width.OnGpuStall();
    }
    EXPECT_EQ(width.Limit(), 1u);
    AdaptiveWidth wide(6, 8);
    for (int i = 0; i < 20; ++i) {
        wide.OnSecond(Smooth, true);
    }
    EXPECT_EQ(wide.Limit(), 8u);
    wide.SetCeiling(3);
    EXPECT_EQ(wide.Limit(), 3u);
    wide.OnSecond(Smooth, true);
    wide.OnSecond(Smooth, true);
    EXPECT_EQ(wide.Limit(), 3u);
}

TEST(CompileWidth, RaisedCeilingLetsLimitClimb) {
    AdaptiveWidth width(6, 8);
    width.SetCeiling(3);
    width.OnSecond(Smooth, true);
    width.OnSecond(Smooth, true);
    ASSERT_EQ(width.Limit(), 3u);
    width.SetCeiling(8);
    EXPECT_EQ(width.Limit(), 3u);
    width.OnSecond(Smooth, true);
    width.OnSecond(Smooth, true);
    EXPECT_EQ(width.Limit(), 4u);
}

namespace {
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        // A random tag keeps test processes run in parallel apart.
        static const unsigned tag = std::random_device{}();
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("shadps4_ceiling_test_" + std::to_string(tag) + "_" + std::to_string(++counter));
        std::filesystem::remove_all(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

TEST(CompileCeiling, LoadMissingFileIsEmpty) {
    TempDir dir;
    EXPECT_FALSE(CompileCeiling::Load(dir.path / "none.txt", "1002-7550-1").has_value());
}

TEST(CompileCeiling, StoreThenLoadRoundTrips) {
    TempDir dir;
    const auto file = dir.path / "sub" / "ceiling.txt";
    const auto key = CompileCeiling::Key(0x1002, 0x7550, 0xABCD);
    EXPECT_EQ(key, "1002-7550-abcd");
    CompileCeiling::Store(file, key, {6, 2});
    EXPECT_EQ(CompileCeiling::Load(file, key), (CeilingRecord{6, 2}));
    CompileCeiling::Store(file, key, {7, 0});
    EXPECT_EQ(CompileCeiling::Load(file, key), (CeilingRecord{7, 0}));
}

TEST(CompileCeiling, OtherKeyIgnored) {
    TempDir dir;
    const auto file = dir.path / "ceiling.txt";
    const auto a = CompileCeiling::Key(0x1002, 0x7550, 1);
    const auto b = CompileCeiling::Key(0x1002, 0x7550, 2);
    CompileCeiling::Store(file, a, {5, 1});
    EXPECT_FALSE(CompileCeiling::Load(file, b).has_value());
    CompileCeiling::Store(file, b, {9, 0});
    EXPECT_EQ(CompileCeiling::Load(file, a), (CeilingRecord{5, 1}));
    EXPECT_EQ(CompileCeiling::Load(file, b), (CeilingRecord{9, 0}));
}

TEST(CompileCeiling, CorruptLineIgnored) {
    TempDir dir;
    std::filesystem::create_directories(dir.path);
    const auto file = dir.path / "ceiling.txt";
    {
        std::ofstream out(file);
        out << "abc x y\nk1 99999999999999999999 1\nk2 4\n\nk3 5 1\n";
    }
    EXPECT_NO_THROW({
        EXPECT_FALSE(CompileCeiling::Load(file, "abc").has_value());
        EXPECT_FALSE(CompileCeiling::Load(file, "k1").has_value());
        EXPECT_FALSE(CompileCeiling::Load(file, "k2").has_value());
        EXPECT_EQ(CompileCeiling::Load(file, "k3"), (CeilingRecord{5, 1}));
    });
}

TEST(CompileCeiling, DeviceLostHalves) {
    EXPECT_EQ(CompileCeiling::AfterDeviceLost(6), (CeilingRecord{3, 0}));
    EXPECT_EQ(CompileCeiling::AfterDeviceLost(1), (CeilingRecord{1, 0}));
}

TEST(CompileCeiling, CleanRunsRaiseAfterThree) {
    CeilingRecord r{3, 0};
    r = CompileCeiling::AfterCleanRun(r, 200, 12);
    r = CompileCeiling::AfterCleanRun(r, 200, 12);
    EXPECT_EQ(r, (CeilingRecord{3, 2}));
    r = CompileCeiling::AfterCleanRun(r, 199, 12);
    EXPECT_EQ(r, (CeilingRecord{3, 2}));
    r = CompileCeiling::AfterCleanRun(r, 200, 12);
    EXPECT_EQ(r, (CeilingRecord{4, 0}));
    CeilingRecord top{12, 2};
    EXPECT_EQ(CompileCeiling::AfterCleanRun(top, 500, 12), (CeilingRecord{12, 0}));
}
