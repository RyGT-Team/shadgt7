// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "video_core/renderer_vulkan/compile_width.h"

namespace Vulkan {

namespace CompileCeiling {

namespace {
struct Line {
    std::string key;
    u32 ceiling;
};

// PERF-GT7-160: parses `<key> <ceiling>`; anything else is a corrupt line.
std::optional<Line> ParseLine(const std::string& text) {
    std::istringstream in(text);
    std::string key;
    std::string ceiling;
    std::string extra;
    if (!(in >> key >> ceiling) || (in >> extra)) {
        return std::nullopt;
    }
    if (ceiling.size() > 9 || ceiling.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    return Line{key, static_cast<u32>(std::stoul(ceiling))};
}

std::vector<Line> ReadAll(const std::filesystem::path& file) {
    std::vector<Line> lines;
    std::ifstream in(file);
    std::string text;
    while (std::getline(in, text)) {
        if (auto line = ParseLine(text)) {
            lines.push_back(std::move(*line));
        }
    }
    return lines;
}
} // namespace

std::string Key(u32 vendor, u32 device, u32 driver) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%x-%x-%x", vendor, device, driver);
    return buffer;
}

std::optional<u32> Load(const std::filesystem::path& file, std::string_view key) {
    try {
        for (const auto& line : ReadAll(file)) {
            if (line.key == key) {
                return line.ceiling;
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

void Store(const std::filesystem::path& file, std::string_view key, u32 ceiling) {
    try {
        auto lines = ReadAll(file);
        std::erase_if(lines, [&](const Line& line) { return line.key == key; });
        lines.push_back({std::string(key), ceiling});
        if (file.has_parent_path()) {
            std::filesystem::create_directories(file.parent_path());
        }
        auto temp = file;
        temp += ".tmp";
        std::error_code ec;
        {
            std::ofstream out(temp, std::ios::trunc);
            for (const auto& line : lines) {
                out << line.key << ' ' << line.ceiling << '\n';
            }
            out.close();
            if (!out) {
                std::filesystem::remove(temp, ec); // PERF-GT7-160: no stray file.tmp
                return;
            }
        }
        std::filesystem::rename(temp, file, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
        }
    } catch (...) {
    }
}

u32 FirstCeiling(bool amd_proprietary, u32 widest) {
    return std::max(1u, amd_proprietary ? std::min(6u, widest) : widest);
}

u32 AfterStall(u32 ceiling, u32 peak_width, u32 floor) {
    if (peak_width <= floor) {
        return ceiling;
    }
    return std::min(ceiling, std::max(floor, peak_width / 2));
}

} // namespace CompileCeiling

} // namespace Vulkan
