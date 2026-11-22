// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: fragment and binding contracts, libctru GPU enums, gpu_tev_test and golden/gpu/glsl.

#include "generator.h"

#include <array>

#include "gpu/native/bindings.h"
#include "regs.h"

namespace mh4n::gpu::fragment {
namespace {

using Operation = regenum::TevOperation;
using Compare = regenum::CompareFunc;
using BlendEquation = regenum::BlendEquation;
using BlendFactor = regenum::BlendFactor;
using LogicOp = regenum::LogicOp;

constexpr u32 kStages = 6;
constexpr u32 kBufferWriteStages = 4;
constexpr u32 kConstantSource = 14;
constexpr u32 kPreviousSource = 15;
constexpr u32 kFirstSourceMask = 0x000F000Fu;
constexpr u32 kFirstModifiersMask = 0x0000700Fu;
constexpr u32 kOperationsMask = 0x000F000Fu;
constexpr u32 kLastPosition = 2;

constexpr std::array<std::string_view, 16> kSources{
    "rounded_primary_color", "primary_fragment_color", "secondary_fragment_color",
    "sampleTexUnit0()", "sampleTexUnit1()", "sampleTexUnit2()", "sampleTexUnit3()",
    "", "", "", "", "", "", "combiner_buffer", "", "combiner_output",
};
constexpr std::array<std::string_view, 8> kRgbSwizzles{"rgb", "aaa", "rrr", "", "ggg", "", "bbb", ""};
constexpr std::array<std::string_view, 4> kAlphaSwizzles{"a", "r", "g", "b"};
constexpr std::array<u32, 10> kOperandCounts{1, 2, 2, 2, 3, 2, 2, 2, 3, 3};

constexpr u32 Number(auto enumerator) {
    return static_cast<u32>(enumerator);
}

constexpr u32 TevMultiplier(u32 scale) {
    return scale == 3 ? 1 : 1u << scale;
}

constexpr u32 OperandCount(u32 operation, bool alpha) {
    if (operation >= kOperandCounts.size() || (alpha && (operation == Number(Operation::Dot3_RGB) ||
                                                        operation == Number(Operation::Dot3_RGBA)))) {
        return 0;
    }
    return kOperandCounts[operation];
}

constexpr u32 SourceOf(const TevStageConfigRaw& stage, u32 stage_index, u32 position, bool alpha) {
    const u32 source = alpha ? stage.alpha_source(position) : stage.color_source(position);
    if (stage_index == 0 && position < kLastPosition && source == kPreviousSource) {
        return alpha ? stage.alpha_source(kLastPosition) : stage.color_source(kLastPosition);
    }
    return source;
}

constexpr std::string_view Zero(bool alpha) {
    return alpha ? "0.0" : "vec3(0.0)";
}

constexpr std::string_view One(bool alpha) {
    return alpha ? "1.0" : "vec3(1.0)";
}

std::string SourceExpression(u32 source, std::string_view stage) {
    if (source == kConstantSource) {
        return fmt::format("const_color[{}]", stage);
    }
    return kSources[source].empty() ? "vec4(0.0)" : std::string(kSources[source]);
}

std::string ModifierExpression(u32 modifier, std::string_view source, bool alpha) {
    const std::string_view swizzle = alpha ? kAlphaSwizzles[modifier / 2] : kRgbSwizzles[modifier / 2];
    if (swizzle.empty()) {
        return std::string(Zero(alpha));
    }
    if (modifier % 2 != 0) {
        return fmt::format("{} - {}.{}", One(alpha), source, swizzle);
    }
    return fmt::format("{}.{}", source, swizzle);
}

std::string CombineExpression(Operation op, const std::array<std::string, 3>& operands, bool alpha) {
    const auto& [a, b, c] = operands;
    switch (op) {
    case Operation::Replace: return a;
    case Operation::Modulate: return fmt::format("{} * {}", a, b);
    case Operation::Add: return fmt::format("{} + {}", a, b);
    case Operation::AddSigned:
        return fmt::format("{} + {} - {}", a, b, alpha ? "0.5" : "vec3(0.5)");
    case Operation::Lerp: return fmt::format("mix({}, {}, {})", b, a, c);
    case Operation::Subtract: return fmt::format("{} - {}", a, b);
    case Operation::Dot3_RGB:
    case Operation::Dot3_RGBA:
        if (alpha) {
            return "0.0";
        }
        return fmt::format("vec3(4.0 * dot({} - vec3(0.5), {} - vec3(0.5)))", a, b);
    case Operation::MultiplyThenAdd: return fmt::format("fma({}, {}, {})", a, b, c);
    case Operation::AddThenMultiply: return fmt::format("min({} + {}, {}) * {}", a, b, One(alpha), c);
    }
    return std::string(Zero(alpha));
}

std::string Quantize(std::string_view expression, bool alpha) {
    return fmt::format("byteround(clamp({}, {}, {}))", expression, Zero(alpha), One(alpha));
}

void EmitDeclarations(Emitter& e) {
    e.Raw("#version 450\n#extension GL_EXT_samplerless_texture_functions : require\n");
    if (e.pass() == ShaderPass::ShadowWrite && e.profile().interlock) {
        e.Raw(R"glsl(#extension GL_ARB_fragment_shader_interlock : require
#define beginInvocationInterlock beginInvocationInterlockARB
#define endInvocationInterlock endInvocationInterlockARB
layout(pixel_interlock_ordered) in;
)glsl");
    }

    e.Line("#define NUM_TEV_STAGES {}", kStages);
    e.Line("#define NUM_LIGHTING_SAMPLERS {}", kNumLightingSamplers);
    e.Raw(R"glsl(#define NUM_LIGHTS 8
layout (location = 1) in vec4 primary_color;
layout (location = 2) in vec2 texcoord0;
layout (location = 3) in vec2 texcoord1;
layout (location = 4) in vec2 texcoord2;
layout (location = 5) in float texcoord0_w;
layout (location = 6) in vec4 normquat;
layout (location = 7) in vec3 view;
layout (location = 0) out vec4 color;

struct LightSrc {
    vec3 specular_0;
    vec3 specular_1;
    vec3 diffuse;
    vec3 ambient;
    vec3 position;
    vec3 spot_direction;
    float dist_atten_bias;
    float dist_atten_scale;
};
)glsl");
    using namespace native;
    e.Line("layout (set = {}, binding = {}, std140) uniform fs_data {{", set::kUniforms, binding::kFragmentBlock);
    e.Raw(R"glsl(    int framebuffer_scale;
    int alphatest_ref;
    float depth_scale;
    float depth_offset;
    float shadow_bias_constant;
    float shadow_bias_linear;
    int scissor_x1;
    int scissor_y1;
    int scissor_x2;
    int scissor_y2;
    int fog_lut_offset;
    int proctex_noise_lut_offset;
    int proctex_color_map_offset;
    int proctex_alpha_map_offset;
    int proctex_lut_offset;
    int proctex_diff_lut_offset;
    float proctex_bias;
    int shadow_texture_bias;
    ivec4 lighting_lut_offset[NUM_LIGHTING_SAMPLERS / 4];
    vec3 fog_color;
    vec2 proctex_noise_f;
    vec2 proctex_noise_a;
    vec2 proctex_noise_p;
    vec3 lighting_global_ambient;
    LightSrc light_src[NUM_LIGHTS];
    vec4 const_color[NUM_TEV_STAGES];
    vec4 tev_combiner_buffer_color;
    vec3 tex_lod_bias;
    vec4 tex_border_color[3];
    vec4 blend_color;
)glsl");
    e.Line("    uvec4 cfg[{}];", (kFragmentKeyWords + 3) / 4);
    e.Raw("    uvec4 gas_regs[4];\n    uvec4 gas_table[4];\n};\n");

    e.Line("layout(set = {}, binding = {}, std430) readonly buffer LutLf {{ vec2 data[]; }} texture_buffer_lut_lf;",
           set::kUniforms, binding::kLutLf);
    e.Line("layout(set = {}, binding = {}, std430) readonly buffer LutRg {{ vec2 data[]; }} texture_buffer_lut_rg;",
           set::kUniforms, binding::kLutRg);
    e.Line("layout(set = {}, binding = {}, std430) readonly buffer LutRgba {{ vec4 data[]; }} texture_buffer_lut_rgba;",
           set::kUniforms, binding::kLutRgba);
    for (const auto& [binding, declaration] : std::array{
             std::pair{binding::kTex0, "texture2D tex0"}, std::pair{binding::kTex0Cube, "textureCube tex0_cube"},
             std::pair{binding::kTex0Shadow, "utexture2D tex0_shadow"}, std::pair{binding::kSampler0, "sampler samp0"},
             std::pair{binding::kTex1, "texture2D tex1"}, std::pair{binding::kSampler1, "sampler samp1"},
             std::pair{binding::kTex2, "texture2D tex2"}, std::pair{binding::kSampler2, "sampler samp2"},
             std::pair{binding::kTexColor, "texture2D tex_color"},
         }) {
        e.Line("layout(set = {}, binding = {}) uniform {};", set::kTextures, binding, declaration);
    }
    if (e.pass() == ShaderPass::ShadowWrite) {
        e.Line("layout(set = {}, binding = {}, r32ui) uniform uimage2D shadow_buffer;", set::kShadow, binding::kShadowOut);
    }

    for (std::string_view type : {"float", "vec2", "vec3", "vec4"}) {
        e.Line("{0} byteround({0} x) {{ return round(x * 255.0) / 255.0; }}", type);
    }
    e.Raw(R"glsl(
vec3 quaternion_rotate(vec4 q, vec3 v) {
    vec3 twice_cross = 2.0 * cross(q.xyz, v);
    return v + q.w * twice_cross + cross(q.xyz, twice_cross);
}
float getLod(vec2 uv) {
    vec2 extent = max(abs(dFdx(uv)), abs(dFdy(uv)));
    return log2(max(extent.x, extent.y));
}
uvec2 DecodeShadow(uint packed) {
    return uvec2(packed / 256u, packed % 256u);
}
vec4 sampleTexUnit0();
vec4 sampleTexUnit1();
vec4 sampleTexUnit2();
vec4 sampleTexUnit3();
vec4 ProcTex();
vec4 shadowTexture(vec2 uv, float w);
vec4 shadowTextureCube(vec2 uv, float w);

vec4 rounded_primary_color;
vec4 primary_fragment_color;
vec4 secondary_fragment_color;
vec4 combiner_buffer;
vec4 combiner_output;
)glsl");
}

void EmitUberTevHelpers(Emitter& e) {
    e.Raw("vec4 tev_source(uint selector, uint stage) {\n    switch (selector) {\n");
    for (u32 source = 0; source < kSources.size(); ++source) {
        if (!kSources[source].empty() || source == kConstantSource) {
            e.Line("    case {}u: return {};", source, SourceExpression(source, "stage"));
        }
    }
    e.Raw("    default: return vec4(0.0);\n    }\n}\n");

    for (bool alpha : {false, true}) {
        const char* type = alpha ? "float" : "vec3";
        const char* channel = alpha ? "alpha" : "color";
        e.Line("{} tev_{}_modifier(uint modifier, vec4 src) {{\n    switch (modifier) {{", type, channel);
        for (u32 modifier = 0; modifier < (alpha ? 8u : 16u); ++modifier) {
            e.Line("    case {}u: return {};", modifier, ModifierExpression(modifier, "src", alpha));
        }
        e.Line("    default: return {};\n    }}\n}}", Zero(alpha));

        e.Line("{0} tev_{1}_combiner(uint op, {0} a, {0} b, {0} c) {{\n    switch (op) {{", type, channel);
        for (u32 op = 0; op < kOperandCounts.size(); ++op) {
            if (OperandCount(op, alpha) != 0) {
                e.Line("    case {}u: return {};", op,
                       Quantize(CombineExpression(static_cast<Operation>(op), {"a", "b", "c"}, alpha), alpha));
            }
        }
        e.Line("    default: return {};\n    }}\n}}", Zero(alpha));
    }

    e.Raw("uint tev_operand_count(uint op) {\n    switch (op) {\n");
    for (u32 op = 0; op < kOperandCounts.size(); ++op) {
        e.Line("    case {}u: return {}u;", op, OperandCount(op, false));
    }
    e.Raw("    default: return 0u;\n    }\n}\n");
    e.Line("uint tev_operand_source(uint sources, uint stage, uint position) {{");
    e.Line("    uint source = (sources >> (4u * position)) & 15u;");
    e.Line("    if (stage == 0u && position < {}u && source == {}u) {{", kLastPosition, kPreviousSource);
    e.Line("        source = (sources >> {}u) & 15u;", kLastPosition * 4);
    e.Raw("    }\n    return source;\n}\n");
}

void EmitSpecialisedChannel(Emitter& e, const TevStageConfigRaw& stage, u32 index, bool alpha, bool legacy) {
    const auto operation = alpha ? stage.alpha_op() : stage.color_op();
    const char* channel = alpha ? "alpha" : "color";
    std::array<std::string, 3> operands;
    for (u32 i = 0; i < OperandCount(Number(operation), alpha); ++i) {
        const u32 source = SourceOf(stage, index, i, alpha);
        const u32 modifier = Bits(stage.modifiers_raw, (alpha ? 12u : 0u) + 4u * i, alpha ? 3 : 4);
        operands[i] = fmt::format("{}_results_{}", channel, i + 1);
        e.Line("{} {} = {};", alpha ? "float" : "vec3", operands[i],
               ModifierExpression(modifier, SourceExpression(source, std::to_string(index)), alpha));
    }

    const auto expression = CombineExpression(operation, operands, alpha);
    const bool quantized_previous = operation == Operation::Replace && SourceOf(stage, index, 0, alpha) == kPreviousSource &&
                                    Bits(stage.modifiers_raw, alpha ? 12u : 0u, alpha ? 3 : 4) == 0;
    e.Line("{} {}_result = {};", alpha ? "float" : "vec3", channel,
           !legacy && quantized_previous ? expression : Quantize(expression, alpha));
}

std::string ScaleExpression(std::string_view channel, u32 scale, bool alpha, bool legacy) {
    const u32 multiplier = TevMultiplier(scale);
    if (!legacy && multiplier == 1) {
        return std::string(channel);
    }
    return fmt::format("clamp({} * {}.0, {}, {})", channel, multiplier, Zero(alpha), One(alpha));
}

void EmitSpecialisedTev(Emitter& e, const FragmentKey& key) {
    const bool legacy = (key.user.raw & kUserShadersAsBefore) != 0;
    for (u32 index = 0; index < kStages; ++index) {
        const auto& stage = key.texture.tev_stages[index];
        if (!TevStageIsPassThrough(stage)) {
            e.Raw("{\n");
            EmitSpecialisedChannel(e, stage, index, false, legacy);
            if (stage.color_op() == Operation::Dot3_RGBA) {
                e.Raw("float alpha_result = color_result.r;\n");
            } else {
                EmitSpecialisedChannel(e, stage, index, true, legacy);
            }
            e.Line("combiner_output = vec4({}, {});", ScaleExpression("color_result", stage.color_scale(), false, legacy),
                   ScaleExpression("alpha_result", stage.alpha_scale(), true, legacy));
            e.Raw("}\n");
        }

        // The stage reads the previous snapshot before publishing the pending buffer for the next one.
        e.Raw("combiner_buffer = next_combiner_buffer;\n");
        if (key.TevStageUpdatesCombinerBufferColor(index)) {
            e.Raw("next_combiner_buffer.rgb = combiner_output.rgb;\n");
        }
        if (key.TevStageUpdatesCombinerBufferAlpha(index)) {
            e.Raw("next_combiner_buffer.a = combiner_output.a;\n");
        }
    }
}

void EmitUberTev(Emitter& e) {
    static_assert(keyword::kTexTevStages % 4 == 0 && keyword::kTevStageWords == 4);
    e.Line("for (uint stage = 0u; stage < {}u; ++stage) {{", kStages);
    e.Line("    uvec4 words = cfg[{}u + stage];", keyword::kTexTevStages / 4);
    e.Raw(R"glsl(    uint rgb_op = words.z & 15u;
    uint alpha_op = (words.z >> 16u) & 15u;
    uint rgb_scale = words.w & 3u;
    uint alpha_scale = (words.w >> 16u) & 3u;
)glsl");
    e.Line("    bool pass_through = (words.x & {}u) == {}u && (words.y & {}u) == 0u &&",
           kFirstSourceMask, kFirstSourceMask, kFirstModifiersMask);
    e.Line("                        (words.z & {}u) == 0u && (rgb_scale % 3u) == 0u && (alpha_scale % 3u) == 0u;",
           kOperationsMask);
    e.Raw(R"glsl(    if (!pass_through) {
        vec3 rgb[3] = vec3[3](vec3(0.0), vec3(0.0), vec3(0.0));
        for (uint i = 0u; i < tev_operand_count(rgb_op); ++i) {
            uint source = tev_operand_source(words.x, stage, i);
            uint modifier = (words.y >> (4u * i)) & 15u;
            rgb[i] = tev_color_modifier(modifier, tev_source(source, stage));
        }
        vec3 rgb_value = tev_color_combiner(rgb_op, rgb[0], rgb[1], rgb[2]);
        float alpha_value = rgb_value.r;
        if (rgb_op != 7u) {
            float a[3] = float[3](0.0, 0.0, 0.0);
            uint count = (alpha_op == 6u || alpha_op == 7u) ? 0u : tev_operand_count(alpha_op);
            for (uint i = 0u; i < count; ++i) {
                uint source = tev_operand_source(words.x >> 16u, stage, i);
                uint modifier = (words.y >> (12u + 4u * i)) & 7u;
                a[i] = tev_alpha_modifier(modifier, tev_source(source, stage));
            }
            alpha_value = tev_alpha_combiner(alpha_op, a[0], a[1], a[2]);
        }
        vec4 value = vec4(rgb_value * float(1u << (rgb_scale % 3u)),
                          alpha_value * float(1u << (alpha_scale % 3u)));
        combiner_output = clamp(value, 0.0, 1.0);
    }
    combiner_buffer = next_combiner_buffer;
)glsl");
    e.Line("    if (stage < {}u) {{", kBufferWriteStages);
    e.Line("        uint updates = {} >> stage;", e.Field(keyword::kTexRaw, 4, 8).glsl);
    e.Raw(R"glsl(        if ((updates & 1u) != 0u) next_combiner_buffer.rgb = combiner_output.rgb;
        if ((updates & 16u) != 0u) next_combiner_buffer.a = combiner_output.a;
    }
}
)glsl");
}

void EmitScissorAndDepth(Emitter& e, const Cond& identity_depth) {
    const Value scissor = e.Field(keyword::kFbRaw, 3, 2);
    const Cond include = e.Eq(scissor, Number(regenum::ScissorMode::Include));
    const Cond exclude = e.Eq(scissor, Number(regenum::ScissorMode::Exclude));
    e.If(e.Or(include, exclude), [&] {
        e.Raw(R"glsl(bool inside_scissor = all(greaterThanEqual(gl_FragCoord.xy, vec2(scissor_x1, scissor_y1))) &&
                      all(lessThan(gl_FragCoord.xy, vec2(scissor_x2, scissor_y2)));
)glsl");
        e.If(include, [&] { e.Raw("if (!inside_scissor) discard;\n"); },
                      [&] { e.Raw("if (inside_scissor) discard;\n"); });
    });

    e.Raw("float depth = gl_FragCoord.z;\n");
    e.If(e.Not(identity_depth), [&] {
        e.Raw("depth = -gl_FragCoord.z * depth_scale + depth_offset;\n");
        e.If(e.Eq(e.Field(keyword::kFbRaw, 5, 1), Number(regenum::DepthBuffering::WBuffering)),
             [&] { e.Raw("depth /= gl_FragCoord.w;\n"); });
    });
}

void EmitAlphaTest(Emitter& e, const Value& function) {
    auto discard_if = [&](std::string_view comparison) {
        e.Line("if (int(round(combiner_output.a * 255.0)) {} alphatest_ref) discard;", comparison);
    };
    e.Switch(function, {{Number(Compare::Equal), [&] { discard_if("!="); }},
                        {Number(Compare::NotEqual), [&] { discard_if("=="); }},
                        {Number(Compare::LessThan), [&] { discard_if(">="); }},
                        {Number(Compare::LessThanOrEqual), [&] { discard_if(">"); }},
                        {Number(Compare::GreaterThan), [&] { discard_if("<="); }},
                        {Number(Compare::GreaterThanOrEqual), [&] { discard_if("<"); }}});
}

struct BlendWords {
    Value equation;
    Value source;
    Value destination;
};

BlendWords ReadBlend(const Emitter& e, size_t first) {
    return {e.Word(first), e.Word(first + 1), e.Word(first + 2)};
}

Cond IsFactor(const Emitter& e, const Value& field, BlendFactor factor) {
    return e.Eq(field, Number(factor));
}

Cond EmulateMinMax(const Emitter& e, const BlendWords& blend) {
    if (e.profile().has_blend_minmax_factor) {
        return Emitter::False();
    }
    return e.And(e.Or(e.Eq(blend.equation, Number(BlendEquation::Min)), e.Eq(blend.equation, Number(BlendEquation::Max))),
                 e.Not(e.And(IsFactor(e, blend.source, BlendFactor::One), IsFactor(e, blend.destination, BlendFactor::One))));
}

Cond UsesSourceAlpha(const Emitter& e, const Value& factor) {
    return e.Or(e.Or(IsFactor(e, factor, BlendFactor::SourceAlpha), IsFactor(e, factor, BlendFactor::OneMinusSourceAlpha)),
                IsFactor(e, factor, BlendFactor::SourceAlphaSaturate));
}

Cond UsesBothBlendConstants(const Emitter& e, const BlendWords& rgb) {
    auto uses_color = [&](const Value& factor) {
        return e.Or(IsFactor(e, factor, BlendFactor::ConstantColor), IsFactor(e, factor, BlendFactor::OneMinusConstantColor));
    };
    auto uses_alpha = [&](const Value& factor) {
        return e.Or(IsFactor(e, factor, BlendFactor::ConstantAlpha), IsFactor(e, factor, BlendFactor::OneMinusConstantAlpha));
    };
    return e.Or(e.And(uses_color(rgb.source), uses_alpha(rgb.destination)),
                e.And(uses_alpha(rgb.source), uses_color(rgb.destination)));
}

Cond LogicReadsDestination(const Emitter& e, const Value& operation) {
    if (operation.known) {
        return Cond::Known(LogicOpReadsDestination(static_cast<LogicOp>(operation.literal)));
    }
    Cond reads = Emitter::False();
    for (u32 op = 0; op < 16; ++op) {
        if (LogicOpReadsDestination(static_cast<LogicOp>(op))) {
            reads = e.Or(reads, e.Eq(operation, op));
        }
    }
    return reads;
}

void EmitOutputHelpers(Emitter& e) {
    if (!e.profile().logic_op) {
        e.Raw(R"glsl(
vec4 mh4n_logic_op(uint op, vec4 src, vec4 dst) {
    const uint truth[16] = uint[16](0u, 8u, 4u, 12u, 15u, 3u, 12u, 5u,
                                    7u, 14u, 1u, 6u, 9u, 2u, 13u, 11u);
    uint table = truth[op];
    uvec4 s = uvec4(round(clamp(src, 0.0, 1.0) * 255.0));
    uvec4 d = uvec4(round(clamp(dst, 0.0, 1.0) * 255.0));
    // Each table bit selects bits with one of the four source/destination combinations.
    uvec4 result = (~s & ~d) & (0u - (table & 1u));
    result |= (~s & d) & (0u - ((table >> 1u) & 1u));
    result |= (s & ~d) & (0u - ((table >> 2u) & 1u));
    result |= (s & d) & (0u - ((table >> 3u) & 1u));
    return vec4(result & 255u) / 255.0;
}
)glsl");
    }
    e.Raw(R"glsl(
vec4 mh4n_blend_factor(uint factor, vec4 s, vec4 d) {
    if (factor < 2u) return vec4(float(factor));
    if (factor == 14u) return vec4(vec3(min(s.a, 1.0 - d.a)), 1.0);

    vec4 channels[6] = vec4[6](s, d, s.aaaa, d.aaaa, blend_color, blend_color.aaaa);
    uint index = (factor - 2u) / 2u;
    if (index >= 6u) return vec4(1.0);
    return (factor % 2u == 0u) ? channels[index] : (1.0 - channels[index]);
}
vec4 mh4n_blend(uint equation, uint sf, uint df, vec4 s, vec4 d) {
    vec4 x = s * mh4n_blend_factor(sf, s, d);
    vec4 y = d * mh4n_blend_factor(df, s, d);
    switch (equation) {
    case 0u: return min(x + y, 1.0);
    case 1u: return max(x - y, 0.0);
    case 2u: return max(y - x, 0.0);
    case 3u: return min(x, y);
    case 4u: return max(x, y);
    default: return s;
    }
}
)glsl");
}

void EmitOutput(Emitter& e, const Emitter& requested, const Cond& identity_depth) {
    if (!e.specialised() || !identity_depth.value) {
        // An ubershader statically writing depth must assign it for every key.
        e.Raw("gl_FragDepth = depth;\n");
    }
    e.Raw("color = byteround(combiner_output);\n");

    const Cond blend_enabled = e.Ne(e.Field(keyword::kFbRaw, 11, 1), 0);
    const auto rgb = ReadBlend(requested, keyword::kFbRgbBlend);
    const auto alpha = ReadBlend(requested, keyword::kFbAlphaBlend);
    const Cond blend_alpha = e.And(blend_enabled, EmulateMinMax(e, alpha));
    const Cond blend_rgb = e.And(blend_enabled, e.Or(EmulateMinMax(e, rgb), e.Or(UsesBothBlendConstants(e, rgb),
        e.And(blend_alpha, e.Or(UsesSourceAlpha(e, rgb.source), UsesSourceAlpha(e, rgb.destination))))));

    const Value operation = requested.Word(keyword::kFbLogicOp);
    const Cond logic = e.profile().logic_op ? Emitter::False() : e.And(e.Not(blend_enabled),
        e.And(e.Ne(operation, Number(LogicOp::Copy)), e.Ne(operation, Number(LogicOp::NoOp))));

    e.If(e.Or(blend_rgb, blend_alpha), [&] {
        e.Raw("vec4 source_color = color;\nvec4 destination_color = byteround(texelFetch(tex_color, ivec2(gl_FragCoord.xy), 0));\n");
        auto blend = [&](std::string_view lanes, const BlendWords& words) {
            e.Line("color.{0} = mh4n_blend({1}, {2}, {3}, source_color, destination_color).{0};", lanes,
                   words.equation.glsl, words.source.glsl, words.destination.glsl);
        };
        e.If(blend_rgb, [&] { blend("rgb", rgb); });
        e.If(blend_alpha, [&] { blend("a", alpha); });
    });

    e.If(logic, [&] {
        e.Raw("vec4 logic_dst = vec4(0.0);\n");
        e.If(LogicReadsDestination(e, operation),
             [&] { e.Raw("logic_dst = texelFetch(tex_color, ivec2(gl_FragCoord.xy), 0);\n"); });
        e.Line("color = mh4n_logic_op({}, color, logic_dst);", operation.glsl);
    });
}

std::string GenerateGlsl(Mode mode, const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass,
                         const FeatureRegistry& features) {
    FragmentKey applied = key;
    if (mode == Mode::Specialised) {
        applied.ApplyProfile(profile);
    }
    Emitter e(mode, applied, profile, pass);
    EmitContext context{applied, profile, mode, &e};
    auto emit_features = [&](Point point) {
        for (const auto* feature : features.All()) {
            feature->Emit(point, context);
        }
    };

    EmitDeclarations(e);
    emit_features(Point::Declarations);
    if (!e.specialised()) {
        EmitUberTevHelpers(e);
    }
    if (pass == ShaderPass::Normal) {
        EmitOutputHelpers(e);
    }

    const Value function = e.Field(keyword::kFbRaw, 0, 3);
    const Cond identity_depth = e.Ne(e.Field(keyword::kFbRaw, 12, 1), 0);
    e.Raw("void main() {\nrounded_primary_color = byteround(primary_color);\n");
    e.If(e.Eq(function, Number(Compare::Never)), [&] { e.Raw("discard;\n"); });
    emit_features(Point::Prologue);
    EmitScissorAndDepth(e, identity_depth);
    emit_features(Point::ScissorDepth);

    e.Raw("primary_fragment_color = vec4(0.0);\nsecondary_fragment_color = vec4(0.0);\n");
    emit_features(Point::Lighting);
    e.Raw("combiner_buffer = vec4(0.0);\ncombiner_output = vec4(0.0);\nvec4 next_combiner_buffer = tev_combiner_buffer_color;\n");
    emit_features(Point::Tev);
    if (e.specialised()) {
        EmitSpecialisedTev(e, applied);
    } else {
        EmitUberTev(e);
    }
    EmitAlphaTest(e, function);
    emit_features(Point::AlphaTest);
    emit_features(Point::FogGas);
    if (pass == ShaderPass::Normal) {
        const Emitter requested(mode, key, profile, pass);
        EmitOutput(e, requested, identity_depth);
    }
    emit_features(Point::Output);
    e.Raw("}\n");
    return std::move(e.text());
}

Generated CompileGenerated(std::string glsl) {
    Generated result;
    result.glsl = std::move(glsl);
    CompileFragmentGlslToSpirv(result.glsl, result.spirv, result.error, &result.compile_ms);
    return result;
}

} // namespace

bool TevStageIsPassThrough(const TevStageConfigRaw& stage) {
    return (stage.sources_raw & kFirstSourceMask) == kFirstSourceMask &&
           (stage.modifiers_raw & kFirstModifiersMask) == 0 && (stage.ops_raw & kOperationsMask) == 0 &&
           TevMultiplier(stage.color_scale()) == 1 && TevMultiplier(stage.alpha_scale()) == 1;
}

bool TevUsesSource(const FragmentKey& key, u32 source) {
    for (u32 index = 0; index < kStages; ++index) {
        const auto& stage = key.texture.tev_stages[index];
        if (TevStageIsPassThrough(stage)) {
            continue;
        }
        for (bool alpha : {false, true}) {
            if (alpha && stage.color_op() == Operation::Dot3_RGBA) {
                break;
            }
            const auto operation = alpha ? stage.alpha_op() : stage.color_op();
            for (u32 i = 0; i < OperandCount(Number(operation), alpha); ++i) {
                if (SourceOf(stage, index, i, alpha) == source) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::string GenerateSpecialisedGlsl(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass,
                                    const FeatureRegistry& features) {
    return GenerateGlsl(Mode::Specialised, key, profile, pass, features);
}

std::string GenerateSpecialisedGlsl(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass) {
    return GenerateSpecialisedGlsl(key, profile, pass, FeatureRegistry::Global());
}

std::string GenerateUberGlsl(ShaderPass pass, const DeviceProfile& profile, const FeatureRegistry& features) {
    return GenerateGlsl(Mode::Uber, {}, profile, pass, features);
}

std::string GenerateUberGlsl(ShaderPass pass, const DeviceProfile& profile) {
    return GenerateUberGlsl(pass, profile, FeatureRegistry::Global());
}

Generated GenerateSpecialised(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass,
                              const FeatureRegistry& features) {
    return CompileGenerated(GenerateSpecialisedGlsl(key, profile, pass, features));
}

Generated GenerateSpecialised(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass) {
    return GenerateSpecialised(key, profile, pass, FeatureRegistry::Global());
}

Generated GenerateUber(ShaderPass pass, const DeviceProfile& profile, const FeatureRegistry& features) {
    return CompileGenerated(GenerateUberGlsl(pass, profile, features));
}

Generated GenerateUber(ShaderPass pass, const DeviceProfile& profile) {
    return GenerateUber(pass, profile, FeatureRegistry::Global());
}

} // namespace mh4n::gpu::fragment
