// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include "common/serdes.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

// Set before cache readers/workers start; candidate blobs are ignored on other profiles.
void SetSingleSampleBarycentricCacheEnabled(bool enabled);
void SetVertexOrderCheckCacheEnabled(bool enabled); // AMD-307
void SetNativeCubeCacheEnabled(bool enabled);       // FIX-GT7-083

/// FEAT-GT7-185 (from shadGT PERF-038): the pipeline records the start-up precompile builds from.
void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata);
void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata);
void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash, size_t perm_idx);
void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx);
/// FIX-GT7-088: `other_cube_sampling` is set when a whole entry was refused only because the other
/// engine's cube sampling made it (FIX-GT7-083): its index is that translation's, not a free one.
bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx,
                    bool* other_cube_sampling = nullptr);
/// FEAT-GT7-185 (PERF-038): the program hash and permutation index of stored shader metadata, without decoding
/// the rest (decoding registers SRT walker code, which only the GPU thread may do).
bool PeekShaderMeta(std::vector<u8>&& blob, u64& pgm_hash, size_t& perm_idx);
/// FEAT-GT7-189 (from shadGT FEAT-GTS-471): writes a stored graphics pipeline file at once (see
/// RegisterPipelineData), not through the store's queue, so a listing right after it sees it.
bool WriteStoredGraphicsPipeline(const std::filesystem::path& file, const GraphicsPipelineKey& key,
                                 GraphicsPipeline::SerializationSupport& sdata);
/// The current pipeline record version and the key's size, for pipeline recipe files.
u32 StoredPipelineKeyVersion();
/// FEAT-GT7-185 (PERF-038): a stored graphics pipeline of the current key version (others are skipped).
bool LoadStoredGraphicsPipeline(std::vector<u8>&& blob, GraphicsPipelineKey& key,
                                GraphicsPipeline::SerializationSupport& sdata);

} // namespace Vulkan
