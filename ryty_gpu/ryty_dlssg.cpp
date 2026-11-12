// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "ryty_gpu/ryty_dlssg.h"
#include "ryty_gpu/ryty_upscale.h"
#include "video_core/host_shaders/dlss_motion_comp.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"

// Streamline's headers (MIT, ryty_gpu/streamline). Its functions are loaded from
// sl.interposer.dll at run time; the header helpers that need the import library are not used.
#include "ryty_gpu/streamline/sl.h"
#include "ryty_gpu/streamline/sl_consts.h"
#include "ryty_gpu/streamline/sl_dlss.h"
#include "ryty_gpu/streamline/sl_dlss_g.h"
#include "ryty_gpu/streamline/sl_pcl.h"
#include "ryty_gpu/streamline/sl_reflex.h"

namespace Vulkan::RytyFrameGen {
extern std::atomic<u64> g_shown_frames; // ryty_framegen.cpp, read by the FPS counter
}

namespace Vulkan::RytyDlssG {

namespace {

using PFunInit = sl::Result (*)(const sl::Preferences&, uint64_t);
using PFunShutdown = sl::Result (*)();
using PFunIsFeatureSupported = sl::Result (*)(sl::Feature, const sl::AdapterInfo&);
using PFunSetTagForFrame = sl::Result (*)(const sl::FrameToken&, const sl::ViewportHandle&,
                                          const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
using PFunSetConstants = sl::Result (*)(const sl::Constants&, const sl::FrameToken&,
                                        const sl::ViewportHandle&);
using PFunGetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
using PFunGetNewFrameToken = sl::Result (*)(sl::FrameToken*&, const uint32_t*);
using PFunEvaluateFeature = sl::Result (*)(sl::Feature, const sl::FrameToken&,
                                           const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);

// DLSS-GT7-001: GT7's motion vectors hold the frame's own jitter (measured on 2026-11-12: in
// still menu scenes every pixel carries the same vector, the un-resolved picture's shift against
// the history, to 0.03 pixel). Handed as they are, DLSS read its history a quarter of a pixel
// off in a still scene and the picture trembled. A compute pass (dlss_motion.comp) writes them
// again with the jitter taken out, into a picture of this file's own.
// RYGT_DLSS_TEMPORAL_MOTION=0: the game's vectors as they are.
struct MotionFix {
    vk::Extent2D size{};
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    bool failed = false;
    u64 frames = 0;
    /// The pictures DLSS got, kept by the pass in menu frames: this frame's and the one before.
    std::array<VideoCore::UniqueImage, 2> pictures;
    std::array<vk::UniqueImageView, 2> picture_views;
    u32 next_picture = 0;       ///< the one this frame's goes into
    bool have_previous = false; ///< the other holds the frame before's
    /// The interface's own shift per 16x16 tile (dlss_motion.comp, stage 1).
    VideoCore::UniqueImage tiles;
    vk::UniqueImageView tiles_view;
    vk::Extent2D tiles_size{};
    /// The interface's shift in the last masked frame (the jitter, when it is drawn shifted).
    std::array<float, 2> last_ui_shift{};
};
MotionFix g_motion_fix;

bool MotionFixOn() {
    static const bool on = [] {
        const char* env = std::getenv("RYGT_DLSS_TEMPORAL_MOTION");
        return !(env && env[0] == '0');
    }();
    return on;
}

/// Records the pass into cmdbuf; false when the game's vectors must be used as they are. input:
/// the colour DLSS gets (shader read-only layout), compared with t.ui_scene_view when there is one.
bool FixMotion(vk::Device device, VmaAllocator allocator, vk::CommandBuffer cmdbuf,
               const RytyUpscale::TemporalInputs& t, vk::ImageView input) {
    MotionFix& fix = g_motion_fix;
    if (fix.failed || !MotionFixOn() || t.mvec_scale[0] == 0.0f || t.mvec_scale[1] == 0.0f) {
        return false;
    }
    if (!fix.pipeline) {
        const std::array<vk::DescriptorSetLayoutBinding, 7> bindings{{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 2,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 3,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 4,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 5,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 6,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        auto [set_result, set_layout] =
            device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{
                .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                .bindingCount = static_cast<u32>(bindings.size()),
                .pBindings = bindings.data(),
            });
        if (set_result != vk::Result::eSuccess) {
            fix.failed = true;
            return false;
        }
        fix.set_layout = std::move(set_layout);
        const vk::PushConstantRange range{
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .offset = 0,
            .size = 10 * sizeof(float),
        };
        auto [layout_result, layout] =
            device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
                .setLayoutCount = 1,
                .pSetLayouts = &fix.set_layout.get(),
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &range,
            });
        if (layout_result != vk::Result::eSuccess) {
            fix.failed = true;
            return false;
        }
        fix.layout = std::move(layout);
        const vk::ShaderModule module = CompileSPV(DLSS_MOTION_COMP, device);
        if (!module) {
            fix.failed = true;
            return false;
        }
        auto [pipeline_result, pipeline] =
            device.createComputePipelineUnique({}, vk::ComputePipelineCreateInfo{
                                                       .stage{
                                                           .stage = vk::ShaderStageFlagBits::eCompute,
                                                           .module = module,
                                                           .pName = "main",
                                                       },
                                                       .layout = fix.layout.get(),
                                                   });
        device.destroyShaderModule(module);
        if (pipeline_result != vk::Result::eSuccess) {
            fix.failed = true;
            LOG_ERROR(Render_Vulkan, "DLSS-GT7-001: the motion pass could not be built; the "
                                     "game's motion vectors are used as they are");
            return false;
        }
        fix.pipeline = std::move(pipeline);
    }
    bool fresh_pictures = false;
    if (fix.size != t.size || !fix.view) {
        fix.view.reset();
        fix.image.Destroy();
        fix.image = VideoCore::UniqueImage(device, allocator);
        fix.image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16Sfloat,
            .extent{.width = t.size.width, .height = t.size.height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        auto [view_result, view] = device.createImageViewUnique(vk::ImageViewCreateInfo{
            .image = fix.image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16Sfloat,
            .subresourceRange{.aspectMask = vk::ImageAspectFlagBits::eColor,
                              .levelCount = 1,
                              .layerCount = 1},
        });
        if (view_result != vk::Result::eSuccess) {
            fix.failed = true;
            return false;
        }
        fix.view = std::move(view);
        // The pictures DLSS got (8 bits a channel are enough to match them).
        for (u32 i = 0; i < 2; ++i) {
            fix.picture_views[i].reset();
            fix.pictures[i].Destroy();
            fix.pictures[i] = VideoCore::UniqueImage(device, allocator);
            fix.pictures[i].Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eR8G8B8A8Unorm,
                .extent{.width = t.size.width, .height = t.size.height, .depth = 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
            auto [picture_result, picture_view] =
                device.createImageViewUnique(vk::ImageViewCreateInfo{
                    .image = fix.pictures[i],
                    .viewType = vk::ImageViewType::e2D,
                    .format = vk::Format::eR8G8B8A8Unorm,
                    .subresourceRange{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                      .levelCount = 1,
                                      .layerCount = 1},
                });
            if (picture_result != vk::Result::eSuccess) {
                fix.failed = true;
                return false;
            }
            fix.picture_views[i] = std::move(picture_view);
        }
        // The tiles' shifts: a 16x16 tile a texel.
        fix.tiles_view.reset();
        fix.tiles.Destroy();
        fix.tiles = VideoCore::UniqueImage(device, allocator);
        fix.tiles_size = vk::Extent2D{(t.size.width + 15) / 16, (t.size.height + 15) / 16};
        fix.tiles.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent{.width = fix.tiles_size.width, .height = fix.tiles_size.height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eStorage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        auto [tiles_result, tiles_view] = device.createImageViewUnique(vk::ImageViewCreateInfo{
            .image = fix.tiles,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange{.aspectMask = vk::ImageAspectFlagBits::eColor,
                              .levelCount = 1,
                              .layerCount = 1},
        });
        if (tiles_result != vk::Result::eSuccess) {
            fix.failed = true;
            return false;
        }
        fix.tiles_view = std::move(tiles_view);
        fix.have_previous = false;
        fix.next_picture = 0;
        fresh_pictures = true;
        fix.size = t.size;
    }
    constexpr vk::ImageSubresourceRange range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    // Every pixel is written: what the picture held is dropped. The game's own picture is read
    // after whatever wrote it.
    const vk::ImageMemoryBarrier2 to_write{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = fix.image,
        .subresourceRange = range,
    };
    const vk::MemoryBarrier2 written{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    // The kept pictures stay in the general layout once made (the frame before's picture is read
    // after the barrier above, this frame's written after it: it names every command before).
    std::array<vk::ImageMemoryBarrier2, 4> image_barriers{to_write, to_write, to_write, to_write};
    u32 image_barrier_count = 1;
    if (fresh_pictures) {
        for (const vk::Image made :
             {static_cast<vk::Image>(fix.pictures[0]), static_cast<vk::Image>(fix.pictures[1]),
              static_cast<vk::Image>(fix.tiles)}) {
            image_barriers[image_barrier_count] = to_write;
            image_barriers[image_barrier_count].dstAccessMask =
                vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderStorageRead |
                vk::AccessFlagBits2::eShaderSampledRead;
            image_barriers[image_barrier_count].image = made;
            ++image_barrier_count;
        }
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &written,
        .imageMemoryBarrierCount = image_barrier_count,
        .pImageMemoryBarriers = image_barriers.data(),
    });
    const vk::DescriptorImageInfo source{
        .imageView = t.motion_view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };
    const vk::DescriptorImageInfo target{
        .imageView = *fix.view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };
    // Without the display before its interface, the colour is compared with itself (no mask).
    const bool mask = t.ui_scene_view && input;
    const vk::DescriptorImageInfo shown{
        .imageView = input ? input : t.motion_view,
        .imageLayout = input ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eGeneral,
    };
    const vk::DescriptorImageInfo scene = mask ? vk::DescriptorImageInfo{
                                                     .imageView = t.ui_scene_view,
                                                     .imageLayout = vk::ImageLayout::eGeneral,
                                                 }
                                               : shown;
    const vk::DescriptorImageInfo previous{
        .imageView = *fix.picture_views[fix.next_picture ^ 1],
        .imageLayout = vk::ImageLayout::eGeneral,
    };
    const vk::DescriptorImageInfo keep{
        .imageView = *fix.picture_views[fix.next_picture],
        .imageLayout = vk::ImageLayout::eGeneral,
    };
    const vk::DescriptorImageInfo tiles{
        .imageView = *fix.tiles_view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };
    const std::array<vk::WriteDescriptorSet, 7> writes{{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &source},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &target},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &scene},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &shown},
        {.dstBinding = 4,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &previous},
        {.dstBinding = 5,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &keep},
        {.dstBinding = 6,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &tiles},
    }};
    // jitter (pixels) = -(c14*c20, c15*c21) and scale = -(c14, c15) / size: back to (c20, c21).
    // Then the vectors' unit in pixels (vector * scale * size = where it was - where it is), the
    // mask, the least change taken for the interface (half a step of a 10-bit display's colour,
    // so equal pictures never count), whether the frame before's picture is there, the stage,
    // and the interface's shift in the frame before minus in this one (it is drawn shifted by the
    // frame's jitter when Home leaves it so: the pictures' shift is the jitters' difference more
    // than its motion).
    const bool ui_jittered = RytyUpscale::TemporalUiJitter().load(std::memory_order_relaxed);
    const std::array<float, 2> ui_shift{ui_jittered ? t.jitter[0] : 0.0f,
                                        ui_jittered ? t.jitter[1] : 0.0f};
    const bool search = mask && fix.have_previous;
    std::array<float, 10> jitter{
        t.jitter[0] / (t.mvec_scale[0] * static_cast<float>(t.size.width)),
        t.jitter[1] / (t.mvec_scale[1] * static_cast<float>(t.size.height)),
        t.mvec_scale[0] * static_cast<float>(t.size.width),
        t.mvec_scale[1] * static_cast<float>(t.size.height),
        mask ? 1.0f : 0.0f,
        0.5f / 1023.0f,
        search ? 1.0f : 0.0f,
        0.0f,
        fix.last_ui_shift[0] - ui_shift[0],
        fix.last_ui_shift[1] - ui_shift[1],
    };
    static u64 masked_frames = 0;
    if (mask && masked_frames++ % 600 == 0) {
        LOG_WARNING(Render_Vulkan,
                    "DLSS-GT7-001: the menu's interface handed to DLSS with its own motion, "
                    "measured per tile against the frame before ({} frames so)",
                    masked_frames);
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *fix.pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *fix.layout, 0, writes);
    if (search) {
        // Stage 1: the interface's shift per tile; stage 0 reads it.
        jitter[7] = 1.0f;
        cmdbuf.pushConstants(*fix.layout, vk::ShaderStageFlagBits::eCompute, 0,
                             static_cast<u32>(sizeof(jitter)), jitter.data());
        cmdbuf.dispatch(fix.tiles_size.width, fix.tiles_size.height, 1);
        const vk::MemoryBarrier2 tiles_written{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &tiles_written,
        });
        jitter[7] = 0.0f;
    }
    cmdbuf.pushConstants(*fix.layout, vk::ShaderStageFlagBits::eCompute, 0,
                         static_cast<u32>(sizeof(jitter)), jitter.data());
    cmdbuf.dispatch((t.size.width + 7) / 8, (t.size.height + 7) / 8, 1);
    // This frame's picture is kept for the next only in a masked frame.
    fix.have_previous = mask;
    if (mask) {
        fix.next_picture ^= 1;
        fix.last_ui_shift = ui_shift;
    }
    const vk::ImageMemoryBarrier2 to_read{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = fix.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_read,
    });
    if (fix.frames++ == 0) {
        LOG_WARNING(Render_Vulkan,
                    "DLSS-GT7-001: motion vectors handed to DLSS without the frame's jitter "
                    "({:.6f}, {:.6f} in their unit in this frame)",
                    jitter[0], jitter[1]);
    }
    return true;
}

// Zero motion vectors and a flat depth image (the game picture comes without either).
struct AuxImages {
    vk::Extent2D size{};
    VideoCore::UniqueImage motion_vectors;
    VideoCore::UniqueImage depth;
    vk::UniqueImageView motion_vectors_view;
    vk::UniqueImageView depth_view;

    bool Ready(vk::Extent2D wanted) const {
        return wanted == size && motion_vectors_view && depth_view;
    }
    void Destroy() {
        motion_vectors_view.reset();
        depth_view.reset();
        motion_vectors.Destroy();
        depth.Destroy();
        size = {};
    }
};

struct Streamline {
    HMODULE module{};
    PFunInit init{};
    PFunShutdown shutdown{};
    PFunIsFeatureSupported is_supported{};
    PFunSetTagForFrame set_tag{};
    PFunSetConstants set_constants{};
    PFunGetFeatureFunction get_feature_function{};
    PFunGetNewFrameToken new_frame_token{};
    PFunEvaluateFeature evaluate{};
    PFun_slDLSSGSetOptions* dlssg_set_options{};
    PFun_slDLSSGGetState* dlssg_get_state{};
    int last_status = -1;
    PFun_slReflexSetOptions* reflex_set_options{};
    PFun_slPCLSetMarker* pcl_set_marker{};
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
    bool tried{};
    bool features_ready{};
    bool options_set{};
    sl::FrameToken* frame{};
    u32 frame_index{};
    std::mutex mutex;
    AuxImages fg; // frame generation, at the swapchain size
    // DLSS upscaling (sl.dlss.dll) on its own viewport, at the game picture's size.
    bool upscale_requested{};
    bool upscale_ready{};
    PFun_slDLSSGetOptimalSettings* dlss_get_optimal{};
    PFun_slDLSSSetOptions* dlss_set_options{};
    AuxImages sr;
    vk::Extent2D sr_in{}, sr_out{};
    int sr_mode = -1; // DLSSMode for sr_in -> sr_out, -1 when not supported
    bool sr_options_set{};
    // The frame token the upscaler (draw thread) started for the next presented frame; frame
    // generation's Prepare (present thread) carries on with it instead of starting another.
    // Kept apart from frame, which the present thread's markers use for the frame on screen.
    sl::FrameToken* upscale_frame{};
};
Streamline g_sl;

std::wstring DlssDir() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir{exe};
    return dir.substr(0, dir.find_last_of(L"\\/")) + L"\\dlss";
}

std::wstring LogDir() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir{exe};
    return dir.substr(0, dir.find_last_of(L"\\/")) + L"\\user\\log";
}

void SlLog(sl::LogType type, const char* message) {
    if (type == sl::LogType::eError) {
        LOG_ERROR(Render_Vulkan, "Streamline: {}", message ? message : "");
    } else if (type == sl::LogType::eWarn) {
        LOG_WARNING(Render_Vulkan, "Streamline: {}", message ? message : "");
    } else {
        LOG_INFO(Render_Vulkan, "Streamline: {}", message ? message : "");
    }
}

template <typename T>
T* Feature(sl::Feature feature, const char* name) {
    void* function{};
    if (!g_sl.get_feature_function ||
        g_sl.get_feature_function(feature, name, function) != sl::Result::eOk) {
        return nullptr;
    }
    return reinterpret_cast<T*>(function);
}

bool LoadFeatureFunctions() {
    if (g_sl.features_ready) {
        return g_sl.dlssg_set_options != nullptr;
    }
    g_sl.features_ready = true;
    g_sl.dlssg_set_options =
        Feature<PFun_slDLSSGSetOptions>(sl::kFeatureDLSS_G, "slDLSSGSetOptions");
    g_sl.dlssg_get_state = Feature<PFun_slDLSSGGetState>(sl::kFeatureDLSS_G, "slDLSSGGetState");
    g_sl.reflex_set_options =
        Feature<PFun_slReflexSetOptions>(sl::kFeatureReflex, "slReflexSetOptions");
    g_sl.pcl_set_marker = Feature<PFun_slPCLSetMarker>(sl::kFeaturePCL, "slPCLSetMarker");
    if (!g_sl.dlssg_set_options) {
        LOG_ERROR(Render_Vulkan, "ryGT DLSS-G: DLSS frame generation is not available here "
                                 "(see the Streamline lines above: it needs an RTX 40 or newer "
                                 "GPU and Windows' hardware-accelerated GPU scheduling)");
        return false;
    }
    if (g_sl.reflex_set_options) {
        sl::ReflexOptions reflex{};
        reflex.mode = sl::ReflexMode::eLowLatency;
        g_sl.reflex_set_options(reflex);
    }
    return true;
}

void CreateAuxImages(AuxImages& aux, vk::Device device, VmaAllocator allocator,
                     vk::CommandBuffer cmdbuf,
                     vk::Extent2D size) {
    aux.Destroy();
    aux.size = size;
    aux.motion_vectors = VideoCore::UniqueImage(device, allocator);
    aux.depth = VideoCore::UniqueImage(device, allocator);
    const auto make = [&](VideoCore::UniqueImage& image, vk::Format format,
                          vk::UniqueImageView& view) {
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent{.width = size.width, .height = size.height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        auto [result, created] = device.createImageViewUnique(vk::ImageViewCreateInfo{
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange{.aspectMask = vk::ImageAspectFlagBits::eColor,
                              .levelCount = 1,
                              .layerCount = 1},
        });
        if (result == vk::Result::eSuccess) {
            view = std::move(created);
        }
    };
    make(aux.motion_vectors, vk::Format::eR16G16Sfloat, aux.motion_vectors_view);
    make(aux.depth, vk::Format::eR32Sfloat, aux.depth_view);

    constexpr vk::ImageSubresourceRange range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const std::array<vk::Image, 2> images = {aux.motion_vectors, aux.depth};
    std::array<vk::ImageMemoryBarrier2, 2> to_clear{};
    std::array<vk::ImageMemoryBarrier2, 2> to_read{};
    for (size_t i = 0; i < images.size(); ++i) {
        to_clear[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = images[i],
            .subresourceRange = range,
        };
        to_read[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = images[i],
            .subresourceRange = range,
        };
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(to_clear.size()),
        .pImageMemoryBarriers = to_clear.data(),
    });
    const vk::ClearColorValue zero{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
    for (const vk::Image image : images) {
        cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, zero, range);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(to_read.size()),
        .pImageMemoryBarriers = to_read.data(),
    });
}

sl::float4x4 Identity() {
    sl::float4x4 m;
    m.setRow(0, sl::float4(1, 0, 0, 0));
    m.setRow(1, sl::float4(0, 1, 0, 0));
    m.setRow(2, sl::float4(0, 0, 1, 0));
    m.setRow(3, sl::float4(0, 0, 0, 1));
    return m;
}

// Constants for a picture without camera data: identity matrices, no jitter, zero motion.
sl::Constants FlatConstants(vk::Extent2D size, bool reset) {
    sl::Constants constants{};
    constants.cameraViewToClip = Identity();
    constants.clipToCameraView = Identity();
    constants.clipToLensClip = Identity();
    constants.clipToPrevClip = Identity();
    constants.prevClipToClip = Identity();
    constants.jitterOffset = sl::float2(0.0f, 0.0f);
    constants.mvecScale = sl::float2(1.0f, 1.0f);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
    constants.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    constants.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
    constants.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
    constants.cameraNear = 0.1f;
    constants.cameraFar = 10000.0f;
    constants.cameraFOV = 1.0f;
    constants.cameraAspectRatio = float(size.width) / float(std::max(size.height, 1u));
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    return constants;
}

sl::Resource Texture(vk::Image image, vk::ImageView view, vk::ImageLayout layout,
                     vk::Extent2D size, vk::Format format, vk::ImageUsageFlags usage) {
    sl::Resource resource{sl::ResourceType::eTex2d, static_cast<VkImage>(image), nullptr,
                          static_cast<VkImageView>(view), static_cast<uint32_t>(layout)};
    resource.width = size.width;
    resource.height = size.height;
    resource.nativeFormat = static_cast<uint32_t>(format);
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0; // no VkImageCreateFlags (Resource leaves this field uninitialized)
    resource.usage = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(usage));
    return resource;
}

std::wstring DlssFile(const wchar_t* name) {
    return DlssDir() + L"\\" + name;
}

void Marker(sl::PCLMarker marker) {
    if (g_sl.pcl_set_marker && g_sl.frame) {
        g_sl.pcl_set_marker(marker, *g_sl.frame);
    }
}

} // namespace

bool Requested() {
    return EmulatorSettings.IsDlssgEnabled();
}

bool Active() {
    return g_sl.get_instance_proc_addr != nullptr;
}

bool UpscaleRequested() {
    if (!EmulatorSettings.IsDlssEnabled()) {
        return false;
    }
    // RYGT_DLSS_STREAMLINE=0 keeps upscaling on the self-built ryGT_dlss.dll plugin.
    if (const char* env = std::getenv("RYGT_DLSS_STREAMLINE"); env && env[0] == '0') {
        return false;
    }
    return GetFileAttributesW(DlssFile(L"sl.dlss.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
}

PFN_vkGetInstanceProcAddr LoadInterposer() {
    if (g_sl.tried) {
        return g_sl.get_instance_proc_addr;
    }
    g_sl.tried = true;
    g_sl.upscale_requested = UpscaleRequested();
    if (!Requested() && !g_sl.upscale_requested) {
        return nullptr;
    }
    const std::wstring dir = DlssDir();
    const std::wstring path = dir + L"\\sl.interposer.dll";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        LOG_ERROR(Render_Vulkan,
                  "ryGT DLSS-G: DLSS is on but dlss\\sl.interposer.dll could not be loaded "
                  "(error {}); no DLSS frame generation or Streamline upscaling",
                  GetLastError());
        return nullptr;
    }
    const auto get = [&](const char* name) { return GetProcAddress(module, name); };
    g_sl.init = reinterpret_cast<PFunInit>(get("slInit"));
    g_sl.shutdown = reinterpret_cast<PFunShutdown>(get("slShutdown"));
    g_sl.is_supported = reinterpret_cast<PFunIsFeatureSupported>(get("slIsFeatureSupported"));
    g_sl.set_tag = reinterpret_cast<PFunSetTagForFrame>(get("slSetTagForFrame"));
    g_sl.set_constants = reinterpret_cast<PFunSetConstants>(get("slSetConstants"));
    g_sl.get_feature_function =
        reinterpret_cast<PFunGetFeatureFunction>(get("slGetFeatureFunction"));
    g_sl.new_frame_token = reinterpret_cast<PFunGetNewFrameToken>(get("slGetNewFrameToken"));
    g_sl.evaluate = reinterpret_cast<PFunEvaluateFeature>(get("slEvaluateFeature"));
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(get("vkGetInstanceProcAddr"));
    if (!g_sl.init || !g_sl.set_tag || !g_sl.set_constants || !g_sl.get_feature_function ||
        !g_sl.new_frame_token || !g_sl.evaluate || !gipa) {
        LOG_ERROR(Render_Vulkan, "ryGT DLSS-G: dlss\\sl.interposer.dll lacks Streamline's API");
        FreeLibrary(module);
        return nullptr;
    }

    static const std::wstring plugin_dir = dir;
    static const std::wstring log_dir = LogDir();
    static const wchar_t* plugin_paths[] = {plugin_dir.c_str()};
    // Frame generation (DLSS-G with Reflex and PCL) and/or upscaling (DLSS), as asked.
    static std::vector<sl::Feature> features;
    if (Requested()) {
        features.insert(features.end(),
                        {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL});
    }
    if (g_sl.upscale_requested) {
        features.push_back(sl::kFeatureDLSS);
    }
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = plugin_paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = log_dir.c_str();
    pref.logMessageCallback = SlLog;
    // Streamline's interposer stands in for vulkan-1.dll (its vkGetInstanceProcAddr gives the
    // driver's functions except for the few it intercepts), and its device proxy adds the
    // extensions, features and queues DLSS-G needs. No over-the-air downloads.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features.data();
    pref.numFeaturesToLoad = static_cast<u32>(features.size());
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "ryGT";
    pref.projectId = "5f1a2b3c-4d5e-4f60-8a7b-9c0d1e2f3a4b";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result result = g_sl.init(pref, sl::kSDKVersion);
    if (result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "ryGT DLSS-G: Streamline did not start (result {}); no DLSS "
                                 "frame generation",
                  static_cast<int>(result));
        FreeLibrary(module);
        return nullptr;
    }
    g_sl.module = module;
    g_sl.get_instance_proc_addr = gipa;
    LOG_WARNING(Render_Vulkan, "ryGT DLSS-G: Streamline started ({}{}); Vulkan goes through it",
                Requested() ? "frame generation" : "",
                g_sl.upscale_requested ? (Requested() ? ", upscaling" : "upscaling") : "");
    return gipa;
}

void Prepare(vk::Device device, VmaAllocator allocator, vk::CommandBuffer cmdbuf,
             vk::Extent2D size) {
    if (!Active() || !Requested()) {
        return;
    }
    std::scoped_lock lock{g_sl.mutex};
    if (!LoadFeatureFunctions()) {
        return;
    }
    if (!g_sl.fg.Ready(size)) {
        CreateAuxImages(g_sl.fg, device, allocator, cmdbuf, size);
    }
    if (g_sl.upscale_frame) {
        g_sl.frame = std::exchange(g_sl.upscale_frame, nullptr);
    } else {
        ++g_sl.frame_index;
        if (g_sl.new_frame_token(g_sl.frame, &g_sl.frame_index) != sl::Result::eOk ||
            !g_sl.frame) {
            g_sl.frame = nullptr;
            return;
        }
    }
    Marker(sl::PCLMarker::eSimulationStart);
    Marker(sl::PCLMarker::eSimulationEnd);
    Marker(sl::PCLMarker::eRenderSubmitStart);

    const sl::ViewportHandle viewport{0u};
    const sl::Constants constants = FlatConstants(size, false);
    g_sl.set_constants(constants, *g_sl.frame, viewport);

    const sl::Extent extent{0, 0, size.width, size.height};
    sl::Resource mvec{sl::ResourceType::eTex2d,
                      static_cast<VkImage>(static_cast<vk::Image>(g_sl.fg.motion_vectors)), nullptr,
                      static_cast<VkImageView>(*g_sl.fg.motion_vectors_view),
                      static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
    mvec.width = size.width;
    mvec.height = size.height;
    mvec.nativeFormat = static_cast<uint32_t>(VK_FORMAT_R16G16_SFLOAT);
    mvec.mipLevels = 1;
    mvec.arrayLayers = 1;
    mvec.flags = 0;
    mvec.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sl::Resource dep{sl::ResourceType::eTex2d,
                     static_cast<VkImage>(static_cast<vk::Image>(g_sl.fg.depth)), nullptr,
                     static_cast<VkImageView>(*g_sl.fg.depth_view),
                     static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
    dep.width = size.width;
    dep.height = size.height;
    dep.nativeFormat = static_cast<uint32_t>(VK_FORMAT_R32_SFLOAT);
    dep.mipLevels = 1;
    dep.arrayLayers = 1;
    dep.flags = 0;
    dep.usage = mvec.usage;
    // The backbuffer tag only carries the extent (Streamline knows the swapchain image).
    sl::Resource backbuffer{sl::ResourceType::eTex2d, nullptr, nullptr, nullptr};
    backbuffer.flags = 0;
    const sl::ResourceTag tags[] = {
        sl::ResourceTag{&backbuffer, sl::kBufferTypeBackbuffer,
                        sl::ResourceLifecycle::eValidUntilPresent, &extent},
        sl::ResourceTag{&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent,
                        &extent},
        sl::ResourceTag{&dep, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
                        &extent},
    };
    auto* command_buffer =
        reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(cmdbuf));
    g_sl.set_tag(*g_sl.frame, viewport, tags, static_cast<u32>(std::size(tags)), command_buffer);

    if (!g_sl.options_set) {
        sl::DLSSGOptions options{};
        options.mode = sl::DLSSGMode::eOn;
        // GT-MFG: frames generated between game frames, RYGT_DLSSG_FRAMES=1..5 (2x to 6x;
        // default 1; multi-frame generation needs an RTX 50, 6x a recent driver). DLSS-G
        // reports what the GPU supports only once it runs, so the request goes in as asked and
        // falls back to one generated frame when refused.
        u32 frames = 1;
        if (const char* env = std::getenv("RYGT_DLSSG_FRAMES"); env && *env) {
            frames = std::clamp<u32>(static_cast<u32>(std::strtoul(env, nullptr, 10)), 1u, 5u);
        }
        options.numFramesToGenerate = frames;
        sl::Result set = g_sl.dlssg_set_options(viewport, options);
        if (set != sl::Result::eOk && frames > 1) {
            options.numFramesToGenerate = 1;
            set = g_sl.dlssg_set_options(viewport, options);
        }
        g_sl.options_set = set == sl::Result::eOk;
        LOG_WARNING(Render_Vulkan,
                    "ryGT DLSS-G: frame generation {} (result {}), {} generated frames a game "
                    "frame ({} asked)",
                    g_sl.options_set ? "on" : "could not be turned on", static_cast<int>(set),
                    options.numFramesToGenerate, frames);
    }
    Marker(sl::PCLMarker::eRenderSubmitEnd);

    // Frames shown (game and generated) for the FPS counter, and DLSS-G's status when it changes.
    if (g_sl.dlssg_get_state) {
        sl::DLSSGState state{};
        if (g_sl.dlssg_get_state(viewport, state, nullptr) == sl::Result::eOk) {
            RytyFrameGen::g_shown_frames.fetch_add(state.numFramesActuallyPresented,
                                                   std::memory_order_relaxed);
            if (static_cast<int>(state.status) != g_sl.last_status) {
                g_sl.last_status = static_cast<int>(state.status);
                LOG_WARNING(Render_Vulkan,
                            "ryGT DLSS-G: status {:#x} (0 = generating), up to {} generated "
                            "frames supported",
                            g_sl.last_status, state.numFramesToGenerateMax);
            }
        }
    }
}

bool InitUpscale() {
    if (!Active() || !g_sl.upscale_requested) {
        return false;
    }
    std::scoped_lock lock{g_sl.mutex};
    if (g_sl.upscale_ready) {
        return true;
    }
    g_sl.dlss_get_optimal =
        Feature<PFun_slDLSSGetOptimalSettings>(sl::kFeatureDLSS, "slDLSSGetOptimalSettings");
    g_sl.dlss_set_options = Feature<PFun_slDLSSSetOptions>(sl::kFeatureDLSS, "slDLSSSetOptions");
    g_sl.upscale_ready = g_sl.dlss_get_optimal && g_sl.dlss_set_options;
    if (g_sl.upscale_ready) {
        LOG_WARNING(Render_Vulkan, "ryGT DLSS: upscaling through Streamline (sl.dlss.dll)");
    } else {
        LOG_ERROR(Render_Vulkan, "ryGT DLSS: Streamline's DLSS upscaling is not available here "
                                 "(see the Streamline lines above: it needs an NVIDIA RTX GPU and "
                                 "dlss\\nvngx_dlss.dll); using FSR");
    }
    return g_sl.upscale_ready;
}

namespace {

// The DLSS mode for the launcher's Upscaling quality (eOff for Auto).
sl::DLSSMode PreferredMode() {
    switch (RytyUpscale::GetQuality()) {
    case RytyUpscale::Quality::Native:
        return sl::DLSSMode::eDLAA;
    case RytyUpscale::Quality::Quality:
        return sl::DLSSMode::eMaxQuality;
    case RytyUpscale::Quality::Balanced:
        return sl::DLSSMode::eBalanced;
    case RytyUpscale::Quality::Performance:
        return sl::DLSSMode::eMaxPerformance;
    case RytyUpscale::Quality::UltraPerformance:
        return sl::DLSSMode::eUltraPerformance;
    default:
        return sl::DLSSMode::eOff;
    }
}

// The DLSS quality mode whose render size range holds input for output, or -1: the chosen
// quality's mode when it fits, else the first that does.
int FindMode(vk::Extent2D input, vk::Extent2D output) {
    if (input.width > output.width || input.height > output.height) {
        return -1;
    }
    static constexpr sl::DLSSMode all[] = {
        sl::DLSSMode::eDLAA,           sl::DLSSMode::eUltraQuality,
        sl::DLSSMode::eMaxQuality,     sl::DLSSMode::eBalanced,
        sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance,
    };
    std::vector<sl::DLSSMode> modes;
    if (const sl::DLSSMode preferred = PreferredMode(); preferred != sl::DLSSMode::eOff) {
        modes.push_back(preferred);
    }
    modes.insert(modes.end(), std::begin(all), std::end(all));
    for (const sl::DLSSMode mode : modes) {
        sl::DLSSOptions options{};
        options.mode = mode;
        options.outputWidth = output.width;
        options.outputHeight = output.height;
        sl::DLSSOptimalSettings settings{};
        if (g_sl.dlss_get_optimal(options, settings) != sl::Result::eOk) {
            continue;
        }
        const bool fits = input.width >= settings.renderWidthMin &&
                          input.width <= settings.renderWidthMax &&
                          input.height >= settings.renderHeightMin &&
                          input.height <= settings.renderHeightMax;
        // DLAA only for a picture already at the output size.
        if (fits && (mode != sl::DLSSMode::eDLAA || input == output)) {
            return static_cast<int>(mode);
        }
    }
    return -1;
}

} // namespace

bool UpscaleSupports(vk::Extent2D input, vk::Extent2D output) {
    if (!g_sl.upscale_ready) {
        return false;
    }
    std::scoped_lock lock{g_sl.mutex};
    if (input != g_sl.sr_in || output != g_sl.sr_out) {
        g_sl.sr_in = input;
        g_sl.sr_out = output;
        g_sl.sr_mode = FindMode(input, output);
        g_sl.sr_options_set = false;
        LOG_WARNING(Render_Vulkan, "ryGT DLSS: {}x{} to {}x{}: {}", input.width, input.height,
                    output.width, output.height,
                    g_sl.sr_mode < 0 ? std::string{"no DLSS mode fits; using FSR"}
                                     : fmt::format("DLSS mode {}", g_sl.sr_mode));
    }
    return g_sl.sr_mode >= 0;
}

bool Upscale(vk::Device device, VmaAllocator allocator, vk::CommandBuffer cmdbuf,
             vk::Image input_image, vk::ImageView input, vk::Format input_format,
             vk::Extent2D input_size, vk::Image output_image, vk::ImageView output,
             vk::Format output_format, vk::Extent2D output_size,
             const RytyUpscale::TemporalInputs* temporal) {
    if (!UpscaleSupports(input_size, output_size)) {
        return false;
    }
    std::scoped_lock lock{g_sl.mutex};
    const sl::ViewportHandle viewport{1u};
    // DLSS-GT7-001: the model chosen with Home (ryty_upscale.h); a change sets the options again.
    static u32 preset_set = 0;
    const u32 preset_now = RytyUpscale::TemporalPreset().load(std::memory_order_relaxed) %
                           static_cast<u32>(std::size(RytyUpscale::TemporalPresets));
    if (preset_now != preset_set) {
        g_sl.sr_options_set = false;
    }
    if (!g_sl.sr_options_set) {
        sl::DLSSOptions options{};
        options.mode = static_cast<sl::DLSSMode>(g_sl.sr_mode);
        options.outputWidth = output_size.width;
        options.outputHeight = output_size.height;
        options.colorBuffersHDR = sl::Boolean::eFalse;
        constexpr sl::DLSSPreset presets[] = {sl::DLSSPreset::eDefault, sl::DLSSPreset::ePresetK,
                                              sl::DLSSPreset::ePresetL, sl::DLSSPreset::ePresetJ};
        options.dlaaPreset = options.qualityPreset = options.balancedPreset =
            options.performancePreset = options.ultraPerformancePreset =
                options.ultraQualityPreset = presets[preset_now];
        preset_set = preset_now;
        const sl::Result set = g_sl.dlss_set_options(viewport, options);
        if (set != sl::Result::eOk) {
            LOG_ERROR(Render_Vulkan, "ryGT DLSS: Streamline refused the options (result {})",
                      static_cast<int>(set));
            return false;
        }
        g_sl.sr_options_set = true;
    }
    if (!g_sl.sr.Ready(input_size)) {
        CreateAuxImages(g_sl.sr, device, allocator, cmdbuf, input_size);
    }
    // DLSS-GT7-001 (prototype, RYGT_DLSS_TEMPORAL=1; ryty_upscale.h): with the frame's own depth,
    // motion vectors and jitter, DLSS keeps its history from frame to frame. A refusal from
    // Streamline ends the prototype for the run, and the frame is scaled as before.
    static bool temporal_refused = false;
    static bool last_temporal = false;
    static u64 temporal_frames = 0;
    static u64 flat_frames = 0;
    const bool use_temporal = temporal && !temporal_refused && temporal->size == input_size &&
                              temporal->depth_view && temporal->motion_view;
    const auto run = [&](const RytyUpscale::TemporalInputs* t) -> bool {
    sl::FrameToken* frame{};
    ++g_sl.frame_index;
    if (g_sl.new_frame_token(frame, &g_sl.frame_index) != sl::Result::eOk || !frame) {
        return false;
    }
    // Frame generation (when on) carries on with this frame's token at the present.
    if (Requested()) {
        g_sl.upscale_frame = frame;
    }

    // No motion vectors: every frame starts DLSS's history over, like the plugin.
    sl::Constants constants = FlatConstants(input_size, true);
    if (t) {
        static const std::array<float, 4> factors = [] {
            std::array<float, 4> f{};
            RytyUpscale::TemporalFactors("RYGT_DLSS_TEMPORAL_JITTER", &f[0]);
            RytyUpscale::TemporalFactors("RYGT_DLSS_TEMPORAL_MVEC", &f[2]);
            return f;
        }();
        static u32 last_choice = 0;
        const u32 choice_index = RytyUpscale::TemporalJitterIndex().load(std::memory_order_relaxed);
        const auto& choice = RytyUpscale::TemporalJitterNow();
        const bool reset = t->reset || !last_temporal || choice_index != last_choice;
        last_choice = choice_index;
        constants.jitterOffset = sl::float2(t->jitter[0] * factors[0] * choice.x,
                                            t->jitter[1] * factors[1] * choice.y);
        constants.mvecScale =
            sl::float2(t->mvec_scale[0] * factors[2], t->mvec_scale[1] * factors[3]);
        constants.depthInverted = sl::Boolean::eTrue;
        constants.cameraMotionIncluded = sl::Boolean::eTrue;
        constants.motionVectorsJittered = sl::Boolean::eFalse;
        constants.motionVectorsDilated = sl::Boolean::eFalse;
        constants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        if (temporal_frames < 4 || (reset && temporal_frames % 16 == 0)) {
            LOG_WARNING(Render_Vulkan,
                        "DLSS-GT7-001: temporal frame {}: jitter {:.4f},{:.4f} px, motion scale "
                        "{:.4f},{:.4f}, depth {} motion {}, {}x{}{}",
                        temporal_frames, constants.jitterOffset.x, constants.jitterOffset.y,
                        constants.mvecScale.x, constants.mvecScale.y,
                        vk::to_string(t->depth_format), vk::to_string(t->motion_format),
                        t->size.width, t->size.height, reset ? ", history reset" : "");
        }
    }
    g_sl.set_constants(constants, *frame, viewport);

    constexpr auto aux_usage = vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eTransferDst |
                               vk::ImageUsageFlagBits::eTransferSrc;
    // The A2R10G10B10 picture is a view of the game's A2B10G10R10 render target (the view swaps
    // red and blue). Streamline has no name for A2R10G10B10 (it logged an error each frame), so
    // it is told the image's own format; DLSS reads through the view, which keeps the colors.
    const vk::Format tag_format = input_format == vk::Format::eA2R10G10B10UnormPack32
                                      ? vk::Format::eA2B10G10R10UnormPack32
                                      : input_format;
    sl::Resource color_in = Texture(input_image, input, vk::ImageLayout::eShaderReadOnlyOptimal,
                                    input_size, tag_format, vk::ImageUsageFlagBits::eSampled);
    sl::Resource color_out = Texture(output_image, output, vk::ImageLayout::eGeneral, output_size,
                                     output_format,
                                     vk::ImageUsageFlagBits::eStorage |
                                         vk::ImageUsageFlagBits::eSampled);
    sl::Resource mvec = Texture(g_sl.sr.motion_vectors, *g_sl.sr.motion_vectors_view,
                                vk::ImageLayout::eShaderReadOnlyOptimal, input_size,
                                vk::Format::eR16G16Sfloat, aux_usage);
    sl::Resource depth = Texture(g_sl.sr.depth, *g_sl.sr.depth_view,
                                 vk::ImageLayout::eShaderReadOnlyOptimal, input_size,
                                 vk::Format::eR32Sfloat, aux_usage);
    if (t) {
        // The game's own pictures: native mode keeps them in the general layout, and the next
        // frame draws into them again, so they are only promised until the evaluate.
        mvec = Texture(t->motion, t->motion_view, vk::ImageLayout::eGeneral, input_size,
                       t->motion_format,
                       vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment);
        if (FixMotion(device, allocator, cmdbuf, *t, input)) {
            mvec = Texture(g_motion_fix.image, *g_motion_fix.view,
                           vk::ImageLayout::eShaderReadOnlyOptimal, input_size,
                           vk::Format::eR16G16Sfloat,
                           vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage);
        }
        depth = Texture(t->depth, t->depth_view, vk::ImageLayout::eGeneral, input_size,
                        t->depth_format,
                        vk::ImageUsageFlagBits::eSampled |
                            vk::ImageUsageFlagBits::eDepthStencilAttachment);
    }
    const auto aux_life = t ? sl::ResourceLifecycle::eValidUntilEvaluate
                            : sl::ResourceLifecycle::eValidUntilPresent;
    const sl::Extent in_extent{0, 0, input_size.width, input_size.height};
    const sl::Extent out_extent{0, 0, output_size.width, output_size.height};
    const sl::ResourceTag tags[] = {
        sl::ResourceTag{&color_in, sl::kBufferTypeScalingInputColor,
                        sl::ResourceLifecycle::eValidUntilEvaluate, &in_extent},
        sl::ResourceTag{&color_out, sl::kBufferTypeScalingOutputColor,
                        sl::ResourceLifecycle::eValidUntilEvaluate, &out_extent},
        sl::ResourceTag{&mvec, sl::kBufferTypeMotionVectors, aux_life, &in_extent},
        sl::ResourceTag{&depth, sl::kBufferTypeDepth, aux_life, &in_extent},
    };
    auto* command_buffer =
        reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(cmdbuf));
    g_sl.set_tag(*frame, viewport, tags, static_cast<u32>(std::size(tags)), command_buffer);
    if (static bool logged = false; !logged) {
        logged = true;
        LOG_WARNING(Render_Vulkan, "ryGT DLSS: Streamline input {} -> output {}",
                    vk::to_string(input_format), vk::to_string(output_format));
    }
    const sl::BaseStructure* inputs[] = {&viewport};
    const sl::Result result = g_sl.evaluate(sl::kFeatureDLSS, *frame, inputs,
                                            static_cast<u32>(std::size(inputs)), command_buffer);
    if (result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "ryGT DLSS: Streamline's DLSS failed (result {}){}",
                  static_cast<int>(result), t ? " with the game's depth and motion vectors" : "");
        return false;
    }
    return true;
    };
    bool ok = false;
    if (use_temporal) {
        ok = run(temporal);
        if (!ok) {
            temporal_refused = true;
            LOG_ERROR(Render_Vulkan, "DLSS-GT7-001: the temporal inputs were refused; DLSS runs "
                                     "on single frames for the rest of this run");
        }
    }
    const bool ran_temporal = ok;
    if (!ok) {
        ok = run(nullptr);
    }
    if (ok) {
        if (ran_temporal != last_temporal && RytyUpscale::TemporalRequested()) {
            LOG_WARNING(Render_Vulkan,
                        "DLSS-GT7-001: DLSS now runs {} ({} temporal frames, {} single so far)",
                        ran_temporal ? "with the game's depth, motion vectors and jitter"
                                     : "on single frames",
                        temporal_frames, flat_frames);
        }
        ++(ran_temporal ? temporal_frames : flat_frames);
        last_temporal = ran_temporal;
        RytyUpscale::TemporalStatus().store(ran_temporal ? 1 : 0, std::memory_order_relaxed);
    }
    return ok;
}

void PresentStart() {
    if (Active()) {
        std::scoped_lock lock{g_sl.mutex};
        Marker(sl::PCLMarker::ePresentStart);
    }
}

void PresentEnd() {
    if (Active()) {
        std::scoped_lock lock{g_sl.mutex};
        Marker(sl::PCLMarker::ePresentEnd);
    }
}

void Shutdown() {
    std::scoped_lock lock{g_sl.mutex};
    g_sl.fg.Destroy();
    g_sl.sr.Destroy();
    g_sl.sr_in = g_sl.sr_out = {};
    g_sl.sr_mode = -1;
    g_sl.sr_options_set = false;
}

} // namespace Vulkan::RytyDlssG
