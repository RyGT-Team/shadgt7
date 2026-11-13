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
constexpr double LoadMs = 80.0;     // PERF-GT7-160: a second this slow is a load or a wait.
constexpr double LagMs = 40.0;      // PERF-GT7-160: slower is lag (30 fps cap: 34-39 ms).
constexpr double SmoothMs = 36.0;   // PERF-GT7-160: faster is smooth (30 fps cap: 34-39 ms).
constexpr u32 SmoothToRaise = 2;    // PERF-GT7-160: smooth saturated seconds before one more build.
constexpr u32 StallSeconds = 10;    // PERF-GT7-160: seconds without step ups after a GPU stall.
constexpr u32 MinCleanBuilds = 200; // PERF-GT7-160: builds that make a run count as clean.
constexpr u32 CleanRunsToRaise = 3; // PERF-GT7-160: counted runs before the ceiling grows.
} // namespace

AdaptiveWidth::AdaptiveWidth(u32 start, u32 ceiling_) : limit{1}, ceiling{std::max(1u, ceiling_)} {
    limit = std::clamp(start, 1u, ceiling);
}

void AdaptiveWidth::SetCeiling(u32 new_ceiling) {
    ceiling = std::max(1u, new_ceiling);
    limit = std::min(limit, ceiling);
}

void AdaptiveWidth::OnSecond(double frame_ms, bool saturated) {
    const bool blocked = blocked_seconds > 0;
    if (blocked) {
        --blocked_seconds;
    }
    // PERF-GT7-160: a load second says nothing about the builds; it only breaks the streak.
    if (frame_ms >= LoadMs) {
        smooth_seconds = 0;
        return;
    }
    // PERF-GT7-160: only a saturated second (as many builds pending as the width) moves the
    // limit: otherwise the width is not in use and changing it has no effect. Load frames of
    // 55-62 ms with nothing pending stepped it down in the first in-game run.
    // PERF-GT7-160: lag halves the limit: back from a pause screen (cheap frames, up to 10 at
    // a time) one step a second left a race at 20-25 fps for seconds.
    if (frame_ms > LagMs) {
        if (saturated) {
            limit = std::max(1u, limit / 2);
        }
        smooth_seconds = 0;
        return;
    }
    // PERF-GT7-160: borderline, unsaturated or blocked seconds do not count toward a step up.
    if (!(frame_ms < SmoothMs) || !saturated || blocked) {
        smooth_seconds = 0;
        return;
    }
    if (++smooth_seconds >= SmoothToRaise) {
        limit = std::min(limit + 1, ceiling);
        smooth_seconds = 0;
    }
}

void AdaptiveWidth::OnGpuStall() {
    limit = std::max(1u, limit / 2);
    smooth_seconds = 0;
    blocked_seconds = StallSeconds;
}

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

CeilingRecord AfterDeviceLost(u32 peak_width) {
    return {std::max(1u, peak_width / 2), 0};
}

CeilingRecord AfterCleanRun(CeilingRecord current, u32 builds, u32 widest) {
    if (builds < MinCleanBuilds) {
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
