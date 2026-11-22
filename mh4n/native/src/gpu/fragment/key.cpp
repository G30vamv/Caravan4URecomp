// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: fragment headers, gpu_fragment_key_test, gpu_lighting_test, 3dbrew GPU/Internal Registers, libctru GPU enums and citro3d lighting register writes.

#include "key.h"

#include <algorithm>
#include <atomic>

#include "feature.h"
#include "generator.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {
namespace {

using RegisterFile = decltype(pica::PicaState::regs);

constexpr std::array kTevStageRegisters{
    freg::kTevStage0, freg::kTevStage1, freg::kTevStage2,
    freg::kTevStage3, freg::kTevStage4, freg::kTevStage5,
};
constexpr std::array kTextureRegisters{freg::kTexture0, freg::kTexture1, freg::kTexture2};

std::atomic<bool> shaders_as_before{false};

BlendConfig DecodeBlend(u32 word, u32 equation_start, u32 factors_start) {
    BlendConfig blend{
        static_cast<re::BlendEquation>(Bits(word, equation_start, 3)),
        static_cast<re::BlendFactor>(Bits(word, factors_start, 4)),
        static_cast<re::BlendFactor>(Bits(word, factors_start + 4, 4)),
    };
    return blend.RequiresMinMaxEmulation() ? blend : BlendConfig{};
}

FramebufferConfig DecodeFramebuffer(const RegisterFile& regs) {
    const u32 operation = regs[freg::kFragmentOperation];
    const u32 alpha_test = regs[freg::kAlphaTest];
    FramebufferConfig result{};
    result.set_alpha_test_func(Bits(alpha_test, 0, 1) != 0
                                   ? static_cast<re::CompareFunc>(Bits(alpha_test, 4, 3))
                                   : re::CompareFunc::Always);
    result.set_scissor_test_mode(static_cast<re::ScissorMode>(Bits(regs[freg::kScissorMode], 0, 2)));
    result.set_depthmap_enable(static_cast<re::DepthBuffering>(Bits(regs[freg::kDepthmapEnable], 0, 1)));
    result.set_depth_identity(result.depthmap_enable() == re::DepthBuffering::ZBuffering
                              && Bits(regs[freg::kViewportDepthRange], 0, 24) == 0xBF0000u
                              && Bits(regs[freg::kViewportDepthNearPlane], 0, 23) == 0);
    result.set_shadow_rendering(Bits(operation, 0, 2) == static_cast<u32>(re::FragmentOperationMode::Shadow));
    result.set_logic_op(re::LogicOp::Copy);
    result.set_alphablend_enable(Bits(operation, 8, 1));
    if (result.alphablend_enable()) {
        const u32 blend = regs[freg::kAlphaBlending];
        result.requested_rgb_blend = DecodeBlend(blend, 0, 16);
        result.requested_alpha_blend = DecodeBlend(blend, 8, 24);
    } else {
        result.requested_logic_op = static_cast<re::LogicOp>(Bits(regs[freg::kLogicOp], 0, 4));
    }
    return result;
}

TextureConfig DecodeTexture(const RegisterFile& regs) {
    TextureConfig result{};
    result.set_texture0_type(static_cast<re::TextureType>(
        Bits(regs[freg::kTexture0 + freg::kTextureParams], 28, 3)));
    result.set_texture2_use_coord1(Bits(regs[freg::kMainConfig], 13, 1));
    result.set_shadow_texture_orthographic(Bits(regs[freg::kShadowTexture], 0, 1));
    const u32 fog_buffer = regs[freg::kFogModeAndCombiner];
    result.set_combiner_buffer_input(Bits(fog_buffer, 8, 8));
    result.set_fog_mode(static_cast<re::FogMode>(Bits(fog_buffer, 0, 3)));
    result.set_fog_flip(Bits(fog_buffer, 16, 1));

    for (size_t i = 0; i < kTevStageRegisters.size(); ++i) {
        const u32 base = kTevStageRegisters[i];
        TevStageConfigRaw stage{
            regs[base + freg::kTevSources],
            regs[base + freg::kTevModifiers],
            regs[base + freg::kTevOps],
            regs[base + freg::kTevScales],
        };
        if (stage.color_op() == re::TevOperation::Dot3_RGBA) {
            stage.sources_raw &= 0xFFF;
            stage.modifiers_raw &= 0xFFF;
            stage.ops_raw &= 0xF;
        }
        result.tev_stages[i] = stage;
    }
    for (u32 i = 0; i < kTextureRegisters.size(); ++i) {
        const u32 params = regs[kTextureRegisters[i] + freg::kTextureParams];
        result.requested_wrap[i] = {
            static_cast<re::WrapMode>(Bits(params, 12, 3)),
            static_cast<re::WrapMode>(Bits(params, 8, 3)),
        };
    }
    return result;
}

// Claude fix 2026-11-19 (differential oracle vs production, golden/keyfuzz): the lod_free bit follows every source slot of a stage that
// is not a pass-through (all six: three colour and three alpha), not only the operands its operation reads.
bool TevMentionsSource(const FragmentKey& key, u32 source) {
    for (const auto& stage : key.texture.tev_stages) {
        if (TevStageIsPassThrough(stage)) {
            continue;
        }
        for (u32 i = 0; i < 3; ++i) {
            if (stage.color_source(i) == source || stage.alpha_source(i) == source) {
                return true;
            }
        }
    }
    return false;
}

// Claude fix 2026-11-19 (differential oracle): the units that the lighting reads (the bump map, the shadow map) count as read too.
bool LightingReadsUnit(const FragmentKey& key, u32 unit) {
    const auto& light = key.lighting;
    if (!light.enable()) {
        return false;
    }
    return (static_cast<u32>(light.bump_mode()) != 0 && light.bump_selector() == unit) ||
           (light.enable_shadow() && light.shadow_selector() == unit);
}

void FindLodFreeUnits(const RegisterFile& regs, FragmentKey& key, bool legacy) {
    for (u32 i = 0; i < kTextureRegisters.size(); ++i) {
        const u32 base = kTextureRegisters[i];
        const u32 params = regs[base + freg::kTextureParams];
        const bool same_filter = Bits(params, 1, 1) == Bits(params, 2, 1);
        const u32 lod = regs[base + freg::kTextureLod];
        const bool base_level_only = Bits(lod, 16, 4) == 0 && Bits(lod, 24, 4) == 0;
        const bool read = same_filter && (!legacy || base_level_only) && (TevMentionsSource(key, 3 + i) || LightingReadsUnit(key, i));
        key.texture.set_lod_free(i, read);
    }
}

LightConfig DecodeLighting(const RegisterFile& regs) {
    LightConfig result{};
    if (Bits(regs[freg::kLightingDisable], 0, 1)) {
        return result;
    }
    const u32 config = regs[freg::kLightingConfig0];
    const u32 disabled = regs[freg::kLightingConfig1];
    result.set_enable(1);
    result.set_src_num(Bits(regs[freg::kLightingMaxLightIndex], 0, 3) + 1);
    result.set_enable_primary_alpha(Bits(config, 2, 1));
    result.set_enable_secondary_alpha(Bits(config, 3, 1));
    result.set_config(static_cast<re::LightingConfig>(Bits(config, 4, 4)));
    result.set_enable_shadow(Bits(config, 0, 1));
    // Claude fix 2026-11-19 (differential oracle vs production, golden/keyfuzz): the shadow fields are zero unless enable_shadow.
    if (Bits(config, 0, 1)) {
        result.set_shadow_primary(Bits(config, 16, 1));
        result.set_shadow_secondary(Bits(config, 17, 1));
        result.set_shadow_invert(Bits(config, 18, 1));
        result.set_shadow_alpha(Bits(config, 19, 1));
        result.set_shadow_selector(Bits(config, 24, 2));
    }
    result.set_bump_selector(Bits(config, 22, 2));
    result.set_clamp_highlights(Bits(config, 27, 1));
    result.set_bump_mode(static_cast<re::LightingBumpMode>(Bits(config, 28, 2)));
    result.set_bump_renorm(!Bits(config, 30, 1));

    for (u32 slot = 0; slot < result.src_num(); ++slot) {
        const u32 light = Bits(regs[freg::kLightingLightEnable], slot * 4, 3);
        const u32 flags = regs[freg::kLight0 + light * freg::kLightStride + freg::kLightConfig];
        auto& dest = result.lights[slot];
        dest.SetField(0, 3, light);
        dest.SetField(3, 2, Bits(flags, 0, 2));
        dest.SetField(5, 1, !Bits(disabled, 24 + light, 1));
        dest.SetField(6, 1, !Bits(disabled, 8 + light, 1));
        dest.SetField(7, 2, Bits(flags, 2, 2));
        dest.SetField(9, 1, !Bits(disabled, light, 1));
    }

    constexpr std::array fields{
        &LightConfig::lut_d0, &LightConfig::lut_d1, &LightConfig::lut_sp, &LightConfig::lut_fr,
        &LightConfig::lut_rb, &LightConfig::lut_rg, &LightConfig::lut_rr,
    };
    // Reflection input and scale fields are ordered B, G, R. The disable bits are ordered R, G, B.
    constexpr std::array disable_bits{16u, 17u, 0u, 19u, 22u, 21u, 20u};
    constexpr u32 spotlight_slot = 2;
    // Claude fix 2026-11-19: the reserved codes 4 and 5 give scale 0 in production (differential oracle).
    constexpr std::array scales{1.0f, 2.0f, 4.0f, 8.0f, 0.0f, 0.0f, 0.25f, 0.5f};
    for (u32 slot = 0; slot < fields.size(); ++slot) {
        if (slot != spotlight_slot && Bits(disabled, disable_bits[slot], 1)) {
            continue;
        }
        auto& lut = result.*fields[slot];
        const u32 shift = 4 * slot;
        lut.set_enable(1);
        lut.set_abs_input(!Bits(regs[freg::kLightingAbsLutInput], shift + 1, 1));
        lut.set_type(static_cast<re::LightingLutInput>(Bits(regs[freg::kLightingLutInput], shift, 3)));
        lut.SetScale(scales[Bits(regs[freg::kLightingLutScale], shift, 3)]);
    }
    return result;
}

ProcTexConfig DecodeProcTex(const RegisterFile& regs) {
    ProcTexConfig result{};
    const u32 main = regs[freg::kMainConfig];
    if (!Bits(main, 10, 1)) {
        return result;
    }
    const u32 config = regs[freg::kProcTex];
    const u32 lut = regs[freg::kProcTexLut];
    const u32 offsets = regs[freg::kProcTexLutOffset];
    result.set_enable(1);
    result.set_coord(Bits(main, 8, 2));
    result.set_u_clamp(static_cast<re::ProcTexClamp>(Bits(config, 0, 3)));
    result.set_v_clamp(static_cast<re::ProcTexClamp>(Bits(config, 3, 3)));
    result.set_color_combiner(static_cast<re::ProcTexCombiner>(Bits(config, 6, 4)));
    result.set_alpha_combiner(static_cast<re::ProcTexCombiner>(Bits(config, 10, 4)));
    result.set_separate_alpha(Bits(config, 14, 1));
    result.set_noise_enable(Bits(config, 15, 1));
    result.set_u_shift(static_cast<re::ProcTexShift>(Bits(config, 16, 2)));
    result.set_v_shift(static_cast<re::ProcTexShift>(Bits(config, 18, 2)));
    result.set_lut_filter(static_cast<re::ProcTexFilter>(Bits(lut, 0, 3)));
    result.lod_min = static_cast<u16>(Bits(lut, 3, 4));
    result.lod_max = static_cast<u16>(Bits(lut, 7, 4));
    result.lut_width = static_cast<s32>(Bits(lut, 11, 8));
    result.lut_offset0 = static_cast<s32>(Bits(offsets, 0, 8));
    result.lut_offset1 = static_cast<s32>(Bits(offsets, 8, 8));
    result.lut_offset2 = static_cast<s32>(Bits(offsets, 16, 8));
    result.lut_offset3 = static_cast<s32>(Bits(offsets, 24, 8));
    return result;
}

bool NeedsBlendEmulation(const BlendConfig& blend, const DeviceProfile& profile) {
    return blend.RequiresMinMaxEmulation() && !profile.has_blend_minmax_factor && !blend.UsesNoOpFactors();
}

bool WrapUsesBorder(re::WrapMode wrap) {
    return wrap == re::WrapMode::ClampToBorder || wrap == re::WrapMode::ClampToBorder2;
}

} // namespace

void SetShadersAsBefore(bool on) {
    shaders_as_before.store(on, std::memory_order_relaxed);
}

bool ShadersAsBefore() {
    return shaders_as_before.load(std::memory_order_relaxed);
}

void FramebufferConfig::ApplyProfile(const DeviceProfile& profile) {
    set_logic_op((alphablend_enable() || profile.logic_op) ? re::LogicOp::Copy : requested_logic_op);
    if (alphablend_enable()) {
        for (auto* blend : {&requested_rgb_blend, &requested_alpha_blend}) {
            if (!NeedsBlendEmulation(*blend, profile)) {
                blend->SetMinMaxEmulationDisabled();
            }
        }
    }
}

void TextureConfig::ApplyProfile(const DeviceProfile& profile) {
    for (size_t i = 0; i < requested_wrap.size(); ++i) {
        texture_border_color[i].set_enable_s(!profile.custom_border_color && WrapUsesBorder(requested_wrap[i].s));
        texture_border_color[i].set_enable_t(!profile.custom_border_color && WrapUsesBorder(requested_wrap[i].t));
    }
}

void FragmentKey::ApplyProfile(const DeviceProfile& profile) {
    framebuffer.ApplyProfile(profile);
    texture.ApplyProfile(profile);
}

FragmentKey MakeKey(const pica::PicaState& state) {
    return MakeKey(state, FeatureRegistry::Global());
}

FragmentKey MakeKey(const pica::PicaState& state, const FeatureRegistry& features) {
    const bool legacy = ShadersAsBefore();
    FragmentKey key{};
    key.framebuffer = DecodeFramebuffer(state.regs);
    key.lighting = DecodeLighting(state.regs);
    key.texture = DecodeTexture(state.regs);
    key.proctex = DecodeProcTex(state.regs);
    FindLodFreeUnits(state.regs, key, legacy);
    for (const auto* feature : features.All()) {
        feature->Key(state, key, {key.user.raw, key.gas.raw});
    }
    if (legacy) {
        key.user.raw |= kUserShadersAsBefore;
    }
    return key;
}

void Needs::Merge(const Needs& other) {
    shadow_write_pass |= other.shadow_write_pass;
    dst_copy |= other.dst_copy;
    fixup_gs |= other.fixup_gs;
    lut_lf |= other.lut_lf;
    lut_rg |= other.lut_rg;
    lut_rgba |= other.lut_rgba;
    texture_units = std::max(texture_units, other.texture_units);
    ordered_rmw |= other.ordered_rmw;
}

Needs CoreNeedsOf(const FragmentKey& key, const DeviceProfile& profile) {
    const auto& fb = key.framebuffer;
    Needs needs{};
    needs.shadow_write_pass = fb.shadow_rendering() != 0;
    needs.fixup_gs = key.lighting.enable() != 0;
    if (!needs.shadow_write_pass) {
        needs.dst_copy = fb.alphablend_enable()
            ? NeedsBlendEmulation(fb.requested_rgb_blend, profile)
                || NeedsBlendEmulation(fb.requested_alpha_blend, profile)
            : !profile.logic_op && LogicOpReadsDestination(fb.requested_logic_op);
    }
    return needs;
}

Needs NeedsOf(const FragmentKey& key, const DeviceProfile& profile) {
    return NeedsOf(key, profile, FeatureRegistry::Global());
}

Needs NeedsOf(const FragmentKey& key, const DeviceProfile& profile, const FeatureRegistry& features) {
    Needs needs = CoreNeedsOf(key, profile);
    for (const auto* feature : features.All()) {
        needs.Merge(feature->NeedsOf(key, profile));
    }
    return needs;
}

} // namespace mh4n::gpu::fragment
