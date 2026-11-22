// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the PICA state contract, gpu_pica_test, and 3dbrew GPU/Internal Registers.

#include "state.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>

namespace mh4n::gpu::pica {
namespace {

template <size_t N>
bool UpdateWords(std::array<u32, N>& storage, u32 offset, std::span<const u32> values,
                 u32& high_water_mark) {
    if (offset >= N || values.size() > N - offset) {
        return false;
    }
    bool changed = false;
    for (const u32 value : values) {
        if (storage[offset] != value) {
            storage[offset] = value;
            high_water_mark = std::max(high_water_mark, offset + 1);
            changed = true;
        }
        ++offset;
    }
    return changed;
}

} // namespace

float Float24ToFloat(u32 raw24) {
    const u32 sign = (raw24 & 0x800000) << 8;
    const u32 exponent = BitsOf(raw24, 16, 7);
    const u32 fraction = raw24 & 0xFFFF;
    if (exponent == 0) {
        const float magnitude = std::ldexp(static_cast<float>(fraction), -78);
        return sign == 0 ? magnitude : -magnitude;
    }
    const u32 float_exponent = exponent == 127 ? 255 : exponent + 64;
    return std::bit_cast<float>(sign | (float_exponent << 23) | (fraction << 7));
}

bool PackedAttribute::Push(u32 word, bool is_float32) {
    const u32 length = is_float32 ? 4 : 3;
    if (index >= length) {
        Reset();
    }
    buffer[index++] = word;
    return index == length;
}

Vec4f PackedAttribute::Get(bool is_float32) {
    Reset();
    if (is_float32) {
        return {{std::bit_cast<float>(buffer[3]), std::bit_cast<float>(buffer[2]),
                 std::bit_cast<float>(buffer[1]), std::bit_cast<float>(buffer[0])}};
    }

    const u64 low = (u64{buffer[1]} << 32) | buffer[2];
    const u64 high = (u64{buffer[0]} << 32) | buffer[1];
    return {{Float24ToFloat(static_cast<u32>(low)),
             Float24ToFloat(static_cast<u32>(low >> 24)),
             Float24ToFloat(static_cast<u32>(high >> 16)),
             Float24ToFloat(static_cast<u32>(high >> 40))}};
}

u64 ShaderSetup::NextSerial() {
    static std::atomic<u64> serial{1};
    return serial.fetch_add(1, std::memory_order_relaxed);
}

void ShaderSetup::WriteUniformBoolReg(u32 value) {
    std::array<bool, 16> decoded{};
    for (u32 bit = 0; bit < decoded.size(); ++bit) {
        decoded[bit] = BitsOf(value, bit, 1) != 0;
    }
    if (uniform_b != decoded) {
        uniform_b = decoded;
        uniforms_dirty = true;
        uniforms_serial = NextSerial();
    }
}

void ShaderSetup::WriteUniformIntReg(u32 index, const std::array<u8, 4>& values) {
    if (index < uniform_i.size() && uniform_i[index] != values) {
        uniform_i[index] = values;
        uniforms_dirty = true;
        uniforms_serial = NextSerial();
    }
}

std::optional<u32> ShaderSetup::WriteUniformFloatReg(u32& index_reg, u32 value) {
    const bool float32 = BitsOf(index_reg, 31, 1) != 0;
    if (!uniform_queue.Push(value, float32)) {
        return std::nullopt;
    }
    const Vec4f decoded = uniform_queue.Get(float32);
    const u32 index = BitsOf(index_reg, 0, 7);
    if (index >= uniform_f.size()) {
        return std::nullopt;
    }
    if (uniform_f[index] != decoded) {
        uniform_f[index] = decoded;
        uniforms_dirty = true;
        uniforms_serial = NextSerial();
    }
    index_reg = WithBits(index_reg, 0, 7, index + 1);
    return index;
}

std::optional<ShaderSetup::UniformWriteRange> ShaderSetup::WriteUniformFloatRegRange(
    u32& index_reg, std::span<const u32> values) {
    const u32 first = BitsOf(index_reg, 0, 7);
    u32 count = 0;
    for (const u32 word : values) {
        if (WriteUniformFloatReg(index_reg, word)) {
            ++count;
        } else if (uniform_queue.index == 0) {
            break;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    return UniformWriteRange{first, count};
}

void ShaderSetup::UpdateProgramCode(u32 offset, u32 value) {
    UpdateProgramCodeRange(offset, {&value, 1});
}

void ShaderSetup::UpdateProgramCodeRange(u32 offset, std::span<const u32> values) {
    if (UpdateWords(program_code, offset, values, biggest_program_size)) {
        program_dirty = true;
        program_serial = NextSerial();
    }
}

void ShaderSetup::UpdateSwizzleData(u32 offset, u32 value) {
    UpdateSwizzleDataRange(offset, {&value, 1});
}

void ShaderSetup::UpdateSwizzleDataRange(u32 offset, std::span<const u32> values) {
    if (UpdateWords(swizzle_data, offset, values, biggest_swizzle_size)) {
        swizzle_dirty = true;
        swizzle_serial = NextSerial();
    }
}

PicaState::PicaState() {
    regs[reg::kIrqCompare] = 0x12345678;
    regs[reg::kIrqMask] = 0xFFFFFFF0;
    regs[reg::kIrqAutostop] = 1;
    regs[reg::kGs + reg::kShaderInputBufferConfig] = (kShaderModeVs << 24) | 1;
    dirty.SetAll();
}

void PicaState::ClearDirty() {
    dirty.Reset();
    vs.ClearDirty();
    gs.ClearDirty();
    lighting.lut_dirty = 0;
    fog.lut_dirty = false;
    gas.table_dirty = false;
    proctex.table_dirty = 0;
}

void DelayGenerator::AddVertices(size_t vertices, u32 topology) {
    if (topology == kTopologyStrip || topology == kTopologyFan) {
        triangle_count_ += vertices > 2 ? vertices - 2 : 1;
    } else {
        triangle_count_ += vertices / 3;
    }
}

u64 DelayGenerator::CalculateAndResetDelay() {
    const float triangles = static_cast<float>(triangle_count_);
    float ns = static_cast<float>(command_count_) * 22.4f;
    ns += (1.f - 0.35f) * triangles * 400.f;
    ns += 0.35f * triangles * (400.f * 0.20f);
    Reset();
    return static_cast<u64>(ns);
}

} // namespace mh4n::gpu::pica
