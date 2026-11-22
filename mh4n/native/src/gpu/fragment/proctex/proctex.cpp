// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: fragment and PICA state contracts, gpu_proctex_test black-box cases and golden/gpu/glsl procedural traces.

#include "emitter.h"
#include "feature_data.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {
namespace {

Value ProcField(const Emitter& e, u32 first, u32 count) {
    return e.Field(keyword::kProcTexRaw, first, count);
}

Cond ProcEnabled(const EmitContext& context) {
    return context.emitter->Ne(ProcField(*context.emitter, 0, 1), 0);
}

void EmitNoise(Emitter& e) {
    e.Raw(R"glsl(
uint ProcNoise1D(uint n) {
    const uint permutation[16] = uint[](0, 4, 10, 8, 4, 9, 7, 12, 5, 15, 13, 14, 11, 15, 2, 11);
    return (((n % 9u + 2u) * 3u) & 15u) ^ permutation[(n / 9u) & 15u];
}
float ProcNoise2D(uvec2 point) {
    const uint permutation[16] = uint[](10, 2, 15, 8, 0, 7, 4, 5, 5, 13, 2, 6, 13, 9, 3, 14);
    uint x = ProcNoise1D(point.x);
    uint y = ProcNoise1D(point.y);
    y += (x & 3u) == 1u ? 4u : 0u;
    uint code = (((y ^ ((x & 1u) * 6u)) + 10u + x) & 15u) ^ permutation[x];
    return float(code) * (2.0 / 15.0) - 1.0;
}
float ProcTexNoiseCoef(vec2 uv) {
    vec2 pos = 9.0 * proctex_noise_f * abs(uv + proctex_noise_p);
    vec2 f = fract(pos);
    uvec2 cell = uvec2(floor(pos));
    float sum = f.x + f.y;
    float a = ProcNoise2D(cell) * sum;
    float b = ProcNoise2D(cell + uvec2(1, 0)) * (sum - 1.0);
    float c = ProcNoise2D(cell + uvec2(0, 1)) * (sum - 1.0);
    float d = ProcNoise2D(cell + uvec2(1, 1)) * (sum - 2.0);
    float u = ProcTexLookupLUT(proctex_noise_lut_offset, f.x);
    float v = ProcTexLookupLUT(proctex_noise_lut_offset, f.y);
    return mix(mix(a, b, u), mix(c, d, u), v);
}
)glsl");
}

void EmitCoordinateShift(Emitter& e, const Value& shift, const Value& clamp,
                         std::string_view coord, std::string_view other) {
    e.Line("float {}_shift = 0.0;", coord);
    const auto factor = e.Select(e.Eq(clamp, 3), e.Const(2), e.Const(1));
    const auto offset = factor.known ? (factor.literal == 2 ? "1.0" : "0.5")
                                     : fmt::format("(0.5 * float({}))", factor.glsl);
    const auto set = [&](int add) {
        e.Line("{0}_shift = {1} * float(((int({2}) + {3}) / 2) & 1);", coord, offset, other, add);
    };
    e.Switch(shift, {{1, [&] { set(0); }}, {2, [&] { set(1); }}});
}

void EmitClamp(Emitter& e, const Value& mode, std::string_view coord) {
    const auto edge = [&] { e.Line("{0} = min({0}, 1.0);", coord); };
    e.Switch(mode, {
        {0, [&] { e.Line("if ({0} > 1.0) {0} = 0.0;", coord); }},
        {1, edge},
        {2, [&] { e.Line("{0} = fract({0});", coord); }},
        {3, [&] { e.Line("{0} = abs(mod({0} + 1.0, 2.0) - 1.0);", coord); }},
        {4, [&] { e.Line("{0} = {0} <= 0.5 ? 0.0 : 1.0;", coord); }},
    }, edge);
}

void EmitMap(Emitter& e, const Value& mode, std::string_view coord, std::string_view table) {
    constexpr std::array<std::string_view, 10> coordinates{
        "u", "u * u", "v", "v * v", "(u + v) * 0.5", "dot(vec2(u, v), vec2(u, v)) * 0.5",
        "min(length(vec2(u, v)), 1.0)", "min(u, v)", "max(u, v)",
        "min((u + v) * 0.25 + length(vec2(u, v)) * 0.5, 1.0)",
    };
    if (mode.known) {
        const auto expression = mode.literal < coordinates.size() ? coordinates[mode.literal] : "0.0";
        e.Line("float {} = ProcTexLookupLUT({}, {});", coord, table, expression);
        return;
    }
    e.Line("float {}_input = 0.0;", coord);
    e.Line("switch ({}) {{", mode.glsl);
    for (size_t i = 0; i < coordinates.size(); ++i) {
        e.Line("case {}u: {}_input = {}; break;", i, coord, coordinates[i]);
    }
    e.Raw("}\n");
    e.Line("float {} = ProcTexLookupLUT({}, {}_input);", coord, table, coord);
}

void EmitProcTex(Emitter& e) {
    const auto width = e.Word(keyword::kProcTex + 1);
    const auto filter = ProcField(e, 17, 3);
    const auto lod_min = e.Field(keyword::kProcTex + 6, 0, 16);
    const auto lod_max = e.Field(keyword::kProcTex + 6, 16, 16);
    const auto noise = e.Ne(ProcField(e, 21, 1), 0);
    e.Raw(R"glsl(
float ProcTexLookupLUT(int offset, float coord) {
    float position = coord * 128.0;
    float i = clamp(floor(position), 0.0, 127.0);
    vec2 entry = texture_buffer_lut_rg.data[offset + int(i)];
    return clamp(entry.x + (position - i) * entry.y, 0.0, 1.0);
}
)glsl");
    if (!noise.known || noise.value) EmitNoise(e);
    e.Raw("vec4 SampleProcTexColor(float lut_coord, int level) {\n");
    e.Line("int width = max(int({}) >> level, 1);", width.glsl);
    e.Line("const int high_offsets[4] = int[](0xf0, 0xf8, 0xfc, 0xfe);");
    e.Line("int offsets[4] = int[](int({}), int({}), int({}), int({}));",
           e.Word(keyword::kProcTex + 2).glsl, e.Word(keyword::kProcTex + 3).glsl,
           e.Word(keyword::kProcTex + 4).glsl, e.Word(keyword::kProcTex + 5).glsl);
    e.Raw("int offset = level < 4 ? offsets[level] : high_offsets[level - 4];\n"
          "float index = lut_coord * float(width - 1);\n");
    const auto linear = e.Or(e.Or(e.Eq(filter, 1), e.Eq(filter, 3)), e.Eq(filter, 5));
    e.If(linear, [&] {
        e.Raw("int i = clamp(offset + int(index), 0, 255);\n"
              "return texture_buffer_lut_rgba.data[proctex_lut_offset + i] +\n"
              "       fract(index) * texture_buffer_lut_rgba.data[proctex_diff_lut_offset + i];\n");
    }, [&] {
        e.Raw("int i = clamp(offset + int(round(index)), 0, 255);\n"
              "return texture_buffer_lut_rgba.data[proctex_lut_offset + i];\n");
    });
    e.Raw("}\nvec4 ProcTexEvaluate() {\n");
    const auto coord = ProcField(e, 1, 2);
    if (coord.known) {
        e.Line("vec2 uv = abs(texcoord{});", coord.literal < 3 ? coord.literal : 0);
    } else {
        e.Line("vec2 uv = abs({0} == 1u ? texcoord1 : ({0} == 2u ? texcoord2 : texcoord0));", coord.glsl);
    }
    e.Raw("vec2 duv = max(abs(dFdx(uv)), abs(dFdy(uv)));\n");
    e.Line("float lod = proctex_bias == 0.0 ? 0.0 : log2(abs(float({}) * proctex_bias) * (duv.x + duv.y));", width.glsl);
    e.Line("float max_level = min(7.0, float({}));", lod_max.glsl);
    e.Line("lod = clamp(lod, min(float({}), max_level), max_level);", lod_min.glsl);
    const auto u_clamp = ProcField(e, 3, 3);
    const auto v_clamp = ProcField(e, 6, 3);
    EmitCoordinateShift(e, ProcField(e, 22, 2), u_clamp, "u", "uv.y");
    EmitCoordinateShift(e, ProcField(e, 24, 2), v_clamp, "v", "uv.x");
    e.If(noise, [&] {
        e.Raw("uv += proctex_noise_a * ProcTexNoiseCoef(uv);\nuv = abs(uv);\n");
    });
    e.Raw("float u = uv.x + u_shift;\nfloat v = uv.y + v_shift;\n");
    EmitClamp(e, u_clamp, "u");
    EmitClamp(e, v_clamp, "v");
    EmitMap(e, ProcField(e, 9, 4), "lut_coord", "proctex_color_map_offset");
    e.Raw("vec4 final_color;\n");
    e.If(e.Or(e.Eq(filter, 2), e.Eq(filter, 3)), [&] {
        e.Raw("final_color = SampleProcTexColor(lut_coord, int(round(lod)));\n");
    }, [&] {
        e.If(e.Or(e.Eq(filter, 4), e.Eq(filter, 5)), [&] {
            e.Raw("int level = int(lod);\n"
                  "final_color = mix(SampleProcTexColor(lut_coord, level), "
                  "SampleProcTexColor(lut_coord, min(level + 1, 7)), fract(lod));\n");
        }, [&] { e.Raw("final_color = SampleProcTexColor(lut_coord, 0);\n"); });
    });
    e.If(e.Ne(ProcField(e, 20, 1), 0), [&] {
        EmitMap(e, ProcField(e, 13, 4), "alpha_coord", "proctex_alpha_map_offset");
        e.Raw("final_color.a = alpha_coord;\n");
    });
    e.Raw("return final_color;\n}\nvec4 proctex_value = vec4(0.0);\nvec4 ProcTex() { return proctex_value; }\n");
}

class ProcTexFeature final : public FeatureModule {
public:
    const char* Name() const override { return "proctex"; }

    Needs NeedsOf(const FragmentKey& key, const DeviceProfile&) const override {
        const bool enabled = key.proctex.enable() != 0;
        return {.lut_rg = enabled, .lut_rgba = enabled};
    }

    void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty) const override {
        const auto& regs = state.regs;
        const auto half = [](u32 value) { return detail::DecodeFloat(value, 10, 5); };
        auto& dst = block.uniforms;
        const auto update = [&](float& output, float input) {
            if (dirty.all || std::bit_cast<u32>(output) != std::bit_cast<u32>(input)) {
                output = input;
                dirty.Mark();
            }
        };
        if (dirty.all || state.dirty.Test(freg::kProcTex) || state.dirty.Test(freg::kProcTexLut)) {
            update(dst.proctex_bias, half(Bits(regs[freg::kProcTex], 20, 8) | Bits(regs[freg::kProcTexLut], 19, 8) << 8));
        }
        if (dirty.all || state.dirty.Test(freg::kProcTexNoiseFrequency)) {
            const auto word = regs[freg::kProcTexNoiseFrequency];
            update(dst.proctex_noise_f.x, half(word));
            update(dst.proctex_noise_f.y, half(word >> 16));
        }
        for (u32 axis = 0; axis < 2; ++axis) {
            const u32 reg = freg::kProcTexNoiseU + axis;
            if (dirty.all || state.dirty.Test(reg)) {
                const u32 word = regs[reg];
                update(axis == 0 ? dst.proctex_noise_a.x : dst.proctex_noise_a.y,
                       static_cast<float>(detail::SignExtend(word, 16)) / 4095.0f);
                update(axis == 0 ? dst.proctex_noise_p.x : dst.proctex_noise_p.y, half(word >> 16));
            }
        }
    }

    void UploadTables(TableUpload& upload) const override {
        const auto& tables = upload.state.proctex;
        auto& dst = upload.block.uniforms;
        const auto value_entry = [](u32 word) {
            return Vec2f{static_cast<float>(Bits(word, 0, 12)) / 4095.0f,
                         static_cast<float>(detail::SignExtend(word >> 12, 12)) / 4095.0f};
        };
        constexpr auto ring = TableRing::General;
        if (upload.need.proctex & (1 << 0)) detail::UploadLut<Vec2f>(upload, ring, tables.noise_table, dst.proctex_noise_lut_offset, value_entry);
        if (upload.need.proctex & (1 << 2)) detail::UploadLut<Vec2f>(upload, ring, tables.color_map_table, dst.proctex_color_map_offset, value_entry);
        if (upload.need.proctex & (1 << 3)) detail::UploadLut<Vec2f>(upload, ring, tables.alpha_map_table, dst.proctex_alpha_map_offset, value_entry);
        if (upload.need.proctex & (1 << 4)) detail::UploadLut<Vec4f>(upload, ring, tables.color_table, dst.proctex_lut_offset, ColorRGBA8);
        if (upload.need.proctex & (1 << 5)) {
            detail::UploadLut<Vec4f>(upload, ring, tables.color_diff_table, dst.proctex_diff_lut_offset, [](u32 word) {
                const auto entry = [&](u32 shift) { return static_cast<float>(detail::SignExtend(word >> shift, 8)) * (2.0f / 255.0f); };
                return Vec4f{entry(0), entry(8), entry(16), entry(24)};
            });
        }
    }

    void Emit(Point point, EmitContext& context) const override {
        if (!context.emitter) return;
        auto& e = *context.emitter;
        const auto active = ProcEnabled(context);
        if (point == Point::Declarations) {
            if (!active.known || active.value) EmitProcTex(e);
            else e.Raw("vec4 ProcTex() { return vec4(0.0); }\n");
        } else if (point == Point::Prologue) {
            e.If(active, [&] { e.Raw("proctex_value = ProcTexEvaluate();\n"); });
        }
    }
};

MH4N_REGISTER_FRAGMENT_FEATURE(ProcTexFeature);

} // namespace
} // namespace mh4n::gpu::fragment
