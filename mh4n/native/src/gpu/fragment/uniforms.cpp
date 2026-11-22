// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: uniforms.h, regs.h, the gpu_fragment_key_test black-box cases and golden/gpu/glsl.

#include "uniforms.h"

#include <algorithm>
#include <array>

#include "feature.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {
namespace {

constexpr std::array kTevColors{
    freg::kTevStage0 + freg::kTevConstColor, freg::kTevStage1 + freg::kTevConstColor,
    freg::kTevStage2 + freg::kTevConstColor, freg::kTevStage3 + freg::kTevConstColor,
    freg::kTevStage4 + freg::kTevConstColor, freg::kTevStage5 + freg::kTevConstColor,
};

UniformContext::Scissor ReadScissor(const pica::PicaState& state, int scale) {
    const u32 first = state.regs[freg::kScissorXY1];
    const u32 last = state.regs[freg::kScissorXY2];
    return {
        static_cast<int>(Bits(first, 0, 10)) * scale,
        static_cast<int>(Bits(first, 16, 10)) * scale,
        (static_cast<int>(Bits(last, 0, 10)) + 1) * scale,
        (static_cast<int>(Bits(last, 16, 10)) + 1) * scale,
    };
}

} // namespace

Vec4f ColorRGBA8(u32 word) {
    return {
        static_cast<float>(Bits(word, 0, 8)) / 255.0f,
        static_cast<float>(Bits(word, 8, 8)) / 255.0f,
        static_cast<float>(Bits(word, 16, 8)) / 255.0f,
        static_cast<float>(Bits(word, 24, 8)) / 255.0f,
    };
}

void FillCoreUniforms(const pica::PicaState& state, FragmentUniforms& uniforms, UniformDirty& dirty,
                      const UniformContext& context) {
    const auto& regs = state.regs;
    const auto& changed = state.dirty;
    if (dirty.all || changed.Test(freg::kTevCombinerBufferColor)
        || std::ranges::any_of(kTevColors, [&](u32 reg) { return changed.Test(reg); })) {
        for (size_t i = 0; i < kTevColors.size(); ++i) {
            uniforms.const_color[i] = ColorRGBA8(regs[kTevColors[i]]);
        }
        uniforms.tev_combiner_buffer_color = ColorRGBA8(regs[freg::kTevCombinerBufferColor]);
        dirty.Mark();
    }
    if (dirty.all || changed.Test(freg::kAlphaTest) || changed.Test(freg::kBlendConst)) {
        uniforms.alphatest_ref = static_cast<int>(Bits(regs[freg::kAlphaTest], 8, 8));
        uniforms.blend_color = ColorRGBA8(regs[freg::kBlendConst]);
        dirty.Mark();
    }
    if (dirty.all || changed.Test(freg::kViewportDepthRange) || changed.Test(freg::kViewportDepthNearPlane)) {
        uniforms.depth_scale = pica::Float24ToFloat(Bits(regs[freg::kViewportDepthRange], 0, 24));
        uniforms.depth_offset = pica::Float24ToFloat(Bits(regs[freg::kViewportDepthNearPlane], 0, 24));
        dirty.Mark();
    }

    const int scale = context.framebuffer_scale;
    const auto rect = context.scissor ? *context.scissor : ReadScissor(state, scale);
    const UniformContext::Scissor previous{
        uniforms.scissor_x1, uniforms.scissor_y1, uniforms.scissor_x2, uniforms.scissor_y2,
    };
    if (dirty.all || scale != uniforms.framebuffer_scale || rect != previous) {
        uniforms.framebuffer_scale = scale;
        uniforms.scissor_x1 = rect.x1;
        uniforms.scissor_y1 = rect.y1;
        uniforms.scissor_x2 = rect.x2;
        uniforms.scissor_y2 = rect.y2;
        dirty.Mark();
    }
}

void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty,
                  const UniformContext& context) {
    FillUniforms(state, block, dirty, context, FeatureRegistry::Global());
}

void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty,
                  const UniformContext& context, const FeatureRegistry& features) {
    FillCoreUniforms(state, block.uniforms, dirty, context);
    for (const auto* feature : features.All()) {
        feature->FillUniforms(state, block, dirty);
    }
}

bool StoreKey(const FragmentKey& key, FragmentBlock& block) {
    if (key == block.cfg) {
        return false;
    }
    block.cfg = key;
    return true;
}

} // namespace mh4n::gpu::fragment
