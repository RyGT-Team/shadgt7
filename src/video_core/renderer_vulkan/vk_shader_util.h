// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/**
 * @brief Creates a vulkan shader module from SPIR-V bytecode.
 * @param code The SPIR-V bytecode data.
 * @param device The vulkan device handle
 */
vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device, bool keep_code = false);

std::vector<u32> ModuleCode(vk::ShaderModule module);

/// FIX-GT7-186: a number for the module CompileSPV made, new for every module it makes, so a
/// handle the driver hands out again after a module is destroyed never names the old one. The
/// shared stage libraries are keyed by it (they live until exit, and FIX-GT7-186 destroys the
/// precompile's unused modules). Modules made elsewhere: their handle, with the top bit set.
u64 ModuleIdentity(vk::ShaderModule module);
/// FIX-GT7-186: forgets a CompileSPV module's kept code and identity, before it is destroyed.
void ForgetModule(vk::ShaderModule module);

} // namespace Vulkan
