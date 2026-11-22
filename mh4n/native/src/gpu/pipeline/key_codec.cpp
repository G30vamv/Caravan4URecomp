// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: key_codec.h, pipeline_key.h, gpu_pipeline_test and byte output of the shipped codec.

#include "key_codec.h"

#include <cstring>

#include "cache_bytes.h"

namespace mh4n::gpu::pipeline {
namespace {

void EncodeState(const rhi::FixedFunctionState& s, std::vector<u8>& out) {
    out.insert(out.end(), {
        s.blend_enabled, static_cast<u8>(s.src_color), static_cast<u8>(s.dst_color), static_cast<u8>(s.color_op),
        static_cast<u8>(s.src_alpha), static_cast<u8>(s.dst_alpha), static_cast<u8>(s.alpha_op), s.color_write_mask,
        s.logic_op_enabled, static_cast<u8>(s.logic_op), s.depth_test, s.depth_write, static_cast<u8>(s.depth_compare),
        s.stencil_test,
    });
    for (const auto* face : {&s.stencil_front, &s.stencil_back}) {
        out.insert(out.end(), {
            static_cast<u8>(face->fail), static_cast<u8>(face->pass), static_cast<u8>(face->depth_fail),
            static_cast<u8>(face->compare),
        });
    }
    out.insert(out.end(), {static_cast<u8>(s.cull), static_cast<u8>(s.front_face), s.depth_bias_enabled});
    disk::Append(out, std::bit_cast<u32>(s.depth_bias_constant));
    disk::Append(out, std::bit_cast<u32>(s.depth_bias_slope));
}

rhi::FixedFunctionState DecodeState(disk::Reader& in) {
    rhi::FixedFunctionState s;
    s.blend_enabled = in.Flag();
    s.src_color = in.Enum<rhi::BlendFactor::SrcAlphaSaturate>();
    s.dst_color = in.Enum<rhi::BlendFactor::SrcAlphaSaturate>();
    s.color_op = in.Enum<rhi::BlendOp::Max>();
    s.src_alpha = in.Enum<rhi::BlendFactor::SrcAlphaSaturate>();
    s.dst_alpha = in.Enum<rhi::BlendFactor::SrcAlphaSaturate>();
    s.alpha_op = in.Enum<rhi::BlendOp::Max>();
    s.color_write_mask = in.Read<u8>();
    s.logic_op_enabled = in.Flag();
    s.logic_op = in.Enum<rhi::LogicOp::OrInverted>();
    s.depth_test = in.Flag();
    s.depth_write = in.Flag();
    s.depth_compare = in.Enum<rhi::CompareOp::Always>();
    s.stencil_test = in.Flag();
    for (auto* face : {&s.stencil_front, &s.stencil_back}) {
        face->fail = in.Enum<rhi::StencilOp::DecrementWrap>();
        face->pass = in.Enum<rhi::StencilOp::DecrementWrap>();
        face->depth_fail = in.Enum<rhi::StencilOp::DecrementWrap>();
        face->compare = in.Enum<rhi::CompareOp::Always>();
    }
    s.cull = in.Enum<rhi::CullMode::Back>();
    s.front_face = in.Enum<rhi::FrontFace::CounterClockwise>();
    s.depth_bias_enabled = in.Flag();
    s.depth_bias_constant = std::bit_cast<float>(in.Read<u32>());
    s.depth_bias_slope = std::bit_cast<float>(in.Read<u32>());
    return s;
}

}  // namespace

void EncodeKey(const native::PipelineKey& key, std::vector<u8>& out) {
    const auto& input = key.vertex.input;
    out.insert(out.end(), {static_cast<u8>(key.pass), static_cast<u8>(key.vertex.kind)});
    disk::Append(out, key.vertex.program);
    out.insert(out.end(), {input.binding_count, input.attribute_count});
    for (u32 i = 0; i < input.binding_count; ++i) {
        disk::Append(out, input.bindings[i].stride);
        out.push_back(input.bindings[i].per_instance);
    }
    for (u32 i = 0; i < input.attribute_count; ++i) {
        const auto& attr = input.attributes[i];
        out.insert(out.end(), {attr.location, attr.binding, static_cast<u8>(attr.format)});
        disk::Append(out, attr.offset);
    }
    out.insert(out.end(), {
        static_cast<u8>(key.fixup), static_cast<u8>(key.fragment.kind), static_cast<u8>(key.fragment.uber),
    });
    if (key.fragment.kind == native::FragmentKind::Specialised) {
        const auto* bytes = reinterpret_cast<const u8*>(&key.fragment.key);
        out.insert(out.end(), bytes, bytes + sizeof(key.fragment.key));
    }
    EncodeState(key.state, out);
    out.insert(out.end(), {static_cast<u8>(key.topology), static_cast<u8>(key.color_format), static_cast<u8>(key.depth_format)});
}

std::vector<u8> EncodeKey(const native::PipelineKey& key) {
    std::vector<u8> bytes;
    EncodeKey(key, bytes);
    return bytes;
}

std::optional<native::PipelineKey> DecodeKey(std::span<const u8> bytes) {
    disk::Reader in(bytes);
    native::PipelineKey key;
    key.pass = in.Enum<native::PassKind::ShadowWrite>();
    key.vertex.kind = in.Enum<native::VertexStageKind::QuaternionPass>();
    key.vertex.program = in.Read<u64>();
    auto& input = key.vertex.input;
    input.binding_count = in.Read<u8>();
    input.attribute_count = in.Read<u8>();
    if (input.binding_count > input.bindings.size() || input.attribute_count > input.attributes.size()) {
        return std::nullopt;
    }
    for (u32 i = 0; i < input.binding_count; ++i) {
        input.bindings[i].stride = in.Read<u32>();
        input.bindings[i].per_instance = in.Read<u8>();
    }
    for (u32 i = 0; i < input.attribute_count; ++i) {
        auto& attr = input.attributes[i];
        attr.location = in.Read<u8>();
        attr.binding = in.Read<u8>();
        attr.format = in.Enum<rhi::Format::Rgba16Sint>();
        attr.offset = in.Read<u32>();
    }
    key.fixup = in.Enum<native::FixupStage::FixedGeometry>();
    key.fragment.kind = in.Enum<native::FragmentKind::Uber>();
    key.fragment.uber = in.Enum<native::UberModule::ShadowWrite>();
    if (key.fragment.kind == native::FragmentKind::Specialised) {
        const auto fragment = in.Take(sizeof(key.fragment.key));
        if (fragment.empty()) {
            return std::nullopt;
        }
        std::memcpy(&key.fragment.key, fragment.data(), fragment.size());
    }
    key.state = DecodeState(in);
    key.topology = in.Enum<rhi::Topology::TriangleFan>();
    key.color_format = in.Enum<rhi::Format::Rgba16Sint>();
    key.depth_format = in.Enum<rhi::Format::Rgba16Sint>();
    return in.Done() ? std::optional{key} : std::nullopt;
}

}  // namespace mh4n::gpu::pipeline
