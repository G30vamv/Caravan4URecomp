// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: lighting/fixup.h, shader/vertex_shader.h, gpu_lighting_test black-box cases and golden/gpu/glsl vertex traces.

#include "lighting/fixup.h"

#include <algorithm>
#include <initializer_list>

#include <fmt/format.h>
#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include "gpu/shader/vertex_shader.h"

namespace mh4n::gpu::fragment::lighting {
namespace {

std::string Attribute(const FixupStageInputs& inputs, u32 semantic) {
    const auto [attribute, component] = inputs.semantic_map[semantic];
    if (attribute >= std::min(inputs.num_outputs, 16u) || component >= 4) {
        return "1.0";
    }
    return fmt::format("vs_out_attr{}[vertex].{}", attribute, "xyzw"[component]);
}

std::string Varying(const FixupStageInputs& inputs, std::initializer_list<u32> semantics) {
    std::string result;
    if (semantics.size() > 1) result = fmt::format("vec{}(", semantics.size());
    for (u32 semantic : semantics) {
        result += Attribute(inputs, semantic);
        if (semantic != *(semantics.end() - 1)) result += ", ";
    }
    if (semantics.size() > 1) result += ')';
    return result;
}

} // namespace

std::string GenerateFixupGeometryGlsl(const FixupStageInputs& inputs) {
    using namespace shader::semantic;
    std::string glsl = R"glsl(#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(set = 0, binding = 1, std140) uniform VertexData {
    uint enable_clip1;
    uint flip_viewport;
    vec4 clip_coef;
};
layout(location = 1) out vec4 primary_color;
layout(location = 2) out vec2 texcoord0;
layout(location = 3) out vec2 texcoord1;
layout(location = 4) out vec2 texcoord2;
layout(location = 5) out float texcoord0_w;
layout(location = 6) out vec4 normquat;
layout(location = 7) out vec3 view;
)glsl";
    for (u32 i = 0; i < std::min(inputs.num_outputs, 16u); ++i) {
        glsl += fmt::format("layout(location = {0}) in vec4 vs_out_attr{0}[];\n", i);
    }
    glsl += fmt::format("vec4 GetVertexQuaternion(int vertex) {{ return {}; }}\n",
                         Varying(inputs, {kQuaternionX, kQuaternionY, kQuaternionZ, kQuaternionW}));
    glsl += R"glsl(
bool AreQuaternionsOpposite(vec4 first, vec4 other) {
    return dot(first, other) < 0.0;
}
void WriteVertex(int vertex, bool reverse_quaternion) {
)glsl";
    glsl += fmt::format("    vec4 pos = {};\n", Varying(inputs, {kPositionX, kPositionY, kPositionZ, kPositionW}));
    glsl += R"glsl(
    float ndc_z = pos.z / pos.w;
    if (ndc_z > 0.0 && ndc_z < 0.000001) pos.z = 0.0;
    if (ndc_z < -1.0 && ndc_z > -1.00001) pos.z = -pos.w;
    if (flip_viewport != 0u) pos.y = -pos.y;
    gl_Position = vec4(pos.xy, -pos.z, pos.w);
)glsl";
    if (inputs.clip_distance) {
        glsl += "    gl_ClipDistance[0] = -pos.z;\n"
                "    gl_ClipDistance[1] = enable_clip1 != 0u ? dot(clip_coef, pos) : 0.0;\n";
    }
    glsl += fmt::format("    vec4 rgba = abs({});\n", Varying(inputs, {kColorR, kColorG, kColorB, kColorA}));
    glsl += "    primary_color = mix(vec4(1.0), rgba, lessThan(rgba, vec4(1.0)));\n";
    glsl += fmt::format("    texcoord0 = {};\n    texcoord1 = {};\n    texcoord2 = {};\n"
                        "    texcoord0_w = {};\n    view = {};\n",
                        Varying(inputs, {kTexcoord0U, kTexcoord0V}), Varying(inputs, {kTexcoord1U, kTexcoord1V}),
                        Varying(inputs, {kTexcoord2U, kTexcoord2V}), Attribute(inputs, kTexcoord0W),
                        Varying(inputs, {kViewX, kViewY, kViewZ}));
    glsl += R"glsl(
    normquat = GetVertexQuaternion(vertex);
    if (reverse_quaternion) normquat = -normquat;
    EmitVertex();
}
void main() {
    const int vtx0 = 0, vtx1 = 1, vtx2 = 2;
    WriteVertex(vtx0, false);
    WriteVertex(vtx1, AreQuaternionsOpposite(GetVertexQuaternion(vtx0), GetVertexQuaternion(vtx1)));
    WriteVertex(vtx2, AreQuaternionsOpposite(GetVertexQuaternion(vtx0), GetVertexQuaternion(vtx2)));
    EndPrimitive();
}
)glsl";
    return glsl;
}

std::vector<u32> GenerateFixupGeometrySpirv(const FixupStageInputs& inputs, std::string* error) {
    if (error) error->clear();
    static const bool initialized = glslang::InitializeProcess();
    if (!initialized) {
        if (error) *error = "Could not initialize glslang";
        return {};
    }
    const std::string glsl = GenerateFixupGeometryGlsl(inputs);
    const char* source = glsl.c_str();
    constexpr auto stage = EShLangGeometry;
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    glslang::TShader shader(stage);
    shader.setStrings(&source, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        if (error) *error = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return {};
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        if (error) *error = std::string(program.getInfoLog()) + program.getInfoDebugLog();
        return {};
    }
    std::vector<u32> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
    if (spirv.empty() && error) *error = "glslang generated no SPIR-V for the geometry stage";
    return spirv;
}

} // namespace mh4n::gpu::fragment::lighting
