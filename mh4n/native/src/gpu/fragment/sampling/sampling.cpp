// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: sampling/sampling.h, emitter.h, gpu_sampling_test black-box cases and golden/gpu/glsl texture lookups.

#include "sampling/sampling.h"

#include <algorithm>
#include <cstring>

#include "emitter.h"
#include "feature_data.h"
#include "generator.h"

namespace mh4n::gpu::fragment {
namespace {

constexpr std::array kUnitRegs{freg::kTexture0, freg::kTexture1, freg::kTexture2};
using namespace sampling;

void Emit2D(Emitter& e, u32 unit, bool projected) {
    const auto coord = unit == 2
        ? e.Select(e.Ne(e.Field(keyword::kTexRaw, 3, 1), 0), e.Const(1), e.Const(2))
        : e.Const(unit);
    if (coord.known) {
        e.Line("vec2 tc = texcoord{};", coord.literal);
    } else {
        e.Line("vec2 tc = {} == 1u ? texcoord1 : texcoord2;", coord.glsl);
    }

    e.Raw("vec2 tcw = tc;\n");
    for (u32 axis = 0; axis < 2; ++axis) {
        const auto wrap = e.Word(keyword::kTexWrap + 2 * unit + axis);
        const char component = axis == 0 ? 'x' : 'y';
        const auto mixed_edge = e.Eq(wrap, kClampToEdge2);
        const auto mixed_border = e.Eq(wrap, kClampToBorder2);
        const auto shader_border = e.profile().custom_border_color
            ? e.False() : e.Eq(wrap, kClampToBorder);
        const auto repeat_negative = e.Or(mixed_edge, mixed_border);

        if (!shader_border.known || shader_border.value || !repeat_negative.known || repeat_negative.value) {
            const auto uv = fmt::format("tc.{}{}", component, projected ? " / texcoord0_w" : "");
            const auto negative = Cond{false, false, fmt::format("({}) < 0.0", uv)};
            const auto upper = Cond{false, false, fmt::format("({}) >= 1.0", uv)};
            const auto border = e.Or(e.And(shader_border, e.Or(negative, upper)), e.And(mixed_border, upper));
            e.If(border, [&] { e.Line("return tex_border_color[{}];", unit); });
            e.If(e.And(repeat_negative, negative), [&] {
                e.Line("tcw.{} = fract({}){};", component, uv, projected ? " * texcoord0_w" : "");
            });
        }
    }

    if (projected) {
        const bool adjusted = !e.specialised() ||
            e.KnownWord(keyword::kTexWrap) == kClampToEdge2 || e.KnownWord(keyword::kTexWrap) == kClampToBorder2 ||
            e.KnownWord(keyword::kTexWrap + 1) == kClampToEdge2 || e.KnownWord(keyword::kTexWrap + 1) == kClampToBorder2;
        e.Line("return textureProj(sampler2D(tex0, samp0), vec3({}, texcoord0_w));", adjusted ? "tcw" : "tc");
        return;
    }
    e.If(e.Ne(e.Field(keyword::kTexRaw, 17 + unit, 1), 0), [&] {
        e.Line("return textureLod(sampler2D(tex{0}, samp{0}), tcw, 0.0);", unit);
    }, [&] {
        e.Line("float lod = getLod(tc * vec2(textureSize(tex{0}, 0))) + tex_lod_bias[{0}];", unit);
        e.Line("return textureLod(sampler2D(tex{0}, samp{0}), tcw, lod);", unit);
    });
}

void EmitSampling(Emitter& e) {
    using Type = regenum::TextureType;
    e.Raw("vec4 sampleTexUnit0() {\n");
    e.Switch(e.Field(keyword::kTexRaw, 0, 3), {
        {static_cast<u32>(Type::TextureCube), [&] {
            e.Raw("return texture(samplerCube(tex0_cube, samp0), vec3(texcoord0, texcoord0_w));\n");
        }},
        {static_cast<u32>(Type::Shadow2D), [&] { e.Raw("return shadowTexture(texcoord0, texcoord0_w);\n"); }},
        {static_cast<u32>(Type::Projection2D), [&] { Emit2D(e, 0, true); }},
        {static_cast<u32>(Type::ShadowCube), [&] { e.Raw("return shadowTextureCube(texcoord0, texcoord0_w);\n"); }},
        {static_cast<u32>(Type::Disabled), [&] { e.Raw("return vec4(0.0);\n"); }},
    }, [&] { Emit2D(e, 0, false); });
    e.Raw("}\n");
    for (u32 unit : {1u, 2u}) {
        e.Line("vec4 sampleTexUnit{}() {{", unit);
        Emit2D(e, unit, false);
        e.Raw("}\n");
    }
    e.Raw("vec4 sampleTexUnit3() {\n");
    e.If(e.Ne(e.Field(keyword::kProcTexRaw, 0, 1), 0),
         [&] { e.Raw("return ProcTex();\n"); },
         [&] { e.Raw("return vec4(0.0);\n"); });
    e.Raw("}\n");
}

class SamplingFeature final : public FeatureModule {
public:
    const char* Name() const override { return "sampling"; }

    void Emit(Point point, EmitContext& context) const override {
        if (point == Point::Declarations && context.emitter) {
            EmitSampling(*context.emitter);
        }
    }

    Needs NeedsOf(const FragmentKey& key, const DeviceProfile&) const override {
        u8 units = 0;
        const auto& light = key.lighting;
        for (u32 unit = 0; unit < kUnitRegs.size(); ++unit) {
            if (TevUsesSource(key, 3 + unit) || (light.enable() &&
                ((light.bump_mode() != regenum::LightingBumpMode::None && light.bump_selector() == unit) ||
                 (light.enable_shadow() && light.shadow_selector() == unit)))) {
                units = static_cast<u8>(unit + 1);
            }
        }
        return {.texture_units = units};
    }

    void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty) const override {
        auto& uniforms = block.uniforms;
        std::array<float*, 3> biases{&uniforms.tex_lod_bias.x, &uniforms.tex_lod_bias.y, &uniforms.tex_lod_bias.z};
        for (size_t unit = 0; unit < kUnitRegs.size(); ++unit) {
            const u32 lod = kUnitRegs[unit] + freg::kTextureLod;
            const u32 border = kUnitRegs[unit] + freg::kTextureBorderColor;
            const u32 address = kUnitRegs[unit] + 4;
            if (dirty.all || state.dirty.Test(lod)) {
                const float value = LodBiasOf(state.regs[lod]);
                if (dirty.all || value != *biases[unit]) {
                    *biases[unit] = value;
                    dirty.Mark();
                }
            }
            if (dirty.all || state.dirty.Test(border) || state.dirty.Test(address) || state.dirty.Test(freg::kMainConfig)) {
                const bool enabled = Bits(state.regs[freg::kMainConfig], static_cast<u32>(unit), 1) != 0;
                const bool addressed = Bits(state.regs[address], 0, 28) != 0;
                const Vec4f value = ColorRGBA8(!enabled ? 0 : (addressed ? state.regs[border] : 0xFF000000u));
                auto& current = uniforms.tex_border_color[unit];
                if (dirty.all || value.x != current.x || value.y != current.y || value.z != current.z || value.w != current.w) {
                    current = value;
                    dirty.Mark();
                }
            }
        }
    }
};

MH4N_REGISTER_FRAGMENT_FEATURE(SamplingFeature);

} // namespace

namespace sampling {

SamplerParams SamplerParamsOf(const u32* regs, u32 unit) {
    const u32 base = kUnitRegs[unit];
    const u32 params = regs[base + freg::kTextureParams];
    const u32 lod = regs[base + freg::kTextureLod];
    return {Bits(params, 1, 1), Bits(params, 2, 1), Bits(params, 24, 1),
            Bits(params, 12, 3), Bits(params, 8, 3), regs[base + freg::kTextureBorderColor],
            Bits(lod, 24, 4), Bits(lod, 16, 4)};
}

float LodBiasOf(u32 lod_word) {
    return static_cast<float>(detail::SignExtend(lod_word, 13)) / 256.0f;
}

rhi::SamplerDesc SamplerDescOf(const SamplerParams& params, const rhi::Capabilities& caps) {
    using A = rhi::AddressMode;
    const auto address = [&](u32 mode) {
        switch (mode) {
        case kClampToBorder: return caps.custom_border_color ? A::ClampToBorder : A::ClampToEdge;
        case kRepeat:
        case kRepeat2:
        case kRepeat3: return A::Repeat;
        case kMirroredRepeat: return A::MirroredRepeat;
        default: return A::ClampToEdge;
        }
    };
    const auto filter = [](u32 flag) { return flag ? rhi::Filter::Linear : rhi::Filter::Nearest; };
    rhi::SamplerDesc desc{
        .min_filter = filter(params.min_filter),
        .mag_filter = filter(params.mag_filter),
        .mip_filter = filter(params.mip_filter),
        .address_u = address(params.wrap_s),
        .address_v = address(params.wrap_t),
        .custom_border_color = std::nullopt,
        .min_lod = static_cast<float>(params.lod_min),
        .max_lod = static_cast<float>(std::max(params.lod_min, params.lod_max)),
    };
    if (desc.address_u == A::ClampToBorder || desc.address_v == A::ClampToBorder) {
        const auto color = ColorRGBA8(params.border_color);
        desc.custom_border_color = std::array{color.x, color.y, color.z, color.w};
    }
    return desc;
}

rhi::Result<std::shared_ptr<rhi::Sampler>> SamplerCache::Get(const SamplerParams& params) {
    const auto desc = SamplerDescOf(params, backend_->Caps());
    auto key = params;
    if (!desc.custom_border_color) {
        key.border_color = 0;
    }
    if (const auto it = samplers_.find(key); it != samplers_.end()) {
        ++hits_;
        return it->second;
    }
    auto made = backend_->CreateSampler(desc);
    if (made) {
        samplers_.emplace(key, *made);
        ++creates_;
    }
    return made;
}

rhi::Result<void> UploadRgba8Rows(rhi::Recorder& rec, rhi::Ring& staging, rhi::Image& image, u32 mip, u32 layer,
                                 u32 width, u32 height, u32 first, u32 count, std::span<const u8> rgba) {
    const u64 row_bytes = static_cast<u64>(width) * 4;
    if (!width || !height || first > height || count > height - first || height > rgba.size() / row_bytes) {
        return rhi::Error{"Invalid RGBA8 upload dimensions or row range"};
    }
    if (!count) {
        return {};
    }
    const auto allocation = staging.Map(row_bytes * count, 4);
    if (!allocation) {
        return rhi::Error{allocation.error()};
    }
    for (u32 row = 0; row < count; ++row) {
        std::memcpy(allocation->data.data() + row * row_bytes,
                    rgba.data() + (first + count - 1 - row) * row_bytes, row_bytes);
    }
    staging.GetBuffer()->Flush(allocation->offset, row_bytes * count);
    const auto written = staging.Commit(row_bytes * count);
    if (!written) {
        return rhi::Error{written.error()};
    }
    const rhi::BufferImageRegion region{
        .buffer_offset = allocation->offset,
        .mip_level = mip,
        .layer = layer,
        .image_rect = {0, static_cast<s32>(height - first - count), width, count},
    };
    rec.CopyBufferToImage(*staging.GetBuffer(), image, std::span{&region, 1});
    return {};
}

rhi::Result<void> UploadRgba8(rhi::Recorder& rec, rhi::Ring& staging, rhi::Image& image, u32 mip, u32 layer,
                             u32 width, u32 height, std::span<const u8> rgba) {
    return UploadRgba8Rows(rec, staging, image, mip, layer, width, height, 0, height, rgba);
}

} // namespace sampling
} // namespace mh4n::gpu::fragment
