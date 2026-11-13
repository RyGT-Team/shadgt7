// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>

#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/kernel/process.h"
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
    const vk::Extent2D output{static_cast<u32>(std::lround(input.width * scale)),
                              static_cast<u32>(std::lround(input.height * scale))};
    // NEO-GT7-001: the PS4 Pro picture (3200x1800 or 3840x2160) is already near the window, and a
    // quality mode made for the 1080p picture would ask for up to 11520x6480. Past the window the
    // output is the window, or the picture itself when it is not smaller (native anti-aliasing).
    if (Libraries::Kernel::sceKernelIsNeoMode() &&
        (output.width > window.width || output.height > window.height)) {
        return input.width >= window.width || input.height >= window.height ? input : window;
    }
    return output;
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
// accumulate. In a race the HUD is drawn apart (twice the size) and laid over DLSS's picture; in
// menus with a 3D scene the interface stays in the picture, shifted by the jitter (NoteTemporalDraw).
// A frame without that pass is scaled as before. Measured and written up in
// RECHERCHE-DLSS-TEMPOREL-GT7.md; seen on an RTX 4070 only.
// Nothing is changed in the game's pictures unless Streamline's DLSS scales them and takes the
// inputs (TemporalUsable): with FSR, the plain scaling, the NGX plugin or a refusal the game
// draws as without the prototype. Off in the PS4 Pro mode on a 4K TV (TemporalRequested).
//   RYGT_DLSS_TEMPORAL_KEY=0     no keys (Page Up switches the prototype off and on while
//                                playing; Page Down steps through the ways the HUD is drawn,
//                                Home shifts the menus' interface by the jitter or not, End
//                                switches the menus' interface motion mask). Only read while
//                                the game's window is in front. Not function keys: the emulator's
//                                own hotkeys take them all (F9 pauses the game, F10 hides the FPS
//                                counter), which the first builds' F9 and F10 ran into.
//   RYGT_DLSS_TEMPORAL_MENUS=1   also outside a race (menus with a 3D scene: their pages come
//                                out wrong, see NoteTemporalDraw)
//   RYGT_DLSS_TEMPORAL_BLOOM=0   the result is replaced at once, so the game's bloom and exposure
//                                read the un-resolved colour too (the first prototype)
//   RYGT_DLSS_TEMPORAL_BLOOM=1   the result is replaced before the first draw into it, so a bloom
//                                that comes after the game's motion blur reads the un-resolved
//                                colour (until 2026-11-13; lights shimmered in pre-races). By
//                                default it stays resolved until the tone mapping (DLSS-GT7-003)
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
    /// A menu frame whose interface went through DLSS: the display as it was before its interface
    /// (same view as the display's), so the motion pass finds where the interface drew. Else null.
    vk::ImageView ui_scene_view;
};

/// RYGT_DLSS_TEMPORAL=1 was set for this run, and the game does not run in the PS4 Pro mode on a
/// 4K TV. There GT7 draws 3200x1800 in a checkerboard and rebuilds the full picture with its own
/// temporal compute pass (cs 0x7aa5f0ad): no 0xcfa125dd pass to replace, and DLSS's history on
/// top of the game's would accumulate twice (trails doubled where nothing moves, as rain). DLSS
/// scales that picture as a single frame, as without the prototype. The Pro mode on a 1080p TV
/// (RYGT_NEO_TV=1080) draws 1920x1080 without the checkerboard and keeps the prototype. First
/// called while drawing, after the game's parameters are read (sceKernelIsNeoMode keeps its
/// first answer).
inline bool TemporalRequested() {
    static const bool on = [] {
        const char* env = std::getenv("RYGT_DLSS_TEMPORAL");
        if (!(env && env[0] == '1')) {
            return false;
        }
        const char* tv = std::getenv("RYGT_NEO_TV");
        if (Libraries::Kernel::sceKernelIsNeoMode() && !(tv && std::string_view{tv} == "1080")) {
            LOG_WARNING(Render_Vulkan, "DLSS-GT7-001: off in the PS4 Pro mode on a 4K TV (the "
                                       "game rebuilds its checkerboard itself); classic DLSS");
            return false;
        }
        return true;
    }();
    return on;
}

/// Set by the presenter's DLSS pass: Streamline's DLSS scaled the last frame and has not refused
/// the temporal inputs. False until a first frame is scaled, and whenever DLSS is not what scales
/// the picture (FSR, the plain scaling, the NGX plugin, a size DLSS cannot take, a failure).
inline std::atomic<bool>& TemporalUsable() {
    static std::atomic<bool> usable{false};
    return usable;
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

/// The prototype is requested, not switched off with its key, and DLSS can take its inputs.
inline bool TemporalActive() {
    return TemporalRequested() && TemporalSwitch().load(std::memory_order_relaxed) &&
           TemporalUsable().load(std::memory_order_relaxed);
}

/// The jitter's sign, measured on 2026-11-12: with the car at a standstill, 126 pairs of frames
/// in a row of the un-resolved scene moved by -1.0 x the change of (c14*c20, c15*c21) on both
/// axes (correlation -0.97; dev\captures\lire-jitter-dlss.py on
/// partie-20261112-091150-jitter-dlss), and by eye the user found choice A the steadiest. So the
/// scene's shift, which is what DLSS takes as its jitter, is -(c14*c20, c15*c21): choice A,
/// what NoteTemporalDraw notes. GT Sport's DLSS pass (shadGT, ryty_gts_dlss.cpp) hands DLSS the
/// opposite for the same pass. Page Down still steps through the other signs while playing, for
/// comparison. Factors of the noted jitter.
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

/// How a frame's HUD is drawn, switched with Page Down while playing (2026-11-12, to measure
/// what each way costs: the main menu fell from 60 to 57 frames a second with the frames "left
/// as the game draws them", which a G-SYNC screen shows as flicker).
///   auto     as the game draws it only while its interface reads the display back, or a scene
///            has just started; else out of the display, with the record over white in place
///            outside a race and in the HUD's own list in a race
///   miroir   every frame left as the game draws it (three records a HUD draw)
///   direct   HUD kept out of the display, the record over white in place (two)
///   liste    HUD kept out of the display, the record over white in the HUD's own list (run after
///            the main list: a menu's pieces then come out with the wrong textures)
inline constexpr const char* TemporalHudModes[] = {"auto", "miroir", "direct", "liste"};
inline std::atomic<u32>& TemporalHudMode() {
    static std::atomic<u32> mode{0};
    return mode;
}

/// The light glows GT7 draws after its anti-aliasing into the pass's result (the halo sprites,
/// ps 0x12b8ffe0 in a race, 56 quads over a depth test), switched with Home while playing. They
/// land on the un-resolved colour DLSS gets, which is shifted by the frame's jitter; if the game
/// draws them for the un-jittered picture (where they go in its own pipeline), DLSS moves them
/// by the jitter each frame and they shimmer (the user's report of 2026-11-12, to be checked).
///   normaux  drawn as the game draws them
///   decales  drawn shifted by the frame's jitter, like the scene under them
///   sans     left out of DLSS's picture (a test only: the glows then go missing)
inline constexpr const char* TemporalHaloModes[] = {"normaux", "decales", "sans"};
inline std::atomic<u32>& TemporalHaloMode() {
    static std::atomic<u32> mode{0};
    return mode;
}

/// In menus the interface stays in the picture DLSS gets (no HUD pictures there): drawn shifted
/// by the frame's jitter like the scene under it (true), or where the game puts it. Switched with
/// Home while playing (2026-11-12, to judge which DLSS keeps the sharper and steadier).
inline std::atomic<bool>& TemporalUiJitter() {
    static std::atomic<bool> on{true};
    return on;
}

/// In such menus, where the interface drew and stays put on the screen, the motion handed to DLSS
/// is none (true): the labels stay put while the camera turns (the garage's smeared behind it,
/// 2026-11-12), while what follows the scene (the World Map's icons) keeps the scene's motion
/// (dlss_motion.comp). Switched with End while playing; RYGT_DLSS_TEMPORAL_UI_MASK=0 starts off.
inline std::atomic<bool>& TemporalUiMask() {
    static std::atomic<bool> on{[] {
        const char* env = std::getenv("RYGT_DLSS_TEMPORAL_UI_MASK");
        return !(env && env[0] == '0');
    }()};
    return on;
}

/// The HUD pictures at twice the display's size (true) or at its size, switched with End while
/// playing: the 4K pictures cost four times the fill for every interface draw, drawn two or three
/// times over (2026-11-12, the main menu at 45 frames a second instead of 60).
inline std::atomic<bool>& TemporalHudFull() {
    static std::atomic<bool> full{true};
    return full;
}

/// The DLSS model (preset): no key any more (Home was tried on 2026-11-12, no change seen); the
/// default of the quality mode
/// (M in performance mode), then K, L and J. To judge which keeps small lights the steadiest.
inline constexpr const char* TemporalPresets[] = {"defaut", "K", "L", "J"};
inline std::atomic<u32>& TemporalPreset() {
    static std::atomic<u32> preset{0};
    return preset;
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

/// DLSS-GT7-002 (test of 2026-11-13): the texture LOD bias NVIDIA's guide asks for with DLSS,
/// log2(render width / display width) - 1, so -2 from 1920 to 3840, on the samplers RyGT-Team/
/// shadgt7 PR 42 gives it to. The scene is drawn at half the display's size: without the bias
/// the game picks its mips for 1080p and DLSS has nothing finer to rebuild (leaves, grass).
/// Zero until DLSS has taken a frame with the game's depth, motion vectors and jitter, so a run
/// without the prototype samples as before; then kept for the run, as menus alternate frames
/// with and without the pass and a bias that followed them would swap every sampler each time.
///   RYGT_DLSS_TEMPORAL_MIP_BIAS=0   no bias
///   RYGT_DLSS_TEMPORAL_MIP_EXTRA=x  the term added to the ratio (default -1; from -2 to 0)
inline std::atomic<float>& TemporalMipBias() {
    static std::atomic<float> bias{0.0f};
    return bias;
}

/// Called by the presenter's DLSS pass after a frame scaled with the temporal inputs.
inline void NoteTemporalScale(vk::Extent2D render, vk::Extent2D display) {
    static const bool wanted = [] {
        const char* env = std::getenv("RYGT_DLSS_TEMPORAL_MIP_BIAS");
        return !(env && env[0] == '0');
    }();
    static const float extra = [] {
        const char* env = std::getenv("RYGT_DLSS_TEMPORAL_MIP_EXTRA");
        const float value = env && *env ? std::strtof(env, nullptr) : -1.0f;
        return std::isfinite(value) ? std::clamp(value, -2.0f, 0.0f) : -1.0f;
    }();
    if (!wanted || render.width == 0 || display.width <= render.width) {
        return;
    }
    const float value =
        std::log2(static_cast<float>(render.width) / static_cast<float>(display.width)) + extra;
    auto& bias = TemporalMipBias();
    if (bias.load(std::memory_order_relaxed) != value) {
        bias.store(value, std::memory_order_relaxed);
        LOG_WARNING(Render_Vulkan,
                    "DLSS-GT7-002: texture bias {:.2f} on the material samplers from now on "
                    "({}x{} scaled to {}x{})",
                    value, render.width, render.height, display.width, display.height);
    }
}

} // namespace Vulkan::RytyUpscale
