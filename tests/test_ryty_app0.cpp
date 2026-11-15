// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FIX-GT7-175: sce_sys files are found through the /app0 folder stack.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include "ryty_gpu/ryty_app0.h"

namespace {

std::filesystem::path MakeRoot() {
    const auto root = std::filesystem::temp_directory_path() / "ryty_app0_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "app0" / "sce_module");
    std::filesystem::create_directories(root / "update" / "sce_sys" / "trophy");
    std::filesystem::create_directories(root / "game" / "sce_sys" / "trophy");
    std::ofstream(root / "update" / "sce_sys" / "param.sfo") << "update";
    std::ofstream(root / "game" / "sce_sys" / "param.sfo") << "game";
    std::ofstream(root / "game" / "sce_sys" / "npbind.dat") << "game";
    std::ofstream(root / "game" / "sce_sys" / "trophy" / "trophy00.trp") << "game";
    std::ofstream(root / "update" / "sce_sys" / "trophy" / "TROPHY00.TRP") << "update";
    std::ofstream(root / "game" / "sce_sys" / "trophy" / "trophy01.trp") << "game";
    std::ofstream(root / "RyGT.ini") << "\xEF\xBB\xBFgame=" << (root / "game").string()
                                     << "\r\nupdate = " << (root / "update").string() << "\r\n";
    return root;
}

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string text;
    std::getline(file, text);
    return text;
}

} // namespace

TEST(RytyApp0, UpdateBeforeBaseGame) {
    SetEnv("RYGT_APP0_LAYERS", "");
    SetEnv("RYGT_SCE_SYS_LAYERS", "");
    const auto root = MakeRoot();
    EXPECT_EQ(Read(Ryty::App0File(root, "sce_sys/param.sfo")), "update");
    EXPECT_EQ(Read(Ryty::App0File(root, "sce_sys/npbind.dat")), "game");
    EXPECT_EQ(Ryty::App0File(root, "sce_sys/icon0.png"), root / "app0" / "sce_sys/icon0.png");
}

TEST(RytyApp0, TopLayerWins) {
    SetEnv("RYGT_APP0_LAYERS", "");
    SetEnv("RYGT_SCE_SYS_LAYERS", "");
    const auto root = MakeRoot();
    std::filesystem::create_directories(root / "app0" / "sce_sys");
    std::ofstream(root / "app0" / "sce_sys" / "param.sfo") << "app0";
    EXPECT_EQ(Read(Ryty::App0File(root, "sce_sys/param.sfo")), "app0");
}

TEST(RytyApp0, ListMergesCaseInsensitively) {
    SetEnv("RYGT_APP0_LAYERS", "");
    SetEnv("RYGT_SCE_SYS_LAYERS", "");
    const auto root = MakeRoot();
    const auto files = Ryty::App0ListFiles(root, "sce_sys/trophy");
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(Read(files[0]), "update"); // TROPHY00.TRP from the update hides the base game's
}

TEST(RytyApp0, SwitchRestoresApp0Only) {
    SetEnv("RYGT_APP0_LAYERS", "");
    SetEnv("RYGT_SCE_SYS_LAYERS", "0");
    const auto root = MakeRoot();
    EXPECT_EQ(Ryty::App0Layers(root).size(), 1u);
    EXPECT_TRUE(Ryty::App0ListFiles(root, "sce_sys/trophy").empty());
    SetEnv("RYGT_SCE_SYS_LAYERS", "");
}

TEST(RytyApp0, EnvironmentOverridesIni) {
    const auto root = MakeRoot();
    SetEnv("RYGT_APP0_LAYERS", (root / "game").string().c_str());
    EXPECT_EQ(Read(Ryty::App0File(root, "sce_sys/param.sfo")), "game");
    SetEnv("RYGT_APP0_LAYERS", "");
}
