// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: vertex_shader.h, gpu_shader_test.cpp and the Khronos glslang public API.

#include "vertex_shader.h"

#include <chrono>
#include <limits>

#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "SPIRV/GlslangToSpv.h"

namespace mh4n::gpu::shader {
namespace {

struct CompilerProcess {
    const bool ready = glslang::InitializeProcess();

    ~CompilerProcess() {
        if (ready) {
            glslang::FinalizeProcess();
        }
    }
};

} // namespace

bool CompileGlslToSpirv(const std::string& source, std::vector<u32>& spirv, std::string& error,
                       double* milliseconds, bool compute) {
    const auto start = std::chrono::steady_clock::now();
    if (milliseconds) {
        *milliseconds = 0.0;
    }
    spirv.clear();
    error.clear();
    if (source.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = "GLSL source exceeds the compiler's input limit";
        return false;
    }
    static const CompilerProcess process;
    if (!process.ready) {
        error = "glslang initialization failed";
        return false;
    }

    const EShLanguage stage = compute ? EShLangCompute : EShLangVertex;
    constexpr auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    glslang::TShader shader(stage);
    const char* data = source.data();
    const auto length = static_cast<int>(source.size());
    shader.setStringsWithLengths(&data, &length, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        error = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        error = std::string(program.getInfoLog()) + program.getInfoDebugLog();
        return false;
    }

    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
    if (spirv.empty()) {
        error = "glslang produced an empty SPIR-V module";
        return false;
    }
    if (milliseconds) {
        *milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    return true;
}

} // namespace mh4n::gpu::shader
