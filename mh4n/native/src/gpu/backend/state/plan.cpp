// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: state.h, rhi.h, gpu_fbops_test.cpp and the Vulkan framebuffer operations specification.

#include "state.h"

namespace mh4n::gpu::rhi::state {
namespace {

bool IsMinMax(BlendOp op) {
    return op == BlendOp::Min || op == BlendOp::Max;
}

bool NeedsMinMaxShader(BlendOp op, BlendFactor src, BlendFactor dst) {
    return IsMinMax(op) && (src != BlendFactor::One || dst != BlendFactor::One);
}

bool ReadsSourceAlpha(BlendFactor factor) {
    return factor == BlendFactor::SrcAlpha || factor == BlendFactor::OneMinusSrcAlpha ||
           factor == BlendFactor::SrcAlphaSaturate;
}

bool IsConstantColor(BlendFactor factor) {
    return factor == BlendFactor::ConstantColor || factor == BlendFactor::OneMinusConstantColor;
}

bool IsConstantAlpha(BlendFactor factor) {
    return factor == BlendFactor::ConstantAlpha || factor == BlendFactor::OneMinusConstantAlpha;
}

BlendFactor RewriteConstantAlpha(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::ConstantAlpha: return BlendFactor::ConstantColor;
    case BlendFactor::OneMinusConstantAlpha: return BlendFactor::OneMinusConstantColor;
    default: return factor;
    }
}

}  // namespace

bool LogicOpNeedsDestination(LogicOp op) {
    switch (op) {
    case LogicOp::Clear:
    case LogicOp::Set:
    case LogicOp::Copy:
    case LogicOp::CopyInverted:
    case LogicOp::NoOp:
        return false;
    default:
        return true;
    }
}

Result<PipelinePlan> PlanPipeline(const FixedFunctionState& requested, const Capabilities& caps) {
    PipelinePlan plan{requested};
    FixedFunctionState& built = plan.built;
    if (requested.logic_op_enabled) {
        if (requested.blend_enabled) {
            return Error{"Blending and logic operations cannot both be enabled"};
        }
        if (!caps.logic_op) {
            built.logic_op_enabled = false;
            if (requested.logic_op == LogicOp::NoOp) {
                built.color_write_mask = 0;
            } else {
                plan.shader_logic_op = true;
                plan.needs_destination_copy = LogicOpNeedsDestination(requested.logic_op);
            }
        }
        return plan;
    }
    if (!requested.blend_enabled) {
        return plan;
    }

    const bool rgb_constant_alpha = IsConstantAlpha(requested.src_color) || IsConstantAlpha(requested.dst_color);
    const bool rgb_constant_color = IsConstantColor(requested.src_color) || IsConstantColor(requested.dst_color);
    plan.alpha_in_shader = NeedsMinMaxShader(requested.alpha_op, requested.src_alpha, requested.dst_alpha);
    plan.rgb_in_shader = NeedsMinMaxShader(requested.color_op, requested.src_color, requested.dst_color) ||
                         (plan.alpha_in_shader &&
                          (ReadsSourceAlpha(requested.src_color) || ReadsSourceAlpha(requested.dst_color))) ||
                         (rgb_constant_alpha && rgb_constant_color);
    plan.rgb_uses_blend_rgb = plan.rgb_in_shader && !IsMinMax(requested.color_op);
    plan.needs_destination_copy = plan.rgb_in_shader || plan.alpha_in_shader;

    if (plan.rgb_in_shader) {
        built.color_op = BlendOp::Add;
        built.src_color = BlendFactor::One;
        built.dst_color = BlendFactor::Zero;
    } else {
        plan.constants_from_alpha = rgb_constant_alpha;
        built.src_color = RewriteConstantAlpha(requested.src_color);
        built.dst_color = RewriteConstantAlpha(requested.dst_color);
    }
    if (plan.alpha_in_shader) {
        built.alpha_op = BlendOp::Add;
        built.src_alpha = BlendFactor::One;
        built.dst_alpha = BlendFactor::Zero;
    } else {
        built.src_alpha = RewriteConstantAlpha(requested.src_alpha);
        built.dst_alpha = RewriteConstantAlpha(requested.dst_alpha);
    }
    return plan;
}

std::array<float, 4> EffectiveBlendConstants(bool constants_from_alpha, const std::array<float, 4>& requested) {
    if (constants_from_alpha) {
        const float alpha = requested[3];
        return {alpha, alpha, alpha, alpha};
    }
    return requested;
}

std::array<float, 4> EffectiveBlendConstants(const PipelinePlan& plan, const std::array<float, 4>& requested) {
    return EffectiveBlendConstants(plan.constants_from_alpha, requested);
}

}  // namespace mh4n::gpu::rhi::state
