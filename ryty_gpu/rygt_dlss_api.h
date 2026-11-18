// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: MIT
//
// The interface between ryGT's renderer (libSceVideoOut.prx) and the optional DLSS plugin
// (dlss\ryGT_dlss.dll next to GTSport.exe). The plugin links NVIDIA's DLSS SDK, whose license does
// not allow it inside the GPL renderer, so the renderer only loads the plugin at run time when the
// DLSS option is on and the plugin is present. Plain C; Vulkan handles are passed as is.
//
// The renderer hands DLSS the finished game picture (no motion vectors or depth exist at that
// point), so the plugin runs DLSS with zero motion vectors and resets its history every frame.

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RYGT_DLSS_API_VERSION 1

typedef struct RygtDlssImage {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} RygtDlssImage;

// Returns RYGT_DLSS_API_VERSION.
typedef int(VKAPI_PTR* PFN_RygtDlss_ApiVersion)(void);

// Instance extensions DLSS needs; call before creating the VkInstance. The list stays valid until
// the plugin is unloaded. Returns 0 on success.
typedef int(VKAPI_PTR* PFN_RygtDlss_GetInstanceExtensions)(uint32_t* count, const char* const** names);

// Device extensions DLSS needs on this physical device; call before creating the VkDevice.
typedef int(VKAPI_PTR* PFN_RygtDlss_GetDeviceExtensions)(VkInstance instance,
                                                         VkPhysicalDevice physical_device,
                                                         PFN_vkGetInstanceProcAddr gipa,
                                                         uint32_t* count, const char* const** names);

// Starts DLSS on the device. data_path receives NVIDIA's logs. Returns 0 when DLSS is available.
typedef int(VKAPI_PTR* PFN_RygtDlss_Init)(VkInstance instance, VkPhysicalDevice physical_device,
                                          VkDevice device, PFN_vkGetInstanceProcAddr gipa,
                                          PFN_vkGetDeviceProcAddr gdpa, const wchar_t* data_path);

// Whether DLSS can scale input_width x input_height to output_width x output_height (an output at
// least as large as the input, within the range one of DLSS's quality modes accepts).
typedef int(VKAPI_PTR* PFN_RygtDlss_Supports)(uint32_t input_width, uint32_t input_height,
                                              uint32_t output_width, uint32_t output_height);

// Records DLSS into cmd: input (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) scaled into output
// (VK_IMAGE_LAYOUT_GENERAL, created with VK_IMAGE_USAGE_STORAGE_BIT). sharpness is 0..1.
// Returns 0 on success; on failure nothing usable was written to output.
typedef int(VKAPI_PTR* PFN_RygtDlss_Evaluate)(VkCommandBuffer cmd, const RygtDlssImage* input,
                                              const RygtDlssImage* output, float sharpness);

// Releases DLSS. The device must be idle.
typedef void(VKAPI_PTR* PFN_RygtDlss_Shutdown)(void);

// The last error, for the log.
typedef const char*(VKAPI_PTR* PFN_RygtDlss_LastError)(void);

#ifdef __cplusplus
}
#endif
