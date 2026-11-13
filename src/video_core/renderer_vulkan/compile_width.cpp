// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "video_core/renderer_vulkan/compile_width.h"

namespace Vulkan {

namespace {
constexpr u32 MinBoundSeconds = 10; // PERF-GT7-160: seconds bound by builds that make a run count.
constexpr u32 CleanRunsToRaise = 3; // PERF-GT7-160: counted runs before the ceiling grows.
} // namespace

namespace CompileCeiling {

namespace {
struct Line {
    std::string key;
    CeilingRecord record;
};

// PERF-GT7-160: parses `<key> <ceiling> <clean_runs>`; anything else is a corrupt line.
std::optional<Line> ParseLine(const std::string& text) {
    std::istringstream in(text);
    std::string key;
    std::string ceiling;
    std::string clean;
    std::string extra;
    if (!(in >> key >> ceiling >> clean) || (in >> extra)) {
        return std::nullopt;
    }
    const auto to_u32 = [](const std::string& s) -> std::optional<u32> {
        if (s.empty() || s.size() > 9 || s.find_first_not_of("0123456789") != std::string::npos) {
            return std::nullopt;
        }
        return static_cast<u32>(std::stoul(s));
    };
    const auto c = to_u32(ceiling);
    const auto r = to_u32(clean);
    if (!c || !r) {
        return std::nullopt;
    }
    return Line{key, {*c, *r}};
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

std::optional<CeilingRecord> Load(const std::filesystem::path& file, std::string_view key) {
    try {
        for (const auto& line : ReadAll(file)) {
            if (line.key == key) {
                return line.record;
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

void Store(const std::filesystem::path& file, std::string_view key, CeilingRecord record) {
    try {
        auto lines = ReadAll(file);
        std::erase_if(lines, [&](const Line& line) { return line.key == key; });
        lines.push_back({std::string(key), record});
        if (file.has_parent_path()) {
            std::filesystem::create_directories(file.parent_path());
        }
        auto temp = file;
        temp += ".tmp";
        std::error_code ec;
        {
            std::ofstream out(temp, std::ios::trunc);
            for (const auto& line : lines) {
                out << line.key << ' ' << line.record.ceiling << ' ' << line.record.clean_runs
                    << '\n';
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

CeilingRecord AfterStall(CeilingRecord current, u32 peak_width, u32 floor) {
    if (peak_width <= floor) {
        return current;
    }
    return {std::min(current.ceiling, std::max(floor, peak_width / 2)), 0};
}

CeilingRecord AfterCleanRun(CeilingRecord current, u32 bound_seconds, u32 widest) {
    if (bound_seconds < MinBoundSeconds) {
        return current;
    }
    if (++current.clean_runs < CleanRunsToRaise) {
        return current;
    }
    current.ceiling = std::min(current.ceiling + 1, std::max(widest, 1u));
    current.clean_runs = 0;
    return current;
}

} // namespace CompileCeiling

} // namespace Vulkan
