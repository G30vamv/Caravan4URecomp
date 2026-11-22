// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: native_gpu.h, shader/vertex_shader.h, vertex/vertex_loader.h and the RHI recording contract.
#include "renderer.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "mh4n/log.h"

namespace mh4n::gpu::native {
namespace {

struct Primitives {
    u32 topology;
    u32 corners;
};

Primitives Triangles(const vertex::LoadedDraw& draw) {
    const u32 count = draw.count;
    switch (draw.topology) {
    case vertex::Topology::TriangleList: return {shader::kPullTriangleList, count - count % 3};
    case vertex::Topology::TriangleStrip: return {shader::kPullTriangleStrip, count > 2 ? (count - 2) * 3 : 0};
    case vertex::Topology::TriangleFan: return {shader::kPullTriangleFan, count > 2 ? (count - 2) * 3 : 0};
    default: return {};
    }
}

u32 IndexForm(vertex::IndexType type) {
    switch (type) {
    case vertex::IndexType::U8: return shader::kPullIndexU8;
    case vertex::IndexType::U16: return shader::kPullIndexU16;
    case vertex::IndexType::U32: return shader::kPullIndexU32;
    default: return shader::kPullIndexNone;
    }
}

u32 OriginalVertex(const vertex::LoadedDraw& draw, u32 slot, u32 form) {
    if (form == shader::kPullIndexNone) return slot;
    const u32 size = 1u << (form - 1);
    const u8* bytes = draw.index_data.data() + size_t{slot} * size;
    u32 value = 0;
    for (u32 byte = 0; byte < size; ++byte) value |= u32{bytes[byte]} << (byte * 8);
    return value + static_cast<u32>(draw.base_vertex);
}

u32 SlotOf(u32 topology, u32 corner) {
    const u32 triangle = corner / 3;
    const u32 slot = corner % 3;
    if (topology == shader::kPullTriangleStrip) {
        return triangle + ((triangle & 1) && slot ? 3 - slot : slot);
    }
    if (topology == shader::kPullTriangleFan) return slot == 2 ? 0 : triangle + slot + 1;
    return corner;
}

u32 Groups(u32 items) {
    return (items + shader::kPullComputeGroup - 1) / shader::kPullComputeGroup;
}

} // namespace

bool Renderer::PreparePull(DrawDesc& draw, const pica::DrawCall& source, rhi::Recorder& recorder) {
    using namespace shader;
    const Primitives primitives = Triangles(loaded_);
    const u32 vertices = source.kind == pica::DrawKind::Indexed
        ? loaded_.list_high - loaded_.list_low + 1 : source.num_vertices;
    if (!primitives.corners || !vertices || draw.vertex_buffers.size() > kPullHeaderWords) return false;
    const u32 first_groups = Groups(vertices), second_groups = Groups(primitives.corners);
    if (first_groups > 65535 || second_groups > 65535) return false;
    const u32 groups = std::max(first_groups, second_groups);
    const u64 words = kPullHeaderWords + kPullJobWords + u64{groups} + u64{vertices} * 4 + primitives.corners;
    if (words * sizeof(u32) > vertices_->Size()) return false;

    PipelineKey quaternion_key{};
    quaternion_key.vertex.kind = VertexStageKind::QuaternionPass;
    quaternion_key.vertex.program = draw.vertex.program;
    auto quaternion = provider_->Lookup(quaternion_key);
    if (!quaternion || quaternion->pending || !quaternion->pipeline) return false;

    if (!flip_pipeline_) {
        std::vector<u32> spirv;
        std::string error;
        if (!CompileGlslToSpirv(FlipPassGlsl(), spirv, error, nullptr, true)) {
            MH4N_LOG(Warning, "NativeGpu", "Quaternion flip shader: {}", error);
            return false;
        }
        rhi::ComputePipelineDesc desc{};
        desc.shader.spirv = spirv;
        desc.sets = FixupPassSets();
        desc.name = "PICA quaternion flip";
        auto pipeline = backend_->CreateComputePipeline(desc);
        if (!pipeline) return false;
        flip_pipeline_ = std::move(*pipeline);
    }

    auto storage = vertices_->Map(words * sizeof(u32), 16);
    if (!storage) return false;
    const u32 header_start = static_cast<u32>(storage->offset / 4);
    const u32 job_start = header_start + kPullHeaderWords;
    const u32 group_start = job_start + kPullJobWords;
    const u32 quaternions_start = group_start + groups;
    const u32 indices_start = quaternions_start + 4 * vertices;
    const u32 form = IndexForm(loaded_.index_type);

    std::array<u32, kPullHeaderWords> header{};
    for (size_t i = 0; i < draw.vertex_buffers.size(); ++i) {
        header[i] = static_cast<u32>(draw.vertex_buffers[i].offset);
    }
    std::array<u32, kPullJobWords> job{};
    job[kPullJobUniforms] = static_cast<u32>(draw.vs_pica_uniforms.offset / 4);
    job[kPullJobHeader] = header_start;
    job[kPullJobVertices] = vertices;
    job[kPullJobQuaternions] = quaternions_start;
    job[kPullJobIndexList] = static_cast<u32>(draw.index_offset / 4);
    job[kPullJobCornerCount] = primitives.corners;
    job[kPullJobIndices] = indices_start;
    job[kPullJobIndexForm] = form | (primitives.topology << 2);
    job[kPullJobBaseVertex] = static_cast<u32>(loaded_.base_vertex);
    auto* bytes = storage->data.data();
    std::memcpy(bytes, header.data(), sizeof(header));
    std::memcpy(bytes + sizeof(header), job.data(), sizeof(job));
    // All the workgroups in each dispatch use job 0. Seed the indices for drivers that inspect the indexed range.
    std::memset(bytes + sizeof(header) + sizeof(job), 0, u64{groups} * sizeof(u32));
    auto* indices = bytes + u64{indices_start - header_start} * sizeof(u32);
    for (u32 i = 0; i < primitives.corners; ++i) {
        const u32 index = 2 * OriginalVertex(loaded_, SlotOf(primitives.topology, i), form);
        std::memcpy(indices + u64{i} * sizeof(u32), &index, sizeof(index));
    }
    if (!vertices_->Commit(words * sizeof(u32))) return false;

    const std::array<u32, 4> params{job_start, group_start, 0, 0};
    auto uniform = uniforms_->Map(sizeof(params), backend_->Caps().uniform_buffer_offset_alignment);
    if (!uniform) return false;
    std::memcpy(uniform->data.data(), params.data(), sizeof(params));
    if (!uniforms_->Commit(sizeof(params))) return false;

    auto set = fixup_sets_->Acquire(FixupPassSets().front());
    if (!set) return false;
    const auto& uniform_buffer = *uniforms_->GetBuffer();
    const auto& vertex_buffer = *vertices_->GetBuffer();
    if (!(*set)->SetStorageBuffer(kBindingPassUniformWords, uniform_buffer, 0, uniform_buffer.Size()) ||
        !(*set)->SetUniformBuffer(kBindingPassParams, uniform_buffer, 0, sizeof(params)) ||
        !(*set)->SetStorageBuffer(kBindingPullWords, vertex_buffer, 0, vertex_buffer.Size())) {
        return false;
    }
    const std::array dynamic_offsets{static_cast<u32>(uniform->offset)};
    recorder.BeginPrelude();
    recorder.SetPipeline(*quaternion->pipeline);
    recorder.Bind(**set, 0, dynamic_offsets);
    recorder.Dispatch(first_groups, 1, 1);
    recorder.Barrier(rhi::Stage::ComputeShader, rhi::Stage::ComputeShader);
    recorder.SetPipeline(*flip_pipeline_);
    recorder.Bind(**set, 0, dynamic_offsets);
    recorder.Dispatch(second_groups, 1, 1);
    recorder.EndPrelude();
    recorded_quaternions_.push_back(std::move(quaternion->pipeline));

    draw.pull_words = &vertex_buffer;
    draw.vertex_buffers = {};
    draw.first_instance = header_start;
    draw.index_buffer = &vertex_buffer;
    draw.index_type = rhi::IndexType::U32;
    draw.index_offset = u64{indices_start} * sizeof(u32);
    draw.count = primitives.corners;
    draw.base_vertex = 0;
    return true;
}

} // namespace mh4n::gpu::native
