// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: shadow/shadow.h, gpu_shadow_test black-box cases and golden/gpu/glsl shadow draws.

#include "shadow/shadow.h"

#include "emitter.h"
#include "feature_data.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {
namespace {

void EmitCompare(Emitter& e) {
    e.Raw(R"glsl(
float CompareShadow(uint packed, int reference) {
    uvec2 entry = DecodeShadow(packed);
    return int(entry.x) >= reference ? float(entry.y) / 255.0 : 0.0;
}
int ShadowReference(float distance) {
    return int(min(abs(distance), 1.0) * float(0xFFFFFF)) - shadow_texture_bias;
}
float FilterShadow(vec4 values, vec2 ratio) {
    vec2 columns = mix(values.xy, values.zw, ratio.y);
    return mix(columns.x, columns.y, ratio.x);
}
)glsl");
}

void Emit2DLookup(Emitter& e) {
    e.Raw(R"glsl(
float ShadowTexel(ivec2 p, ivec2 size, int reference) {
    if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, size))) return 1.0;
    return CompareShadow(texelFetch(tex0_shadow, p, 0).r, reference);
}
vec4 shadowTexture(vec2 uv, float w) {
)glsl");
    e.If(e.Eq(e.Field(keyword::kTexRaw, 16, 1), 0), [&] { e.Raw("uv /= w;\n"); });
    e.Raw(R"glsl(
    int reference = ShadowReference(w);
    ivec2 size = textureSize(tex0_shadow, 0);
    vec2 coord = vec2(size) * uv - 0.5;
    ivec2 p = ivec2(floor(coord));
    vec4 samples = vec4(ShadowTexel(p, size, reference),
                        ShadowTexel(p + ivec2(1, 0), size, reference),
                        ShadowTexel(p + ivec2(0, 1), size, reference),
                        ShadowTexel(p + ivec2(1, 1), size, reference));
    return vec4(FilterShadow(samples, fract(coord)));
}
)glsl");
}

void EmitCubeLookup(Emitter& e) {
    e.Raw(R"glsl(
float ShadowCubeTexel(ivec2 p, ivec2 size, int face, int reference) {
    vec2 st = (vec2(clamp(p, ivec2(0), size - 1)) + 0.5) / vec2(size) * 2.0 - 1.0;
    vec3 direction;
    switch (face) {
    case 0: direction = vec3(1, -st.y, -st.x); break;
    case 1: direction = vec3(-1, -st.y, st.x); break;
    case 2: direction = vec3(st.x, 1, st.y); break;
    case 3: direction = vec3(st.x, -1, -st.y); break;
    case 4: direction = vec3(st.x, -st.y, 1); break;
    default: direction = vec3(-st.x, -st.y, -1); break;
    }
    uvec4 b = uvec4(round(textureLod(samplerCube(tex0_cube, samp0), direction, 0.0) * 255.0));
    return CompareShadow(b.x | b.y << 8 | b.z << 16 | b.w << 24, reference);
}
vec4 shadowTextureCube(vec2 uv, float w) {
    vec3 magnitude = abs(vec3(uv, w));
    int face;
    vec2 face_coord;
    float distance;
    if (magnitude.x > magnitude.y && magnitude.x > magnitude.z) {
        face = uv.x > 0.0 ? 0 : 1;
        face_coord = vec2(-w * sign(uv.x), -uv.y);
        distance = magnitude.x;
    } else if (magnitude.y > magnitude.z) {
        face = uv.y > 0.0 ? 2 : 3;
        face_coord = vec2(uv.x, w * sign(uv.y));
        distance = magnitude.y;
    } else {
        face = w > 0.0 ? 4 : 5;
        face_coord = vec2(uv.x * sign(w), -uv.y);
        distance = magnitude.z;
    }
    int reference = ShadowReference(distance);
    if (distance == 0.0) return vec4(1.0);
    ivec2 size = textureSize(tex0_cube, 0);
    vec2 coord = (face_coord / distance * 0.5 + 0.5) * vec2(size) - 0.5;
    ivec2 p = ivec2(floor(coord));
    vec4 samples = vec4(ShadowCubeTexel(p, size, face, reference),
                        ShadowCubeTexel(p + ivec2(1, 0), size, face, reference),
                        ShadowCubeTexel(p + ivec2(0, 1), size, face, reference),
                        ShadowCubeTexel(p + ivec2(1, 1), size, face, reference));
    return vec4(FilterShadow(samples, fract(coord)));
}
)glsl");
}

void EmitWriteHelper(Emitter& e) {
    e.Raw(R"glsl(
uint UpdateShadow(uint packed, uint depth, uint stencil) {
    uvec2 saved = DecodeShadow(packed);
    if (depth >= saved.x) return packed;
    if (stencil == 0u) return (depth << 8) | saved.y;
    float bias = shadow_bias_constant + shadow_bias_linear * (float(depth) / float(saved.x));
    uint shade = uint(clamp(float(stencil) / bias, 0.0, 255.0));
    return (saved.x << 8) | min(saved.y, shade);
}
)glsl");
}

void EmitWrite(Emitter& e) {
    e.Raw(R"glsl(
vec4 shadow_tev_output = combiner_output;
uint d = uint(clamp(depth, 0.0, 1.0) * float(0xFFFFFF));
uint s = uint(round(clamp(shadow_tev_output.g, 0.0, 1.0) * 255.0));
ivec2 image_coord = ivec2(gl_FragCoord.xy);
)glsl");
    if (e.profile().interlock) {
        e.Raw(R"glsl(
beginInvocationInterlock();
uint saved = imageLoad(shadow_buffer, image_coord).r;
imageStore(shadow_buffer, image_coord, uvec4(UpdateShadow(saved, d, s)));
endInvocationInterlock();
)glsl");
    } else {
        e.Raw(R"glsl(
uint saved = imageAtomicAdd(shadow_buffer, image_coord, 0u);
for (;;) {
    uint updated = UpdateShadow(saved, d, s);
    if (updated == saved) break;
    uint actual = imageAtomicCompSwap(shadow_buffer, image_coord, saved, updated);
    if (actual == saved) break;
    saved = actual;
}
)glsl");
    }
}

class ShadowFeature final : public FeatureModule {
public:
    const char* Name() const override { return "shadow"; }

    Needs NeedsOf(const FragmentKey& key, const DeviceProfile&) const override {
        return {.shadow_write_pass = key.framebuffer.shadow_rendering() != 0};
    }

    void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty) const override {
        auto& uniforms = block.uniforms;
        if (dirty.all || state.dirty.Test(freg::kFramebufferShadow)) {
            const u32 bias = state.regs[freg::kFramebufferShadow];
            const float constant = detail::DecodeFloat(bias, 10, 5);
            const float linear = detail::DecodeFloat(bias >> 16, 10, 5);
            if (dirty.all || std::bit_cast<u32>(uniforms.shadow_bias_constant) != std::bit_cast<u32>(constant) ||
                std::bit_cast<u32>(uniforms.shadow_bias_linear) != std::bit_cast<u32>(linear)) {
                uniforms.shadow_bias_constant = constant;
                uniforms.shadow_bias_linear = linear;
                dirty.Mark();
            }
        }
        if (dirty.all || state.dirty.Test(freg::kShadowTexture)) {
            const int bias = static_cast<int>(state.regs[freg::kShadowTexture] & 0xFFFFFEu);
            if (dirty.all || uniforms.shadow_texture_bias != bias) {
                uniforms.shadow_texture_bias = bias;
                dirty.Mark();
            }
        }
    }

    void Emit(Point point, EmitContext& context) const override {
        if (!context.emitter) return;
        auto& e = *context.emitter;
        const bool writing = e.pass() == ShaderPass::ShadowWrite;
        if (point == Point::Declarations) {
            const auto type = context.key.texture.texture0_type();
            using Type = regenum::TextureType;
            const bool lookup_2d = !e.specialised() || type == Type::Shadow2D;
            const bool lookup_cube = !e.specialised() || type == Type::ShadowCube;
            if (writing) EmitWriteHelper(e);
            if (lookup_2d || lookup_cube) EmitCompare(e);
            if (lookup_2d) Emit2DLookup(e);
            else e.Raw("vec4 shadowTexture(vec2 uv, float w) { return vec4(1.0); }\n");
            if (lookup_cube) EmitCubeLookup(e);
            else e.Raw("vec4 shadowTextureCube(vec2 uv, float w) { return vec4(1.0); }\n");
        } else if (point == Point::Output && writing) {
            EmitWrite(e);
        }
    }
};

MH4N_REGISTER_FRAGMENT_FEATURE(ShadowFeature);

} // namespace

namespace shadow {

std::optional<ShadowTexture> ShadowTextureOf(const u32* regs) {
    using Type = regenum::TextureType;
    const auto type = static_cast<Type>(Bits(regs[freg::kTexture0 + freg::kTextureParams], 28, 3));
    const u32 addr = Bits(regs[freg::kTexture0 + 4], 0, 28);
    if ((regs[freg::kMainConfig] & 1) == 0 || addr == 0 ||
        (type != Type::Shadow2D && type != Type::ShadowCube)) {
        return std::nullopt;
    }
    const u32 size = regs[freg::kTexture0 + 1];
    ShadowTexture texture{
        .cube = type == Type::ShadowCube,
        .width = Bits(size, 16, 11),
        .height = Bits(size, 0, 11),
        .format = Bits(regs[0x8E], 0, 4),
    };
    texture.paddr[0] = addr << 3;
    if (texture.cube) {
        for (u32 face = 1; face < texture.paddr.size(); ++face) {
            texture.paddr[face] = ((addr & ~0x3FFFFFu) | Bits(regs[0x85 + face], 0, 22)) << 3;
        }
    }
    return texture;
}

} // namespace shadow
} // namespace mh4n::gpu::fragment
