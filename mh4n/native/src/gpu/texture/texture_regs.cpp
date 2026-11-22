// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: texture_regs.h, gpu_texture_test.cpp and spec/pica_usage.md.

#include "texture_regs.h"

namespace mh4n::gpu::texture {

std::vector<TextureRef> TexturesOfDraw(const u32* regs, u32* skipped) {
    std::vector<TextureRef> out;
    TexturesOfDraw(regs, out, skipped);
    return out;
}

void TexturesOfDraw(const u32* regs, std::vector<TextureRef>& out, u32* skipped) {
    constexpr u32 bases[] = {kTexture0, kTexture1, kTexture2};
    constexpr u32 formats[] = {kTexture0Format, kTexture1Format, kTexture2Format};
    constexpr u32 face_mask = (1u << 22) - 1;
    out.clear();
    u32 omitted = 0;
    for (u32 unit = 0; unit < 3; ++unit) {
        if ((regs[kMainConfig] & (1u << unit)) == 0) {
            continue;
        }
        const u32 base = bases[unit];
        const u32 size = regs[base + 1];
        const u32 width = (size >> 16) & 0x7ff;
        const u32 height = size & 0x7ff;
        const u32 format = regs[formats[unit]] & 0xf;
        const auto type = unit == 0 ? static_cast<Type>((regs[base + 2] >> 28) & 7)
                                    : Type::Texture2D;
        if (!IsValidSize(width, height) || !IsValidFormat(format) ||
            type == Type::Shadow2D || type == Type::ShadowCube || type == Type::Disabled) {
            ++omitted;
            continue;
        }

        const u32 address = regs[base + 4] & 0x0fffffff;
        const bool cube = type == Type::TextureCube;
        out.push_back({unit, 0, cube, address * 8, width, height, static_cast<Format>(format)});
        if (cube) {
            for (u32 face = 1; face < 6; ++face) {
                const u32 face_address = (address & ~face_mask) |
                                         (regs[kCubeAddress + face - 1] & face_mask);
                out.push_back({unit, face, true, face_address * 8, width, height,
                               static_cast<Format>(format)});
            }
        }
    }
    if (skipped) {
        *skipped = omitted;
    }
}

} // namespace mh4n::gpu::texture
