// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: fragment and PICA state contracts, gpu_lighting_test black-box cases and golden/gpu/glsl lighting traces.

#include "emitter.h"
#include "feature_data.h"
#include "generator.h"
#include "gpu/pica/state.h"

#include <algorithm>

namespace mh4n::gpu::fragment {
namespace {

enum Lut : u32 { D0 = 0, D1 = 1, Spot = 8, Fresnel = 3, Red = 6, Green = 5, Blue = 4 };
constexpr std::array<Lut, 7> kLuts{D0, D1, Spot, Fresnel, Red, Green, Blue};
constexpr size_t kLutWords = sizeof(LutConfig) / sizeof(u32);
constexpr size_t kLightSlots = keyword::kLighting + offsetof(LightConfig, lights) / sizeof(u32);

Value LightField(const Emitter& e, u32 first, u32 count = 1) {
    return e.Field(keyword::kLightRaw, first, count);
}

void EmitLightingHelpers(Emitter& e) {
    e.Raw(R"glsl(
float LookupLightingLUT(int table, int index, float fraction) {
    int offset = lighting_lut_offset[table / 4][table % 4];
    vec2 entry = texture_buffer_lut_lf.data[offset + index];
    return entry.x + fraction * entry.y;
}
float LookupLightingLUTUnsigned(int table, float x) {
    float pos = x * 256.0;
    int i = int(clamp(floor(pos), 0.0, 255.0));
    return LookupLightingLUT(table, i, pos - float(i));
}
float LookupLightingLUTSigned(int table, float x) {
    float pos = x * 128.0;
    int i = int(clamp(floor(pos), -128.0, 127.0));
    return LookupLightingLUT(table, i & 255, pos - float(i));
}
)glsl");
}

bool LightIsUsed(const EmitContext& context) {
    return !context.emitter->specialised() || (context.key.lighting.enable() &&
        (TevUsesSource(context.key, 1) || TevUsesSource(context.key, 2)));
}

void EmitTextureChoice(Emitter& e, const Value& unit, std::string_view dest, std::string_view transform) {
    if (unit.known) {
        e.Line("{} = (sampleTexUnit{}()){};", dest, unit.literal, transform);
        return;
    }
    e.Line("switch ({}) {{", unit.glsl);
    for (u32 i = 0; i < 4; ++i) {
        e.Line("case {}u: {} = (sampleTexUnit{}()){}; break;", i, dest, i, transform);
    }
    e.Raw("}\n");
}

Cond Supports(const Emitter& e, Lut lut, const Value& config) {
    switch (lut) {
    case D0: return e.Ne(config, 1);
    case D1: return e.And(e.And(e.Ne(config, 0), e.Ne(config, 1)), e.Ne(config, 5));
    case Spot: return e.And(e.Ne(config, 2), e.Ne(config, 3));
    case Fresnel: return e.And(e.And(e.Ne(config, 0), e.Ne(config, 2)), e.Ne(config, 4));
    case Red: return e.Ne(config, 3);
    case Green:
    case Blue: return e.Or(e.Or(e.Eq(config, 4), e.Eq(config, 5)), e.Eq(config, 8));
    }
    return e.False();
}

void EmitLut(Emitter& e, Lut lut, std::string_view target, const Value& config,
             const Value& light_num, const Cond& two_sided, const Cond& allowed) {
    const auto slot = static_cast<size_t>(std::ranges::find(kLuts, lut) - kLuts.begin());
    const size_t word = keyword::kLighting + 1 + slot * kLutWords;
    const auto mode = e.Field(word, 2, 3);
    const auto absolute = e.Ne(e.Field(word, 1, 1), 0);
    e.If(e.And(e.And(Supports(e, lut, config), allowed), e.Ne(e.Field(word, 0, 1), 0)), [&] {
        e.Raw("{\nfloat lut_pos = 0.0;\n");
        e.Switch(mode, {
            {0, [&] { e.Raw("lut_pos = dot(normal, half_dir);\n"); }},
            {1, [&] { e.Raw("lut_pos = dot(view_dir, half_dir);\n"); }},
            {2, [&] { e.Raw("lut_pos = dot(normal, view_dir);\n"); }},
            {3, [&] { e.Raw("lut_pos = dot(light_vector, normal);\n"); }},
            {4, [&] { e.Raw("lut_pos = dot(light_vector, light.spot_direction);\n"); }},
            {5, [&] {
                e.If(e.Eq(config, 8), [&] {
                    e.Raw("lut_pos = dot(half_dir - normal * dot(normal, half_dir), tangent);\n");
                });
            }},
        });
        const auto scale = e.Word(word + 1);
        const auto table = lut != Spot ? fmt::format("{}", static_cast<u32>(lut)) :
            (light_num.known ? fmt::format("{}", Spot + light_num.literal) : "8 + int(light_num)");
        const auto factor = fmt::format("uintBitsToFloat({})", scale.glsl);
        e.If(absolute, [&] {
            e.If(two_sided, [&] { e.Raw("lut_pos = abs(lut_pos);\n"); },
                  [&] { e.Raw("lut_pos = max(lut_pos, 0.0);\n"); });
            e.Line("{} = {} * LookupLightingLUTUnsigned({}, lut_pos);", target, factor, table);
        }, [&] {
            e.Line("{} = {} * LookupLightingLUTSigned({}, lut_pos);", target, factor, table);
        });
        e.Raw("}\n");
    });
}

void EmitLight(Emitter& e, const Value& flags, const Cond& last) {
    const auto bit = [&](u32 position) { return e.Ne(e.Slice(flags, position, 1), 0); };
    const auto config = LightField(e, 11, 4);
    const auto num = e.Slice(flags, 0, 3);
    const auto directional = bit(3);
    const auto two_sided = bit(4);
    e.Line("uint light_num = {};", num.glsl);
    e.Raw("LightSrc light = light_src[light_num];\nvec3 light_vector = light.position;\n");
    e.If(e.Not(directional), [&] { e.Raw("light_vector += view;\n"); });
    e.Raw(R"glsl(
float light_distance = length(light_vector);
light_vector = normalize(light_vector);
vec3 half_vector = view_dir + light_vector;
vec3 half_dir = normalize(half_vector);
float dot_product = dot(light_vector, normal);
)glsl");
    e.If(two_sided, [&] { e.Raw("dot_product = abs(dot_product);\n"); },
         [&] { e.Raw("dot_product = max(dot_product, 0.0);\n"); });
    e.Raw("float highlight = 1.0;\n");
    e.If(e.Ne(LightField(e, 10), 0), [&] { e.Raw("highlight = sign(dot_product);\n"); });
    e.Raw("float spot = 1.0;\nfloat distance_atten = 1.0;\nfloat d0 = 1.0;\nfloat d1 = 1.0;\n");
    EmitLut(e, Spot, "spot", config, num, two_sided, bit(6));
    e.If(bit(5), [&] {
        e.Raw("float distance_coord = clamp(light.dist_atten_scale * light_distance + light.dist_atten_bias, 0.0, 1.0);\n");
        if (num.known) e.Line("distance_atten = LookupLightingLUTUnsigned({}, distance_coord);", 16 + num.literal);
        else e.Raw("distance_atten = LookupLightingLUTUnsigned(16 + int(light_num), distance_coord);\n");
    });
    EmitLut(e, D0, "d0", config, num, two_sided, e.True());
    e.Raw("vec3 reflectance = vec3(1.0);\n");
    EmitLut(e, Red, "reflectance.r", config, num, two_sided, e.True());
    e.Raw("reflectance.gb = reflectance.rr;\n");
    EmitLut(e, Green, "reflectance.g", config, num, two_sided, e.True());
    EmitLut(e, Blue, "reflectance.b", config, num, two_sided, e.True());
    EmitLut(e, D1, "d1", config, num, two_sided, e.True());
    const auto geo0 = bit(7), geo1 = bit(8);
    e.If(e.Or(geo0, geo1), [&] {
        e.Raw("float half_length2 = dot(half_vector, half_vector);\n"
              "float geometry = half_length2 == 0.0 ? 0.0 : min(dot_product / half_length2, 1.0);\n");
        e.If(geo0, [&] { e.Raw("d0 *= geometry;\n"); });
        e.If(geo1, [&] { e.Raw("d1 *= geometry;\n"); });
    });

    const auto primary_alpha = e.Ne(LightField(e, 15), 0);
    const auto secondary_alpha = e.Ne(LightField(e, 16), 0);
    e.If(e.And(last, e.Or(primary_alpha, secondary_alpha)), [&] {
        e.Raw("float fresnel = 1.0;\n");
        EmitLut(e, Fresnel, "fresnel", config, num, two_sided, e.True());
        e.If(primary_alpha, [&] { e.Raw("diffuse_sum.a = fresnel;\n"); });
        e.If(secondary_alpha, [&] { e.Raw("specular_sum.a = fresnel;\n"); });
    });
    const auto shadow = e.And(e.Ne(LightField(e, 17), 0), bit(9));
    e.Raw("vec3 diffuse = light.diffuse * dot_product;\n"
          "vec3 specular = (d0 * light.specular_0 + d1 * reflectance * light.specular_1) * highlight * distance_atten * spot;\n");
    e.If(e.And(shadow, e.Ne(LightField(e, 18), 0)), [&] { e.Raw("diffuse *= shadow.rgb;\n"); });
    e.If(e.And(shadow, e.Ne(LightField(e, 19), 0)), [&] { e.Raw("specular *= shadow.rgb;\n"); });
    e.Raw("diffuse_sum.rgb += (diffuse + light.ambient) * distance_atten * spot;\n"
          "specular_sum.rgb += specular;\n");
}

void EmitLighting(Emitter& e) {
    e.Raw(R"glsl(
{
vec4 diffuse_sum = vec4(0, 0, 0, 1);
vec4 specular_sum = vec4(0, 0, 0, 1);
vec3 surface_normal = vec3(0, 0, 1);
vec3 surface_tangent = vec3(1, 0, 0);
)glsl");
    const auto bump = LightField(e, 5, 2);
    const auto selector = LightField(e, 7, 2);
    e.If(e.Eq(bump, 1), [&] {
        EmitTextureChoice(e, selector, "surface_normal", ".rgb * 2.0 - 1.0");
        e.If(e.Ne(LightField(e, 9), 0), [&] {
            e.Raw("surface_normal.z = sqrt(max(1.0 - dot(surface_normal.xy, surface_normal.xy), 0.0));\n");
        });
    }, [&] {
        e.If(e.Eq(bump, 2), [&] {
            EmitTextureChoice(e, selector, "surface_tangent", ".rgb * 2.0 - 1.0");
        });
    });
    e.Raw(R"glsl(
vec4 rotation = normalize(normquat);
vec3 normal = quaternion_rotate(rotation, surface_normal);
vec3 tangent = quaternion_rotate(rotation, surface_tangent);
vec3 view_dir = normalize(view);
vec4 shadow = vec4(1.0);
)glsl");
    const auto use_shadow = e.Ne(LightField(e, 17), 0);
    e.If(use_shadow, [&] {
        EmitTextureChoice(e, LightField(e, 22, 2), "shadow", "");
        e.If(e.Ne(LightField(e, 20), 0), [&] { e.Raw("shadow = vec4(1.0) - shadow;\n"); });
    });
    const auto count = LightField(e, 1, 4);
    if (!e.specialised()) {
        e.Line("uint light_raw[4] = uint[]({}, {}, {}, {});",
               e.Word(kLightSlots).glsl, e.Word(kLightSlots + 1).glsl,
               e.Word(kLightSlots + 2).glsl, e.Word(kLightSlots + 3).glsl);
    }
    e.ForEach(count, 8, "slot", [&](const std::string& index) {
        if (count.known) {
            // Specialised iteration emits each stored light slot once in order.
            const auto i = static_cast<u32>(index.front() - '0');
            EmitLight(e, e.Field(kLightSlots + i / 2, (i % 2) * 16, 16),
                      Cond::Known(i + 1 == std::min(count.literal, 8u)));
        } else {
            EmitLight(e, Value{false, 0, "(light_raw[slot / 2u] >> (16u * (slot % 2u)))"},
                      Cond{false, false, "slot + 1u == min(" + count.glsl + ", 8u)"});
        }
    });
    e.If(e.And(use_shadow, e.Ne(LightField(e, 21), 0)), [&] {
        e.If(e.Ne(LightField(e, 15), 0), [&] { e.Raw("diffuse_sum.a *= shadow.a;\n"); });
        e.If(e.Ne(LightField(e, 16), 0), [&] { e.Raw("specular_sum.a *= shadow.a;\n"); });
    });
    e.Raw(R"glsl(
diffuse_sum.rgb += lighting_global_ambient;
primary_fragment_color = clamp(diffuse_sum, 0.0, 1.0);
secondary_fragment_color = clamp(specular_sum, 0.0, 1.0);
}
)glsl");
}

Vec3f LightColor(u32 word) {
    return {static_cast<float>(Bits(word, 20, 10)) / 255.0f,
            static_cast<float>(Bits(word, 10, 10)) / 255.0f,
            static_cast<float>(Bits(word, 0, 10)) / 255.0f};
}

class LightingFeature final : public FeatureModule {
public:
    const char* Name() const override { return "lighting"; }

    Needs NeedsOf(const FragmentKey& key, const DeviceProfile&) const override {
        const bool enabled = key.lighting.enable() != 0;
        return {.fixup_gs = enabled, .lut_lf = enabled};
    }

    void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty) const override {
        const auto& regs = state.regs;
        if (dirty.all || state.dirty.Test(freg::kLightingGlobalAmbient)) {
            block.uniforms.lighting_global_ambient = LightColor(regs[freg::kLightingGlobalAmbient]);
            dirty.Mark();
        }
        for (u32 i = 0; i < 8; ++i) {
            const u32 base = freg::kLight0 + i * freg::kLightStride;
            constexpr std::array<u32, 10> fields{0, 1, 2, 3, 4, 5, 6, 7, 10, 11};
            if (!dirty.all && std::ranges::none_of(fields, [&](u32 field) { return state.dirty.Test(base + field); })) {
                continue;
            }
            const u32 pos = regs[base + freg::kLightPositionXY];
            const u32 spot = regs[base + freg::kLightSpotXY];
            const auto spot_coord = [](u32 value) { return static_cast<float>(detail::SignExtend(value, 13)) / 2047.0f; };
            const auto half = [](u32 value) { return detail::DecodeFloat(value, 10, 5); };
            block.uniforms.light_src[i] = {
                LightColor(regs[base + freg::kLightSpecular0]),
                LightColor(regs[base + freg::kLightSpecular1]),
                LightColor(regs[base + freg::kLightDiffuse]),
                LightColor(regs[base + freg::kLightAmbient]),
                {half(pos), half(pos >> 16), half(regs[base + freg::kLightPositionZ])},
                {spot_coord(spot), spot_coord(spot >> 16), spot_coord(regs[base + freg::kLightSpotZ])},
                detail::DecodeFloat(regs[base + freg::kLightDistAttenBias], 12, 7),
                detail::DecodeFloat(regs[base + freg::kLightDistAttenScale], 12, 7),
            };
            dirty.Mark();
        }
    }

    void UploadTables(TableUpload& upload) const override {
        constexpr std::array lanes{&Vec4i::x, &Vec4i::y, &Vec4i::z, &Vec4i::w};
        for (u32 i = 0; i < kNumLightingSamplers; ++i) {
            if ((upload.need.lighting & (1u << i)) == 0) continue;
            auto& offset = upload.block.uniforms.lighting_lut_offset[i / 4].*lanes[i % 4];
            detail::UploadLut<Vec2f>(upload, TableRing::Lf, upload.state.lighting.luts[i], offset, [](u32 word) {
                const float magnitude = static_cast<float>(Bits(word, 12, 11)) / 2047.0f;
                return Vec2f{static_cast<float>(Bits(word, 0, 12)) / 4095.0f,
                             Bits(word, 23, 1) ? -magnitude : magnitude};
            });
        }
    }

    void Emit(Point point, EmitContext& context) const override {
        if (!context.emitter || (point != Point::Declarations && point != Point::Lighting) ||
            !LightIsUsed(context)) return;

        auto& e = *context.emitter;
        if (point == Point::Declarations) {
            EmitLightingHelpers(e);
        } else {
            e.If(e.Ne(LightField(e, 0), 0), [&] { EmitLighting(e); });
        }
    }
};

MH4N_REGISTER_FRAGMENT_FEATURE(LightingFeature);

} // namespace
} // namespace mh4n::gpu::fragment
