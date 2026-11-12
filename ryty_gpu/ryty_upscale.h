// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cmath>
#include <iterator>
#include <cstdlib>
#include <cstring>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

// ryGT: the size DLSS and FSR 3.1 scale the game picture to (RYGT_UPSCALE_QUALITY, set by the
// launcher's Upscaling quality list). The game always draws at its own size (1920x1080 for GT7),
// so a quality mode cannot lower the render resolution: it picks the output instead, the picture
// times the mode's scale (Native 1x, Quality 1.5x, Balanced 1.72x, Performance 2x, Ultra
// Performance 3x), and the post-process pass then fits that output to the window. "auto" (the
// default) scales to the window size, as before.
namespace Vulkan::RytyUpscale {

enum class Quality { Auto, Native, Quality, Balanced, Performance, UltraPerformance };

inline Quality GetQuality() {
    static const Quality quality = [] {
        const char* env = std::getenv("RYGT_UPSCALE_QUALITY");
        if (!env) {
            return Quality::Auto;
        }
        if (std::strcmp(env, "native") == 0) {
            return Quality::Native;
        }
        if (std::strcmp(env, "quality") == 0) {
            return Quality::Quality;
        }
        if (std::strcmp(env, "balanced") == 0) {
            return Quality::Balanced;
        }
        if (std::strcmp(env, "performance") == 0) {
            return Quality::Performance;
        }
        if (std::strcmp(env, "ultra_performance") == 0) {
            return Quality::UltraPerformance;
        }
        return Quality::Auto;
    }();
    return quality;
}

inline double Scale(Quality quality) {
    switch (quality) {
    case Quality::Native:
        return 1.0;
    case Quality::Quality:
        return 1.5;
    case Quality::Balanced:
        return 1.0 / 0.58;
    case Quality::Performance:
        return 2.0;
    case Quality::UltraPerformance:
        return 3.0;
    default:
        return 0.0;
    }
}

/// The upscaler's output size for the game picture input shown in window.
inline vk::Extent2D Target(vk::Extent2D input, vk::Extent2D window) {
    const double scale = Scale(GetQuality());
    if (scale == 0.0) {
        return window;
    }
    return {static_cast<u32>(std::lround(input.width * scale)),
            static_cast<u32>(std::lround(input.height * scale))};
}

/// Whether the upscaler should run at the picture's own size (DLAA / FSR native AA).
inline bool NativeAA() {
    return GetQuality() == Quality::Native;
}

// DLSS-GT7-001 (prototype, RYGT_DLSS_TEMPORAL=1, off by default): DLSS fed with the game's own
// depth, motion vectors and sub-pixel jitter instead of a flat depth, zero motion and a history
// reset every frame. GT7's race draws a jittered 1920x1080 scene (49 positions of +-0.25 pixel)
// with a motion-vector target, and resolves it with its own temporal anti-aliasing pass (pixel
// shader 0xcfa125dd); the native renderer notes that pass's inputs and replaces its result with
// the frame's un-resolved colour, so DLSS, at the present, gets jittered frames it can
// accumulate. Elsewhere (menus, a frame without that pass) DLSS runs as before. Measured and
// written up in RECHERCHE-DLSS-TEMPOREL-GT7.md; seen on an RTX 4070 only.
//   RYGT_DLSS_TEMPORAL_KEY=0     no F9 and F10 keys (F9 switches the prototype off and on while
//                                playing; F10 steps through the jitter's signs, see
//                                TemporalJitterChoices)
//   RYGT_DLSS_TEMPORAL_TAA=1     the game's anti-aliasing result is kept (DLSS still gets the data)
//   RYGT_DLSS_TEMPORAL_BLOOM=0   the result is replaced at once, so the game's bloom and exposure
//                                read the un-resolved colour too (the first prototype)
//   RYGT_DLSS_TEMPORAL_HUD_SCALE=1  the HUD is drawn at the game's size, not twice it
//   RYGT_DLSS_TEMPORAL_JITTER=x,y  factors applied to the jitter (default 1,1; -1 flips an axis)
//   RYGT_DLSS_TEMPORAL_MVEC=x,y    factors applied to the motion vectors (default 1,1)

/// The frame's inputs for a temporal upscale, all at the game picture's size.
struct TemporalInputs {
    vk::Image depth;
    vk::ImageView depth_view; ///< depth aspect; larger = nearer (GT7 draws reversed depth)
    vk::Format depth_format;
    vk::Image motion;
    vk::ImageView motion_view;
    vk::Format motion_format;
    vk::Extent2D size;
    float jitter[2];     ///< this frame's jitter in pixels: the scene's shift, x right, y down
    float mvec_scale[2]; ///< motion * scale = (previous - current position) / picture size
    bool reset;          ///< the frame before had no such pass (a camera cut): history starts over
};

/// RYGT_DLSS_TEMPORAL=1 was set for this run.
inline bool TemporalRequested() {
    static const bool on = [] {
        const char* env = std::getenv("RYGT_DLSS_TEMPORAL");
        return env && env[0] == '1';
    }();
    return on;
}

inline std::atomic<bool>& TemporalSwitch() {
    static std::atomic<bool> on{true};
    return on;
}

/// What the last DLSS pass ran with, for the FPS counter: 0 a single frame (as without the
/// prototype), 1 the game's depth, motion vectors and jitter.
inline std::atomic<int>& TemporalStatus() {
    static std::atomic<int> status{0};
    return status;
}

/// The prototype is requested and not switched off with its key.
inline bool TemporalActive() {
    return TemporalRequested() && TemporalSwitch().load(std::memory_order_relaxed);
}

/// The game's anti-aliasing result is replaced with the un-resolved colour.
inline bool TemporalReplacesTaa() {
    static const bool keep = [] {
        const char* env = std::getenv("RYGT_DLSS_TEMPORAL_TAA");
        return env && env[0] == '1';
    }();
    return !keep;
}

/// The jitter's sign is not settled: the pass's shader reads at position - (c14*c20, c15*c21),
/// which says the scene is shifted by the opposite, and GT Sport's DLSS pass (shadGT,
/// ryty_gts_dlss.cpp) hands DLSS +(c20*c14, c21*c15) for the same pass. The frame dumps of
/// 2026-11-04 and 11-11 do not tell them apart (measured shifts within the noise). F10 steps
/// through the choices while playing; the right one gives a steady, sharp picture with the car
/// at a standstill. Factors of the jitter NoteTemporalDraw notes, -(c14*c20, c15*c21).
struct TemporalJitterChoice {
    float x;
    float y;
    const char* name; ///< for the FPS counter and the log
};
inline constexpr TemporalJitterChoice TemporalJitterChoices[] = {
    {1.0f, 1.0f, "A"},   // -(c14*c20, c15*c21): the prototype's first reading
    {-1.0f, -1.0f, "B"}, // +(c14*c20, c15*c21): GT Sport's
    {1.0f, -1.0f, "C"},
    {-1.0f, 1.0f, "D"},
    {0.0f, 0.0f, "0"},   // none given: DLSS takes every frame as centred
};
inline std::atomic<u32>& TemporalJitterIndex() {
    static std::atomic<u32> index{0};
    return index;
}
inline const TemporalJitterChoice& TemporalJitterNow() {
    return TemporalJitterChoices[TemporalJitterIndex().load(std::memory_order_relaxed) %
                                 std::size(TemporalJitterChoices)];
}

/// The two factors of RYGT_DLSS_TEMPORAL_JITTER or _MVEC (1,1 when unset or unreadable).
inline void TemporalFactors(const char* name, float out[2]) {
    out[0] = out[1] = 1.0f;
    if (const char* env = std::getenv(name); env && *env) {
        char* end = nullptr;
        const float x = std::strtof(env, &end);
        if (end != env && *end == ',') {
            const char* second = end + 1;
            const float y = std::strtof(second, &end);
            if (end != second && std::isfinite(x) && std::isfinite(y)) {
                out[0] = x;
                out[1] = y;
            }
        }
    }
}

} // namespace Vulkan::RytyUpscale
