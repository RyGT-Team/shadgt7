// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/compile_width.h"

namespace CompileCeiling = Vulkan::CompileCeiling;

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
    CompileCeiling::Store(file, key, 6);
    EXPECT_EQ(CompileCeiling::Load(file, key), 6u);
    CompileCeiling::Store(file, key, 4);
    EXPECT_EQ(CompileCeiling::Load(file, key), 4u);
}

TEST(CompileCeiling, OtherKeyIgnored) {
    TempDir dir;
    const auto file = dir.path / "ceiling.txt";
    const auto a = CompileCeiling::Key(0x1002, 0x7550, 1);
    const auto b = CompileCeiling::Key(0x1002, 0x7550, 2);
    CompileCeiling::Store(file, a, 5);
    EXPECT_FALSE(CompileCeiling::Load(file, b).has_value());
    CompileCeiling::Store(file, b, 9);
    EXPECT_EQ(CompileCeiling::Load(file, a), 5u);
    EXPECT_EQ(CompileCeiling::Load(file, b), 9u);
}

TEST(CompileCeiling, CorruptLineIgnored) {
    TempDir dir;
    std::filesystem::create_directories(dir.path);
    const auto file = dir.path / "ceiling.txt";
    {
        std::ofstream out(file);
        out << "abc x\nk1 99999999999999999999\nk2 4 1\n\nk3 5\n";
    }
    EXPECT_NO_THROW({
        EXPECT_FALSE(CompileCeiling::Load(file, "abc").has_value());
        EXPECT_FALSE(CompileCeiling::Load(file, "k1").has_value());
        EXPECT_FALSE(CompileCeiling::Load(file, "k2").has_value());
        EXPECT_EQ(CompileCeiling::Load(file, "k3"), 5u);
    });
}

TEST(CompileCeiling, FirstCeilingSixOnAmdProprietary) {
    EXPECT_EQ(CompileCeiling::FirstCeiling(true, 12), 6u);
    EXPECT_EQ(CompileCeiling::FirstCeiling(false, 12), 12u);
}
TEST(CompileCeiling, FirstCeilingClampedToWidest) {
    EXPECT_EQ(CompileCeiling::FirstCeiling(true, 4), 4u);
}
TEST(CompileCeiling, StallHalvesPeakWithFloor) {
    EXPECT_EQ(CompileCeiling::AfterStall(12, 12, 4), 6u);
    EXPECT_EQ(CompileCeiling::AfterStall(6, 6, 4), 4u);
}
TEST(CompileCeiling, StallAtOrBelowFloorKeepsRecord) {
    EXPECT_EQ(CompileCeiling::AfterStall(6, 4, 4), 6u);
    EXPECT_EQ(CompileCeiling::AfterStall(6, 2, 4), 6u);
}
TEST(CompileCeiling, StallNeverRaisesAndIsIdempotent) {
    EXPECT_EQ(CompileCeiling::AfterStall(5, 12, 4), 5u); // 12/2 = 6 > 5
    EXPECT_EQ(CompileCeiling::AfterStall(CompileCeiling::AfterStall(12, 12, 4), 12, 4), 6u);
}
