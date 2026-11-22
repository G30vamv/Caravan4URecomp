// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: cpuvertex and PICA contracts, gpu_cpuvertex tests and 3dbrew GPU/Primitive Engine.

#include "geometry_pipeline.h"

#include <algorithm>
#include <utility>

namespace mh4n::gpu::cpuvertex {

class GeometryPipelineBackend {
public:
    GeometryPipelineBackend(pica::GeometryMode mode, u32 width)
        : mode(mode), width(width) {}

    bool IsEmpty() const {
        return mode == pica::GeometryMode::VariablePrimitive ? need_index : cursor == start;
    }

    pica::GeometryMode mode;
    u32 width;
    u32 cursor = 0;
    u32 start = 0;
    u32 end = 0;
    u32 main_remaining = 0;
    u32 remaining = 0;
    bool need_index = true;
    AttributeBuffer attributes{};
};

namespace {

using pica::GeometryMode;
namespace reg = pica::reg;

ShaderInputConfig InputConfig(const pica::PicaState& state) {
    return {
        state.regs[reg::kGs + reg::kShaderInputBufferConfig] & 15,
        state.regs[reg::kGs + 0x0B] | (static_cast<u64>(state.regs[reg::kGs + 0x0C]) << 32),
    };
}

bool IsValidMode(GeometryMode mode, u32 format, u32 width, u32 input_count,
                 bool variable, bool to_uniform) {
    switch (mode) {
    case GeometryMode::Point:
        return (format & 255) == pica::kGsModePoint && !variable && !to_uniform && input_count % width == 0;
    case GeometryMode::FixedPrimitive:
        return (format & 255) == pica::kGsModeFixedPrimitive && !variable && to_uniform &&
               ((format >> 12) & 15) + 1 == width;
    case GeometryMode::VariablePrimitive:
        return (format & 255) == pica::kGsModeVariablePrimitive && variable && to_uniform;
    default:
        return false;
    }
}

} // namespace

GeometryPipeline::GeometryPipeline(GeometryShaderUnit& gs_unit, Uniforms& gs_uniforms)
    : gs_unit_(gs_unit), gs_(gs_uniforms) {}

GeometryPipeline::~GeometryPipeline() = default;

bool GeometryPipeline::IsEmpty() const {
    return !backend_ || backend_->IsEmpty();
}

bool GeometryPipeline::NeedIndexInput() const {
    return backend_ && backend_->mode == GeometryMode::VariablePrimitive && backend_->need_index;
}

bool GeometryPipeline::Reconfigure(bool adopt_cursors) {
    discarded_partial_input_ = !IsEmpty();
    backend_.reset();
    error_ = {Unhandled::GeometryConfig};

    const auto& state = *state_;
    const auto& saved = state.geometry;
    const u32 stage = state.regs[reg::kGeometryStage];
    const u32 use_gs = stage & 3;
    if (use_gs == pica::kUseGsNo && saved.mode == GeometryMode::None) {
        error_ = {};
        return true;
    }
    const u32 input = state.regs[reg::kGs + reg::kShaderInputBufferConfig];
    const u32 format = state.regs[reg::kGsConfig];
    const u32 width = (state.regs[reg::kVsOutmapTotalA] & 15) + 1;
    const u32 inputs = (input & 15) + 1;
    if (use_gs != pica::kUseGsYes || (input >> 24) != pica::kShaderModeGs ||
        width != (state.regs[reg::kVsOutmapTotalB] & 15) + 1 ||
        saved.vs_output_num != width ||
        !IsValidMode(saved.mode, format, width, inputs, (stage >> 31) != 0, ((input >> 8) & 255) != 0)) {
        return false;
    }

    auto next = std::make_unique<GeometryPipelineBackend>(saved.mode, width);
    switch (saved.mode) {
    case GeometryMode::Point:
        next->end = inputs;
        if (adopt_cursors) {
            next->cursor = saved.buffer_index;
            for (size_t i = 0; i < next->attributes.size(); ++i) {
                for (u32 lane = 0; lane < 4; ++lane) {
                    next->attributes[i].c[lane] = f24::FromFloat32(saved.attribute_buffer[i].c[lane]);
                }
            }
        }
        break;
    case GeometryMode::FixedPrimitive:
        next->start = (format >> 16) & 255;
        next->end = next->start + width * (((format >> 8) & 15) + 1);
        next->cursor = adopt_cursors ? saved.buffer_index : next->start;
        if (next->end > gs_.f.size()) {
            error_ = {Unhandled::GeometryOverflow};
            return false;
        }
        break;
    case GeometryMode::VariablePrimitive:
        next->end = static_cast<u32>(gs_.f.size());
        if (adopt_cursors) {
            next->cursor = saved.buffer_index;
            next->need_index = saved.need_index;
            next->main_remaining = saved.main_vertex_num;
            next->remaining = saved.total_vertex_num;
            if (!next->need_index &&
                (next->cursor == 0 || next->main_remaining > next->remaining || !next->remaining ||
                 u64{next->cursor} + next->remaining + u64{next->main_remaining} * (width - 1) > next->end)) {
                error_ = {Unhandled::GeometryOverflow};
                return false;
            }
        }
        break;
    default:
        return false;
    }
    if (next->cursor < next->start || next->cursor >= next->end) {
        error_ = {Unhandled::GeometryOverflow};
        return false;
    }

    backend_ = std::move(next);
    error_ = {};
    return true;
}

bool GeometryPipeline::SubmitIndex(u32 value) {
    if (!NeedIndexInput()) {
        error_ = {Unhandled::GeometryConfig};
        return false;
    }

    auto& buffer = *backend_;
    const u64 main = u64{state_->regs[reg::kVariableVertexMainNum]} + 1;
    if (value < main || 1 + u64{value} + main * (buffer.width - 1) > gs_.f.size()) {
        error_ = {Unhandled::GeometryOverflow};
        return false;
    }
    gs_.f[0].c[0] = f24::FromFloat32(static_cast<float>(value));
    buffer.cursor = 1;
    buffer.main_remaining = static_cast<u32>(main);
    buffer.remaining = value;
    buffer.need_index = false;
    return true;
}

bool GeometryPipeline::SubmitVertex(const AttributeBuffer& input) {
    if (!backend_) {
        if (vertex_sink_) {
            vertex_sink_->SubmitVertex(input);
        }
        return true;
    }

    auto& buffer = *backend_;
    bool ready = false;
    switch (buffer.mode) {
    case GeometryMode::Point:
        if (buffer.width > buffer.end - buffer.cursor) {
            error_ = {Unhandled::GeometryOverflow};
            return false;
        }
        std::copy_n(input.begin(), buffer.width, buffer.attributes.begin() + buffer.cursor);
        buffer.cursor += buffer.width;
        ready = buffer.cursor == buffer.end;
        if (ready) {
            gs_unit_.LoadInput(InputConfig(*state_), buffer.attributes);
            buffer.cursor = 0;
        }
        break;
    case GeometryMode::FixedPrimitive:
        if (buffer.width > buffer.end - buffer.cursor) {
            error_ = {Unhandled::GeometryOverflow};
            return false;
        }
        std::copy_n(input.begin(), buffer.width, gs_.f.begin() + buffer.cursor);
        buffer.cursor += buffer.width;
        ready = buffer.cursor == buffer.end;
        if (ready) {
            buffer.cursor = buffer.start;
        }
        break;
    case GeometryMode::VariablePrimitive: {
        if (buffer.need_index) {
            error_ = {Unhandled::GeometryNeedsIndex};
            return false;
        }
        const u32 count = buffer.main_remaining ? buffer.width : 1;
        std::copy_n(input.begin(), count, gs_.f.begin() + buffer.cursor);
        buffer.cursor += count;
        if (buffer.main_remaining) {
            --buffer.main_remaining;
        }
        ready = --buffer.remaining == 0;
        if (ready) {
            buffer.need_index = true;
            buffer.cursor = 0;
        }
        break;
    }
    default:
        error_ = {Unhandled::GeometryConfig};
        return false;
    }

    if (!ready) {
        return true;
    }
    gs_unit_.ConfigOutput(state_->regs[reg::kGs + reg::kShaderOutputMask]);
    const ShaderSetup shader{
        state_->gs.program_code.data(), state_->gs.swizzle_data.data(), &gs_,
        state_->regs[reg::kGs + reg::kShaderMainOffset] & 0xffff, native_, check_native_,
    };
    ++invocations_;
    const bool ok = engine_->Run(shader, gs_unit_, error_);
    gs_.b[15] = true;
    return ok;
}

void GeometryPipeline::Reset() {
    backend_.reset();
    error_ = {};
    discarded_partial_input_ = false;
}

} // namespace mh4n::gpu::cpuvertex
