// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: generator.h and the Khronos glslang public API and examples.

#include "generator.h"

#include <chrono>
#include <limits>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

namespace mh4n::gpu::fragment {
namespace {

bool Compile(const std::string& source, std::vector<u32>& spirv, std::string& error) {
    if (source.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = "Fragment shader source exceeds glslang's input length limit";
        return false;
    }

    constexpr EShLanguage stage = EShLangFragment;
    constexpr int version = 450;
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    const char* data = source.data();
    const int length = static_cast<int>(source.size());
    glslang::TShader shader(stage);
    shader.setStringsWithLengths(&data, &length, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    if (!shader.parse(GetDefaultResources(), version, false, messages)) {
        error = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        error = std::string(program.getInfoLog()) + program.getInfoDebugLog();
        return false;
    }

    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &logger);
    if (spirv.empty()) {
        error = logger.getAllMessages();
        if (error.empty()) {
            error = "glslang generated no SPIR-V for the fragment stage";
        }
        return false;
    }
    return true;
}

} // namespace

bool CompileFragmentGlslToSpirv(const std::string& source, std::vector<u32>& spirv, std::string& error,
                                double* milliseconds) {
    spirv.clear();
    error.clear();
    static const bool initialized = glslang::InitializeProcess();
    if (!initialized) {
        error = "Could not initialize glslang";
        if (milliseconds) {
            *milliseconds = 0.0;
        }
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    const bool compiled = Compile(source, spirv, error);
    if (milliseconds) {
        *milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    return compiled;
}

} // namespace mh4n::gpu::fragment
