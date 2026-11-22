// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: fixed_function.h, rhi.h, 3dbrew GPU/Internal Registers and gpu_fbops_test black-box vectors.

#include "fixed_function.h"

namespace mh4n::gpu::fbops {
namespace {

using namespace rhi;

constexpr std::array kLogicOps{
    LogicOp::Clear, LogicOp::And, LogicOp::AndReverse, LogicOp::Copy,
    LogicOp::Set, LogicOp::CopyInverted, LogicOp::NoOp, LogicOp::Invert,
    LogicOp::Nand, LogicOp::Or, LogicOp::Nor, LogicOp::Xor, LogicOp::Equiv,
    LogicOp::AndInverted, LogicOp::OrReverse, LogicOp::OrInverted,
};
constexpr std::array kComparisons{
    CompareOp::Never, CompareOp::Always, CompareOp::Equal, CompareOp::NotEqual,
    CompareOp::Less, CompareOp::LessEqual, CompareOp::Greater, CompareOp::GreaterEqual,
};

BlendOp Equation(u32 field) {
    return field <= static_cast<u32>(BlendOp::Max) ? static_cast<BlendOp>(field) : BlendOp::Add;
}

BlendFactor Factor(u32 field) {
    return field <= static_cast<u32>(BlendFactor::SrcAlphaSaturate)
        ? static_cast<BlendFactor>(field) : BlendFactor::One;
}

bool AllowsDepthStencilWrite(const pica::PicaState& state) {
    return (state.regs[0x115] & 3) != 0;
}

}  // namespace

rhi::FixedFunctionState MakeFixedFunctionState(const pica::PicaState& state) {
    const auto& regs = state.regs;
    FixedFunctionState result;
    const u32 culling = regs[0x40] & 3;
    if (culling == 1 || culling == 2) {
        result.cull = (regs[0x11e] & (1u << 24)) == 0 ? CullMode::Front : CullMode::Back;
        result.front_face = culling == 1 ? FrontFace::CounterClockwise : FrontFace::Clockwise;
    }

    const u32 merger = regs[0x100];
    if ((merger & 3) == 3) {
        return result;
    }

    if (merger & (1u << 8)) {
        const u32 blend = regs[0x101];
        result.color_op = Equation(blend & 7);
        result.alpha_op = Equation((blend >> 8) & 7);
        result.src_color = Factor((blend >> 16) & 15);
        result.dst_color = Factor((blend >> 20) & 15);
        result.src_alpha = Factor((blend >> 24) & 15);
        result.dst_alpha = Factor(blend >> 28);
        result.blend_enabled = result.color_op != BlendOp::Add || result.alpha_op != BlendOp::Add ||
                               result.src_color != BlendFactor::One || result.src_alpha != BlendFactor::One ||
                               result.dst_color != BlendFactor::Zero || result.dst_alpha != BlendFactor::Zero;
    } else {
        const LogicOp op = kLogicOps[regs[0x102] & 15];
        if (op != LogicOp::Copy) {
            result.logic_op_enabled = true;
            result.logic_op = op;
        }
    }

    const u32 depth_color = regs[0x107];
    result.color_write_mask = (regs[0x113] & 15) != 0 ? (depth_color >> 8) & 15 : 0;
    const bool depth_test = (depth_color & 1) != 0;
    const bool depth_write = (depth_color & (1u << 12)) != 0;
    result.depth_test = depth_test || depth_write;
    result.depth_write = depth_write && AllowsDepthStencilWrite(state);
    if (result.depth_test) {
        result.depth_compare = depth_test ? kComparisons[(depth_color >> 4) & 7] : CompareOp::Always;
    }

    const u32 stencil = regs[0x105];
    result.stencil_test = (stencil & 1) != 0 && (regs[0x116] & 3) == 3;
    if (result.stencil_test) {
        const u32 actions = regs[0x106];
        auto& face = result.stencil_front;
        face.compare = kComparisons[(stencil >> 4) & 7];
        face.fail = static_cast<StencilOp>(actions & 7);
        face.depth_fail = static_cast<StencilOp>((actions >> 4) & 7);
        face.pass = static_cast<StencilOp>((actions >> 8) & 7);
        result.stencil_back = face;
    }
    return result;
}

DynamicState MakeDynamicState(const pica::PicaState& state) {
    DynamicState result;
    const u32 color = state.regs[0x103];
    for (u32 lane = 0; lane < result.blend_constants.size(); ++lane) {
        result.blend_constants[lane] = static_cast<float>((color >> (lane * 8)) & 255) / 255.0f;
    }
    const u32 stencil = state.regs[0x105];
    result.stencil_reference = (stencil >> 16) & 255;
    result.stencil_compare_mask = stencil >> 24;
    if (AllowsDepthStencilWrite(state)) {
        result.stencil_write_mask = (stencil >> 8) & 255;
    }
    return result;
}

void ApplyDynamicState(rhi::Recorder& recorder, const DynamicState& dynamic, bool constants_from_alpha) {
    auto constants = dynamic.blend_constants;
    if (constants_from_alpha) {
        constants.fill(constants[3]);
    }
    recorder.SetBlendConstants(constants);
    recorder.SetStencilReference(dynamic.stencil_reference);
    recorder.SetStencilMasks(dynamic.stencil_compare_mask, dynamic.stencil_write_mask);
}

}  // namespace mh4n::gpu::fbops

