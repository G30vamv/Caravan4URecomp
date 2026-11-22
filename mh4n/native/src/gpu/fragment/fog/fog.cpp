// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: fog/fog_layout.h, pica/state.h, gpu_fog_test black-box cases and golden/gpu/glsl fog traces.

#include "emitter.h"
#include "feature_data.h"
#include "fog/fog_layout.h"
#include "gpu/pica/state.h"

#include <algorithm>

namespace mh4n::gpu::fragment {
namespace {

constexpr std::array<u32, fog::kRegWordsUsed> kGasRegisters{
    0x120, 0x121, 0x122, 0x123, 0x124, 0x125, 0x126,
    pica::gas_reg::kFogMode, 0xE4, 0xE5,
};

Cond GasDensity(const Emitter& e) {
    return e.Ne(e.Field(keyword::kGas, 0, 1), 0);
}

Cond FogModeIs(const Emitter& e, regenum::FogMode mode) {
    return e.Eq(e.Field(keyword::kTexRaw, 12, 3), static_cast<u32>(mode));
}

void EmitGasHelpers(Emitter& e) {
    e.Raw(R"glsl(
uint gas_density_bits(vec4 rgba) {
    uvec2 bytes = uvec2(round(rgba.ba * 255.0));
    return (bytes.x << 8) | bytes.y;
}

vec3 mh4n_gas_color(float density) {
    float coord = density * 8.0;
    uint index = uint(min(floor(coord), 7.0));
    uint base = gas_table[2u + index / 4u][index % 4u];
    uint delta = gas_table[index / 4u][index % 4u];
    vec3 color = vec3(base & 255u, (base >> 8) & 255u, (base >> 16) & 255u);
    vec3 change = vec3(bitfieldExtract(int(delta), 0, 8), bitfieldExtract(int(delta), 8, 8),
                       bitfieldExtract(int(delta), 16, 8));
    return clamp((color + (coord - float(index)) * change) / 255.0, 0.0, 1.0);
}

float gas_density_factor(uint density, uint maximum) {
    if ((maximum & 0x7fffu) > 0x7c00u || (density & 0x7fffu) == 0u) return 0.0;
    if (density >= maximum) return 1.0;
    return clamp(unpackHalf2x16(density).x / unpackHalf2x16(maximum).x, 0.0, 1.0);
}
)glsl");
}

void EmitGas(Emitter& e, const Cond& density) {
    e.Raw("uint gas_dst = gas_density_bits(texelFetch(tex_color, ivec2(gl_FragCoord.xy), 0));\n");
    e.If(density, [&] {
        e.Raw(R"glsl(
float gas_sum = unpackHalf2x16(gas_dst).x + unpackHalf2x16(gas_density_bits(combiner_output)).x;
uint gas_bits = packHalf2x16(vec2(gas_sum, 0.0));
combiner_output.ba = vec2((gas_bits >> 8) & 255u, gas_bits & 255u) / 255.0;
)glsl");
    }, [&] {
        const Value light_factor = e.Field(keyword::kGas, 1, 1);
        e.Raw("float gas_t = 1.0;\n");
        e.If(e.Eq(light_factor, 0), [&] {
            e.Raw("gas_t = gas_density_factor(gas_dst, gas_regs[2].y & 65535u);\n");
        });
        e.Raw("combiner_output = vec4(mh4n_gas_color(gas_t), 0.0);\n");
    });
}

void EmitDepthFog(Emitter& e) {
    e.Raw("{\nfloat fog_depth = clamp(depth, 0.0, 1.0);\nfloat fog_index;\n");
    e.If(e.Ne(e.Field(keyword::kTexRaw, 15, 1), 0), [&] {
        e.Raw("fog_index = (1.0 - fog_depth) * 128.0;\n");
    }, [&] {
        e.Raw("fog_index = fog_depth * 128.0;\n");
    });
    e.Raw(R"glsl(
float fog_i = clamp(floor(fog_index), 0.0, 127.0);
float fog_f = fog_index - fog_i;
vec2 entry = texture_buffer_lut_lf.data[int(fog_i) + fog_lut_offset];
float fog_factor = clamp(entry.x + entry.y * fog_f, 0.0, 1.0);
combiner_output.rgb = mix(fog_color, combiner_output.rgb, fog_factor);
}
combiner_output.rgb = byteround(combiner_output.rgb);
)glsl");
}

class FogFeature final : public FeatureModule {
public:
    const char* Name() const override { return "fog"; }

    void Key(const pica::PicaState& state, const FragmentKey& key, ReservedWords words) const override {
        const u32 operation = Bits(state.regs[freg::kFragmentOperation], 0, 2);
        if (operation == static_cast<u32>(regenum::FragmentOperationMode::Gas)) {
            words.gas = fog::kKeyDensityPass;
        } else if (key.texture.fog_mode() == regenum::FogMode::Gas) {
            words.gas = state.regs[kGasRegisters[fog::kRegLightZColor]] & 0x100
                            ? fog::kKeyLightFactorInput : 0;
        }
    }

    Needs NeedsOf(const FragmentKey& key, const DeviceProfile&) const override {
        const bool gas = (key.gas.raw & fog::kKeyDensityPass) != 0
                         || key.texture.fog_mode() == regenum::FogMode::Gas;
        return {.lut_lf = !gas && key.texture.fog_mode() == regenum::FogMode::Fog, .dst_copy = gas};
    }

    void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty) const override {
        if (dirty.all || state.dirty.Test(freg::kFogColor)) {
            const auto rgb = ColorRGBA8(state.regs[freg::kFogColor]);
            auto& color = block.uniforms.fog_color;
            if (dirty.all || color.x != rgb.x || color.y != rgb.y || color.z != rgb.z) {
                color = {rgb.x, rgb.y, rgb.z};
                dirty.Mark();
            }
        }
        for (size_t i = 0; i < kGasRegisters.size(); ++i) {
            const u32 reg = kGasRegisters[i];
            if ((dirty.all || state.dirty.Test(reg)) &&
                (dirty.all || block.gas.regs[i] != state.regs[reg])) {
                block.gas.regs[i] = state.regs[reg];
                dirty.Mark();
            }
        }
        if (dirty.all) {
            std::fill(std::begin(block.gas.regs) + fog::kRegWordsUsed, std::end(block.gas.regs), 0u);
        }
        if ((dirty.all || state.gas.table_dirty) &&
            (dirty.all || !std::ranges::equal(state.gas.lut, block.gas.table))) {
            std::ranges::copy(state.gas.lut, block.gas.table);
            dirty.Mark();
        }
    }

    void UploadTables(TableUpload& upload) const override {
        if (upload.need.fog) {
            detail::UploadLut<Vec2f>(upload, TableRing::Lf, upload.state.fog.lut,
                                     upload.block.uniforms.fog_lut_offset, [](u32 word) {
                return Vec2f{static_cast<float>(Bits(word, 13, 11)) / 2047.0f,
                             static_cast<float>(detail::SignExtend(word, 13)) / 2047.0f};
            });
        }
    }

    void Emit(Point point, EmitContext& context) const override {
        if (!context.emitter) return;
        auto& e = *context.emitter;
        const Cond density = GasDensity(e);
        const Cond gas = e.Or(density, FogModeIs(e, regenum::FogMode::Gas));
        if (point == Point::Declarations && (!gas.known || gas.value)) {
            EmitGasHelpers(e);
        } else if (point == Point::FogGas) {
            e.If(gas, [&] { EmitGas(e, density); }, [&] {
                e.If(FogModeIs(e, regenum::FogMode::Fog), [&] { EmitDepthFog(e); });
            });
        }
    }
};

MH4N_REGISTER_FRAGMENT_FEATURE(FogFeature);

} // namespace
} // namespace mh4n::gpu::fragment
