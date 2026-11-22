// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: vertex_shader.h, vertex_loader.h, gpu_shader_test.cpp and golden/gpu.

#include "vertex_shader.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "glsl_decompiler.h"

namespace mh4n::gpu::shader {
namespace {

constexpr u32 kUnmapped = 16;
constexpr std::string_view kInitialOutput = "vec4(0.0, 0.0, 0.0, 1.0)";
constexpr std::string_view kUniformFields = "    uint b;\n    uvec4 i[4];\n    vec4 f[96];\n";

u64 Mix(u64 word) {
    word = (word ^ (word >> 30)) * 0xbf58476d1ce4e5b9ull;
    word = (word ^ (word >> 27)) * 0x94d049bb133111ebull;
    return word ^ (word >> 31);
}

u64 Fold(u64 hash, u64 word) {
    return Mix(hash + 0x9e3779b97f4a7c15ull + word);
}

struct WordHash {
    u64 serial = 0;
    size_t size = 0;
    u64 hash = 0;

    u64 Get(const u32* words, size_t count, u64 version) {
        if (version == 0 || serial != version || size != count) {
            *this = {version, count, HashWords(words, count)};
        }
        return hash;
    }
};

std::string Output(u32 attribute) {
    return "vs_out_attr" + std::to_string(attribute);
}

void MapOutputs(const VertexShaderConfig& config, TranslatedVertexShader& shader) {
    shader.output_map.fill(kUnmapped);
    for (u32 reg = 0; reg < shader.output_map.size(); ++reg) {
        if (config.output_mask & (1u << reg)) {
            shader.output_map[reg] = static_cast<u8>(shader.num_outputs++);
        }
    }
    const u32 count = std::min(config.outmap_total, static_cast<u32>(config.outmap_attributes.size()));
    for (u32 attribute = 0; attribute < count; ++attribute) {
        const u32 word = config.outmap_attributes[attribute];
        for (u32 lane = 0; lane < 4; ++lane) {
            const u32 id = (word >> (8 * lane)) & 31u;
            if (id < shader.semantic_map.size()) {
                shader.semantic_map[id] = {
                    static_cast<u8>(attribute < shader.num_outputs ? attribute : kUnmapped),
                    static_cast<u8>(attribute < shader.num_outputs ? lane : 0),
                };
            }
        }
    }
}

std::string Semantic(const TranslatedVertexShader& shader, u32 id) {
    const auto [attribute, component] = shader.semantic_map[id];
    return attribute == kUnmapped ? "1.0" : Output(attribute) + '.' + "xyzw"[component];
}

std::string SemanticVector(const TranslatedVertexShader& shader, u32 first, u32 lanes) {
    std::string result = "vec" + std::to_string(lanes) + "(" + Semantic(shader, first);
    for (u32 lane = 1; lane < lanes; ++lane) {
        result += ", " + Semantic(shader, first + lane);
    }
    return result + ')';
}

std::string Layout(u32 binding, std::string_view alignment) {
    return "layout(set = " + std::to_string(kDescriptorSet) + ", binding = " + std::to_string(binding) +
           ", " + std::string(alignment) + ") ";
}

std::string PullBuffer(bool writable) {
    return Layout(kBindingPullWords, "std430") + (writable ? "" : "readonly ") +
           "buffer VertexWords { uint words[]; };\n";
}

std::string PassPreamble() {
    std::string glsl = "#version 450\nlayout(local_size_x = " + std::to_string(kPullComputeGroup) + ") in;\n";
    glsl += Layout(kBindingPassParams, "std140") + "uniform PassParams { uvec4 pass_params; };\n";
    glsl += PullBuffer(true);
    constexpr std::pair<std::string_view, u32> constants[] = {
        {"JOB_WORDS", kPullJobWords},
        {"JOB_UNIFORMS", kPullJobUniforms},
        {"JOB_HEADER", kPullJobHeader},
        {"JOB_FIRST_VERTEX", kPullJobFirstVertex},
        {"JOB_VERTICES", kPullJobVertices},
        {"JOB_QUATERNIONS", kPullJobQuaternions},
        {"JOB_INDEX_LIST", kPullJobIndexList},
        {"JOB_CORNER_COUNT", kPullJobCornerCount},
        {"JOB_INDICES", kPullJobIndices},
        {"JOB_QUATERNION_GROUP", kPullJobQuaternionGroup},
        {"JOB_FLIP_GROUP", kPullJobFlipGroup},
        {"JOB_INDEX_FORM", kPullJobIndexForm},
        {"JOB_BASE_VERTEX", kPullJobBaseVertex},
        {"JOB_TRIANGLE_TABLE", kPullJobTriangleTable},
        {"INDEX_NONE", kPullIndexNone},
        {"INDEX_U8", kPullIndexU8},
        {"INDEX_U16", kPullIndexU16},
        {"TRIANGLE_LIST", kPullTriangleList},
        {"TRIANGLE_STRIP", kPullTriangleStrip},
        {"HAS_TRIANGLE_TABLE", kPullTriangleTable},
    };
    for (const auto& [name, value] : constants) {
        glsl += "const uint ";
        glsl += name;
        glsl += " = " + std::to_string(value) + "u;\n";
    }
    glsl += R"glsl(
uint job_record() {
    return pass_params.x + JOB_WORDS * words[pass_params.y + gl_WorkGroupID.x];
}

uint job_item(uint first_group) {
    return (gl_WorkGroupID.x - first_group) * gl_WorkGroupSize.x + gl_LocalInvocationIndex;
}
)glsl";
    return glsl;
}

constexpr std::string_view kPullRead = R"glsl(
// The buffer is word-addressed, but attributes can start at any byte.
uint read_component(uint byte_offset, uint width) {
    uint word = byte_offset >> 2u;
    uint shift = (byte_offset & 3u) * 8u;
    uint bits = words[word] >> shift;
    if (shift + width > 32u) {
        bits |= words[word + 1u] << (32u - shift);
    }
    return bits;
}
)glsl";

std::string PullLane(vertex::ComponentKind kind, const std::string& address) {
    using vertex::ComponentKind;
    switch (kind) {
    case ComponentKind::S8:
        return "float(bitfieldExtract(int(read_component(" + address + ", 8u)), 0, 8))";
    case ComponentKind::U8:
        return "float(read_component(" + address + ", 8u) & 255u)";
    case ComponentKind::S16:
        return "float(bitfieldExtract(int(read_component(" + address + ", 16u)), 0, 16))";
    case ComponentKind::F32:
        return "uintBitsToFloat(read_component(" + address + ", 32u))";
    }
    return "0.0";
}

void DeclareInputs(std::string& glsl, const VertexShaderConfig& config, u16 used_inputs) {
    for (u32 reg = 0; reg < config.inputs.size(); ++reg) {
        if ((used_inputs & (1u << reg)) == 0) {
            continue;
        }
        const auto id = std::to_string(reg);
        if (!config.pull_fixup) {
            constexpr std::string_view types[] = {"vec4", "ivec4", "uvec4"};
            const auto type = types[static_cast<u32>(config.inputs[reg].conversion)];
            glsl += "layout(location = " + id + ") in " + std::string(type) + " vs_in_typed_reg" + id + ";\n";
        }
        glsl += "vec4 vs_in_reg" + id + ";\n";
    }
}

void InitInputs(std::string& glsl, const VertexShaderConfig& config, u16 used_inputs) {
    for (u32 reg = 0; reg < config.inputs.size(); ++reg) {
        if ((used_inputs & (1u << reg)) == 0) {
            continue;
        }
        const auto id = std::to_string(reg);
        const auto& input = config.inputs[reg];
        if (config.pull_fixup) {
            const auto& pull = config.pull[reg];
            const auto format = vertex::GetFormatInfo(pull.format);
            const auto step = format.size_bytes / format.lanes;
            glsl += "    uint address" + id + " = words[header + " + std::to_string(pull.binding) +
                    "u] + " + std::to_string(pull.offset) + "u";
            if (!pull.per_instance) {
                glsl += " + vertex * " + std::to_string(pull.stride) + "u";
            }
            glsl += ";\n    vs_in_reg" + id + " = vec4(";
            for (u32 lane = 0; lane < 4; ++lane) {
                if (lane != 0) {
                    glsl += ", ";
                }
                if (lane >= format.lanes || (lane == 3 && input.alias_w)) {
                    glsl += lane == 3 ? "1.0" : "0.0";
                } else {
                    glsl += PullLane(format.kind, "address" + id + " + " + std::to_string(lane * step) + "u");
                }
            }
            glsl += ");\n";
        } else {
            glsl += "    vs_in_reg" + id + " = vec4(vs_in_typed_reg" + id + ");\n";
            if (input.alias_w) {
                glsl += "    vs_in_reg" + id + ".w = 1.0;\n";
            }
        }
    }
}

void DeclareVertexOutputs(std::string& glsl) {
    struct Varying {
        u32 location;
        std::string_view declaration;
    };
    constexpr Varying varyings[] = {
        {kLocationPrimaryColor, "vec4 primary_color"},
        {kLocationTexcoord0, "vec2 texcoord0"},
        {kLocationTexcoord1, "vec2 texcoord1"},
        {kLocationTexcoord2, "vec2 texcoord2"},
        {kLocationTexcoord0W, "float texcoord0_w"},
        {kLocationNormquat, "vec4 normquat"},
        {kLocationView, "vec3 view"},
    };
    for (const auto& [location, declaration] : varyings) {
        glsl += "layout(location = " + std::to_string(location) + ") out ";
        glsl += declaration;
        glsl += ";\n";
    }
}

void EmitVertexOutputs(std::string& glsl, const VertexShaderConfig& config, const TranslatedVertexShader& shader) {
    using namespace semantic;
    glsl += "    vec4 pos = " + SemanticVector(shader, kPositionX, 4) + ";\n";
    glsl += R"glsl(
    float depth = pos.z / pos.w;
    if (depth > 0.0 && depth < 0.000001) pos.z = 0.0;
    if (depth < -1.0 && depth > -1.00001) pos.z = -pos.w;
    if (flip_viewport != 0u) pos.y = -pos.y;
    gl_Position = vec4(pos.xy, -pos.z, pos.w);
)glsl";
    if (config.use_clip_planes) {
        glsl += R"glsl(
    gl_ClipDistance[0] = -pos.z;
    gl_ClipDistance[1] = enable_clip1 == 0u ? 0.0 : dot(clip_coef, pos);
)glsl";
    }
    glsl += "    vec4 color = abs(" + SemanticVector(shader, kColorR, 4) + ");\n";
    glsl += "    primary_color = mix(vec4(1.0), color, lessThan(color, vec4(1.0)));\n";
    glsl += "    texcoord0 = " + SemanticVector(shader, kTexcoord0U, 2) + ";\n";
    glsl += "    texcoord1 = " + SemanticVector(shader, kTexcoord1U, 2) + ";\n";
    glsl += "    texcoord2 = " + SemanticVector(shader, kTexcoord2U, 2) + ";\n";
    glsl += "    texcoord0_w = " + Semantic(shader, kTexcoord0W) + ";\n";
    glsl += "    view = " + SemanticVector(shader, kViewX, 3) + ";\n";
    glsl += "    normquat = " + SemanticVector(shader, kQuaternionX, 4) + ";\n";
    if (config.pull_fixup) {
        glsl += "    if ((code & 1u) != 0u) normquat = -normquat;\n";
    }
}

void EmitCapture(std::string& glsl, const VertexShaderConfig& config, u32 num_outputs) {
    glsl += "    uint capture_base = uint(gl_VertexIndex) * " + std::to_string(kCaptureVec4PerVertex) + "u;\n";
    const auto store = [&](u32 slot, std::string_view value) {
        glsl += "    capture[capture_base + " + std::to_string(slot) + "u] = ";
        glsl += value;
        glsl += ";\n";
    };
    for (u32 i = 0; i < kUnmapped; ++i) {
        store(kCaptureOutputs + i, i < num_outputs ? Output(i) : "vec4(0.0)");
    }
    if (config.geometry_stage) {
        for (u32 slot = kCapturePrimaryColor; slot < kCaptureVec4PerVertex; ++slot) {
            store(slot, "vec4(0.0)");
        }
    } else {
        store(kCapturePrimaryColor, "primary_color");
        store(kCaptureTexcoord01, "vec4(texcoord0, texcoord1)");
        store(kCaptureTexcoord2W, "vec4(texcoord2, texcoord0_w, 0.0)");
        store(kCaptureNormquat, "normquat");
        store(kCaptureView, "vec4(view, 0.0)");
        store(kCapturePosition, "gl_Position");
        store(kCaptureClip, config.use_clip_planes ?
              "vec4(gl_ClipDistance[0], gl_ClipDistance[1], 0.0, 0.0)" : "vec4(0.0)");
        store(kCaptureClip + 1, "vec4(0.0)");
    }
}

constexpr std::string_view kQuaternionMain = R"glsl(
void main() {
    uint job = job_record();
    uint item = job_item(words[job + JOB_QUATERNION_GROUP]);
    uint uniform_base = words[job + JOB_UNIFORMS];
    uint lane = gl_LocalInvocationIndex;
    if (lane == 0u) uniforms.b = uniform_words[uniform_base];
    if (lane < 4u) {
        uint src = uniform_base + 4u + 4u * lane;
        uniforms.i[lane] = uvec4(uniform_words[src], uniform_words[src + 1u],
                                  uniform_words[src + 2u], uniform_words[src + 3u]);
    }
    for (uint i = lane; i < 96u; i += gl_WorkGroupSize.x) {
        uint src = uniform_base + 20u + 4u * i;
        uniforms.f[i] = uintBitsToFloat(uvec4(uniform_words[src], uniform_words[src + 1u],
                                              uniform_words[src + 2u], uniform_words[src + 3u]));
    }
    barrier();
    if (item >= words[job + JOB_VERTICES]) return;
    uint header = words[job + JOB_HEADER];
    uint vertex = words[job + JOB_FIRST_VERTEX] + item;
)glsl";

u32 QuaternionOutputs(const TranslatedVertexShader& shader) {
    u32 mask = 0;
    for (u32 id = semantic::kQuaternionX; id <= semantic::kQuaternionW; ++id) {
        const auto attribute = shader.semantic_map[id].attribute;
        if (attribute != kUnmapped) {
            mask |= 1u << attribute;
        }
    }
    return mask;
}

std::string AssembleGlsl(const VertexShaderConfig& config, const TranslatedVertexShader& shader,
                         std::string_view body, u32 live_outputs, bool compute) {
    std::string glsl;
    if (compute) {
        glsl = PassPreamble();
        glsl += Layout(kBindingPassUniformWords, "std430") +
                "readonly buffer PassUniforms { uint uniform_words[]; };\n";
        glsl += "struct PicaUniforms {\n";
        glsl += kUniformFields;
        glsl += "};\nshared PicaUniforms uniforms;\n";
    } else {
        glsl = "#version 450\n";
        glsl += Layout(kBindingPicaUniforms, "std140") + "uniform PicaUniforms {\n";
        glsl += kUniformFields;
        glsl += "} uniforms;\n";
        if (!config.geometry_stage) {
            glsl += Layout(kBindingVertexData, "std140") +
                    "uniform VertexData { uint enable_clip1; uint flip_viewport; vec4 clip_coef; };\n";
            DeclareVertexOutputs(glsl);
        }
        if (config.pull_fixup) {
            glsl += PullBuffer(false);
        }
        if (config.capture_outputs) {
            glsl += Layout(kBindingCapture, "std430") + "writeonly buffer Capture { vec4 capture[]; };\n";
        }
    }
    if (config.pull_fixup) {
        glsl += kPullRead;
    }
    DeclareInputs(glsl, config, shader.used_inputs);
    for (u32 attribute = 0; attribute < shader.num_outputs; ++attribute) {
        if (live_outputs & (1u << attribute)) {
            if (!compute && config.geometry_stage) {
                glsl += "layout(location = " + std::to_string(attribute) + ") out ";
            }
            glsl += "vec4 " + Output(attribute) + ";\n";
        }
    }
    glsl += body;

    if (compute) {
        glsl += kQuaternionMain;
    } else {
        glsl += "\nvoid main() {\n";
        if (config.pull_fixup) {
            glsl += "    uint code = uint(gl_VertexIndex);\n"
                    "    uint vertex = code >> 1u;\n"
                    "    uint header = uint(gl_InstanceIndex);\n";
        }
    }
    InitInputs(glsl, config, shader.used_inputs);
    for (u32 attribute = 0; attribute < shader.num_outputs; ++attribute) {
        if (live_outputs & (1u << attribute)) {
            glsl += "    " + Output(attribute) + " = ";
            glsl += kInitialOutput;
            glsl += ";\n";
        }
    }
    glsl += "    exec_shader();\n";

    if (compute) {
        glsl += "    uvec4 quat = floatBitsToUint(" + SemanticVector(shader, semantic::kQuaternionX, 4) + ");\n";
        glsl += R"glsl(
    uint dst = words[job + JOB_QUATERNIONS] + 4u * item;
    for (uint i = 0u; i < 4u; ++i) words[dst + i] = quat[i];
)glsl";
    } else {
        if (!config.geometry_stage) {
            EmitVertexOutputs(glsl, config, shader);
        }
        if (config.capture_outputs) {
            glsl += "    gl_PointSize = 1.0;\n";
            if (config.geometry_stage) {
                glsl += "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n";
            }
            EmitCapture(glsl, config, shader.num_outputs);
        }
    }
    glsl += "}\n";
    return glsl;
}

} // namespace

void WritePicaUniformData(const pica::ShaderSetup& setup, PicaUniformData* out) {
    out->b = 0;
    std::fill_n(out->pad0, 3, 0u);
    for (u32 reg = 0; reg < setup.uniform_b.size(); ++reg) {
        out->b |= static_cast<u32>(setup.uniform_b[reg]) << reg;
    }
    for (u32 reg = 0; reg < setup.uniform_i.size(); ++reg) {
        std::copy_n(setup.uniform_i[reg].begin(), 4, out->i[reg]);
    }
    for (u32 reg = 0; reg < setup.uniform_f.size(); ++reg) {
        std::copy_n(setup.uniform_f[reg].c, 4, out->f[reg]);
    }
}

PicaUniformData MakePicaUniformData(const pica::ShaderSetup& setup) {
    PicaUniformData data;
    WritePicaUniformData(setup, &data);
    return data;
}

VertexUniformData MakeVertexUniformData(const pica::PicaState& state, bool flip_viewport) {
    VertexUniformData data;
    data.enable_clip1 = state.regs[reg::kClipEnable] & 1u;
    data.flip_viewport = flip_viewport;
    for (u32 lane = 0; lane < 4; ++lane) {
        data.clip_coef[lane] = pica::Float24ToFloat(state.regs[reg::kClipCoef + lane] & 0xffffffu);
    }
    return data;
}

u64 HashWords(const u32* words, size_t count) {
    u64 hash = Mix(count);
    for (size_t i = 0; i < count; ++i) {
        hash = Fold(hash, words[i]);
    }
    return hash;
}

u64 VertexShaderConfig::Hash() const {
    u64 hash = Fold(program_hash, swizzle_hash);
    for (const u32 word : {program_words, swizzle_words, main_offset, output_mask, outmap_total}) {
        hash = Fold(hash, word);
    }
    for (const u32 word : outmap_attributes) {
        hash = Fold(hash, word);
    }
    for (const auto& input : inputs) {
        hash = Fold(hash, static_cast<u32>(input.conversion) | (static_cast<u32>(input.alias_w) << 8));
    }
    hash = Fold(hash, (use_clip_planes ? 1u : 0u) | (geometry_stage ? 2u : 0u) | (capture_outputs ? 4u : 0u) |
                      (exact_arithmetic ? 8u : 0u) | (pull_fixup ? 16u : 0u));
    if (pull_fixup) {
        for (const auto& attribute : pull) {
            hash = Fold(hash, (static_cast<u64>(attribute.stride) << 40) |
                              (static_cast<u64>(attribute.offset) << 24) |
                              (static_cast<u32>(attribute.per_instance) << 16) |
                              (static_cast<u32>(attribute.format) << 8) | attribute.binding);
        }
    }
    return hash;
}

VertexShaderConfig MakeVertexShaderConfig(const pica::PicaState& state, const vertex::LoadedDraw* loaded,
                                          const ConfigOptions& options) {
    const auto& setup = state.vs;
    VertexShaderConfig config;
    config.program_words = setup.biggest_program_size;
    config.swizzle_words = setup.biggest_swizzle_size;
    thread_local WordHash code;
    thread_local WordHash swizzle;
    config.program_hash = code.Get(setup.program_code.data(),
                                   std::min<size_t>(config.program_words, setup.program_code.size()),
                                   setup.program_serial);
    config.swizzle_hash = swizzle.Get(setup.swizzle_data.data(),
                                     std::min<size_t>(config.swizzle_words, setup.swizzle_data.size()),
                                     setup.swizzle_serial);
    config.main_offset = state.regs[pica::reg::kVs + pica::reg::kShaderMainOffset] & 0xffffu;
    config.output_mask = state.regs[pica::reg::kVs + pica::reg::kShaderOutputMask] & 0xffffu;
    config.outmap_total = state.regs[reg::kVsOutputTotal] & 7u;
    std::copy_n(state.regs.begin() + reg::kVsOutputAttributes, config.outmap_attributes.size(),
                config.outmap_attributes.begin());
    config.use_clip_planes = options.use_clip_planes;
    config.geometry_stage = options.geometry_stage;
    config.capture_outputs = options.capture_outputs;
    config.exact_arithmetic = options.exact_arithmetic;
    config.pull_fixup = options.pull_fixup && loaded != nullptr && !options.geometry_stage;
    if (loaded) {
        for (u32 reg = 0; reg < loaded->attributes.size(); ++reg) {
            const auto& attr = loaded->attributes[reg];
            config.inputs[reg] = {attr.conversion, attr.alias_w};
            if (config.pull_fixup && attr.binding < loaded->buffers.size()) {
                const auto& buffer = loaded->buffers[attr.binding];
                config.pull[reg] = {
                    .binding = static_cast<u8>(attr.binding),
                    .format = attr.format,
                    .per_instance = static_cast<u8>(buffer.per_instance),
                    .offset = static_cast<u16>(attr.offset),
                    .stride = static_cast<u16>(buffer.stride),
                };
            }
        }
    }
    return config;
}

namespace {

enum class ShaderForm { VertexGlsl, VertexSpirv, QuaternionGlsl };

TranslateResult BuildShader(const pica::ShaderSetup& setup, const VertexShaderConfig& config, ShaderForm form) {
    const bool quaternion_compute = form == ShaderForm::QuaternionGlsl;
    if (quaternion_compute && !config.pull_fixup) {
        return {nullptr, "The quaternion pass requires vertex pulling", "quaternion-config"};
    }
    if (config.program_words > setup.program_code.size() || config.swizzle_words > setup.swizzle_data.size()) {
        return {nullptr, "Loaded shader data exceeds PICA memory", "program-size"};
    }

    auto shader = std::make_shared<TranslatedVertexShader>();
    MapOutputs(config, *shader);
    const u32 live_outputs = quaternion_compute ? QuaternionOutputs(*shader) : (1u << shader->num_outputs) - 1u;
    DecompileResult body = DecompileProgram(
        {{setup.program_code.data(), config.program_words}, {setup.swizzle_data.data(), config.swizzle_words},
         config.main_offset, config.exact_arithmetic},
        [&](u32 reg) {
            shader->used_inputs |= static_cast<u16>(1u << reg);
            return "vs_in_reg" + std::to_string(reg);
        },
        [&](u32 reg) {
            const u32 attribute = shader->output_map[reg];
            return (live_outputs & (1u << attribute)) ? Output(attribute) : std::string{};
        });
    if (!body.ok) {
        return {nullptr, std::move(body.error), std::move(body.error_category), body.error_word};
    }
    shader->notes = std::move(body.notes);
    shader->uses_unverified_instructions = body.uses_unverified_instructions;
    shader->differs_from_interpreter = body.differs_from_interpreter;
    shader->glsl = AssembleGlsl(config, *shader, body.source, live_outputs, quaternion_compute);
    if (form == ShaderForm::VertexSpirv) {
        std::string error;
        if (!CompileGlslToSpirv(shader->glsl, shader->spirv, error, &shader->compile_ms)) {
            return {nullptr, std::move(error), "compile"};
        }
    }
    return {.shader = std::move(shader), .error = {}, .error_category = {}};
}

} // namespace

TranslateResult GenerateVertexGlsl(const pica::ShaderSetup& setup, const VertexShaderConfig& config,
                                   bool quaternion_compute) {
    return BuildShader(setup, config, quaternion_compute ? ShaderForm::QuaternionGlsl : ShaderForm::VertexGlsl);
}

TranslateResult TranslateVertexShader(const pica::ShaderSetup& setup, const VertexShaderConfig& config) {
    return BuildShader(setup, config, ShaderForm::VertexSpirv);
}

std::string FlipPassGlsl() {
    return PassPreamble() + R"glsl(
uint read_index(uint list_base, uint form, uint i) {
    if (form == INDEX_NONE) return i;
    if (form == INDEX_U8) return (words[list_base + (i >> 2u)] >> (8u * (i & 3u))) & 255u;
    if (form == INDEX_U16) return (words[list_base + (i >> 1u)] >> (16u * (i & 1u))) & 65535u;
    return words[list_base + i];
}

uvec3 triangle_slots(uint topology, uint triangle) {
    if (topology == TRIANGLE_LIST) return 3u * triangle + uvec3(0u, 1u, 2u);
    if (topology == TRIANGLE_STRIP) {
        uint odd = triangle & 1u;
        return triangle + uvec3(0u, 1u + odd, 2u - odd);
    }
    return uvec3(triangle + 1u, triangle + 2u, 0u);
}

vec4 quaternion(uint base, uint first_vertex, uint vertex) {
    uint offset = base + 4u * (vertex - first_vertex);
    return uintBitsToFloat(uvec4(words[offset], words[offset + 1u], words[offset + 2u], words[offset + 3u]));
}

void main() {
    uint job = job_record();
    uint item = job_item(words[job + JOB_FLIP_GROUP]);
    if (item >= words[job + JOB_CORNER_COUNT]) return;
    uint form = words[job + JOB_INDEX_FORM];
    uint triangle = item / 3u;
    if ((form & HAS_TRIANGLE_TABLE) != 0u) {
        triangle = words[words[job + JOB_TRIANGLE_TABLE] + triangle];
    }
    uvec3 slots = triangle_slots((form >> 2u) & 3u, triangle);
    uint corner = item % 3u;
    uint base_vertex = words[job + JOB_BASE_VERTEX];
    uint list_base = words[job + JOB_INDEX_LIST];
    uint vertex = base_vertex + read_index(list_base, form & 3u, slots[corner]);
    uint flip = 0u;
    if (corner != 0u) {
        uint first = base_vertex + read_index(list_base, form & 3u, slots.x);
        uint quat_base = words[job + JOB_QUATERNIONS];
        uint first_vertex = words[job + JOB_FIRST_VERTEX];
        vec4 reference = quaternion(quat_base, first_vertex, first);
        vec4 current = quaternion(quat_base, first_vertex, vertex);
        if (dot(reference, current) < 0.0) flip = 1u;
    }
    words[words[job + JOB_INDICES] + item] = 2u * vertex + flip;
}
)glsl";
}

TranslateResult VertexShaderCache::Get(const pica::ShaderSetup& setup, const VertexShaderConfig& config) {
    {
        const std::lock_guard lock(mutex_);
        ++stats_.lookups;
        const auto entry = entries_.find(config);
        if (entry != entries_.end()) {
            ++stats_.hits;
            return entry->second;
        }
        ++stats_.compiles;
    }

    auto result = TranslateVertexShader(setup, config);
    const std::lock_guard lock(mutex_);
    if (!result.ok()) {
        ++stats_.failures;
    }
    return entries_.try_emplace(config, std::move(result)).first->second;
}

VertexShaderCache::Stats VertexShaderCache::stats() const {
    const std::lock_guard lock(mutex_);
    return stats_;
}

size_t VertexShaderCache::size() const {
    const std::lock_guard lock(mutex_);
    return entries_.size();
}

void VertexShaderCache::Clear() {
    const std::lock_guard lock(mutex_);
    entries_.clear();
}

} // namespace mh4n::gpu::shader
