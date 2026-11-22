// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Camera Services, libctru y2r.h, ITU-R BT.601/709 and boot-village IPC.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <span>

#include "mh4n/types.h"

namespace mh4n::y2r {

using Coefficients = std::array<u16, 8>;

constexpr s32 Nearest(double value) {
    return static_cast<s32>(value + (value < 0 ? -0.5 : 0.5));
}

// Invert the ITU-R luma/chroma equations and quantize the matrix and the offsets
// independently. Studio range uses 219 luma and 224 chroma steps per 255 RGB steps.
constexpr Coefficients ItuMatrix(double kr, double kb, bool limited) {
    const double kg = 1 - kr - kb;
    const double luma = limited ? 255.0 / 219.0 : 1.0;
    const double chroma = limited ? 255.0 / 224.0 : 1.0;
    const double rv = 2 * (1 - kr) * chroma;
    const double bu = 2 * (1 - kb) * chroma;
    const double gv = rv * kr / kg;
    const double gu = bu * kb / kg;
    const double bias = limited ? -16 * luma : 0;
    return {
        static_cast<u16>(Nearest(256 * luma)), static_cast<u16>(Nearest(256 * rv)),
        static_cast<u16>(Nearest(256 * gv)), static_cast<u16>(Nearest(256 * gu)),
        static_cast<u16>(Nearest(256 * bu)), static_cast<u16>(Nearest(32 * (bias - 128 * rv))),
        static_cast<u16>(Nearest(32 * (bias + 128 * (gu + gv)))),
        static_cast<u16>(Nearest(32 * (bias - 128 * bu))),
    };
}

// Preset 0 is the exact register image returned at seq=13257.
inline constexpr std::array<Coefficients, 4> kPresets{{
    {0x0100, 0x0166, 0x00b6, 0x0058, 0x01c5, 0xe991, 0x10ee, 0xe3a5},
    ItuMatrix(0.2126, 0.0722, false),
    ItuMatrix(0.299, 0.114, true),
    ItuMatrix(0.2126, 0.0722, true),
}};

struct Settings {
    u16 width = 0;
    u16 lines = 0;
    u8 input = 0;
    u8 output = 0;
    u8 rotation = 0;
    u8 block_alignment = 0;
    u16 alpha = 0xff;
    Coefficients coefficients = kPresets[0];
};

inline constexpr std::array<u32, 4> kBytesPerPixel{4, 3, 2, 2};

// libctru's Y2RU_ColorCoefficients: unsigned 2.8 multipliers, signed 11.5
// offsets and a 0.75 bias before truncation and saturation.
inline std::array<u32, 3> Rgb(u8 y, u8 u, u8 v, const Coefficients& c) {
    const s32 luma = (c[0] & 0x3ff) * y;
    const auto component = [](s32 value, u16 offset) {
        const s32 fixed = value + 8 * std::bit_cast<s16>(offset) + 192;
        return static_cast<u32>(std::clamp(fixed / 256, 0, 255));
    };
    return {
        component(luma + (c[1] & 0x3ff) * v, c[5]),
        component(luma - (c[2] & 0x3ff) * v - (c[3] & 0x3ff) * u, c[6]),
        component(luma + (c[4] & 0x3ff) * u, c[7]),
    };
}

// 3dbrew output formats share the PICA200 packing, with the least significant byte first.
inline void StorePixel(u8* dst, u8 format, u16 alpha, const std::array<u32, 3>& rgb) {
    const auto [r, g, b] = rgb;
    u32 word = 0;
    switch (format) {
    case 0: word = (r << 24) | (g << 16) | (b << 8) | (alpha & 0xff); break;
    case 1: word = (r << 16) | (g << 8) | b; break;
    case 2: word = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) |
                   ((alpha >> 7) & 1); break;
    case 3: word = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); break;
    }
    for (u32 byte = 0; byte < kBytesPerPixel[format]; ++byte) {
        dst[byte] = static_cast<u8>(word >> (8 * byte));
    }
}

// libctru SetRotation describes an independent clockwise rotation of each
// band of up to eight source lines. 3dbrew specifies Morton order within each tile.
inline u32 OutputIndex(const Settings& settings, u32 x, u32 y) {
    const u32 width = settings.width;
    const u32 band = y & ~7u;
    const u32 height = std::min(8u, settings.lines - band);
    u32 output_width = width;
    u32 dx = x;
    u32 dy = y - band;
    switch (settings.rotation) {
    case 1: dx = height - 1 - dy; dy = x; output_width = height; break;
    case 2: dx = width - 1 - x; dy = height - 1 - dy; break;
    case 3: dx = dy; dy = width - 1 - x; output_width = height; break;
    }

    const u32 base = band * width;
    if (settings.block_alignment == 0) {
        return base + dy * output_width + dx;
    }
    const u32 tile = (dy / 8) * (output_width / 8) + dx / 8;
    const u32 morton = (dx & 1) | ((dy & 1) << 1) | ((dx & 2) << 1) |
                       ((dy & 2) << 2) | ((dx & 4) << 2) | ((dy & 4) << 3);
    return base + tile * 64 + morton;
}

using PlaneSizes = std::array<u32, 3>;

inline PlaneSizes InputSizes(const Settings& settings) {
    const u32 pixels = u32{settings.width} * settings.lines;
    if (settings.input == 4) {
        return {pixels * 2, 0, 0};
    }
    const u32 sample_bytes = settings.input >= 2 ? 2 : 1;
    const u32 chroma_lines = (settings.input & 1) ? (settings.lines + 1u) / 2 : settings.lines;
    const u32 chroma_size = (settings.width / 2) * chroma_lines * sample_bytes;
    return {pixels * sample_bytes, chroma_size, chroma_size};
}

// 3dbrew/libctru input formats: planar 4:2:0 shares chroma over 2x2 and 4:2:2
// over 2x1. Halfword inputs carry the sample in their low byte. Packed input is YUYV.
inline void Convert(const Settings& settings, std::span<const u8> y_plane,
                    std::span<const u8> u_plane, std::span<const u8> v_plane,
                    std::span<u8> output) {
    const u32 width = settings.width;
    const u32 sample_bytes = settings.input >= 2 ? 2 : 1;
    const u32 chroma_height = (settings.input & 1) ? 2 : 1;
    const u32 pixel_bytes = kBytesPerPixel[settings.output];

    for (u32 row = 0; row < settings.lines; ++row) {
        for (u32 x = 0; x < width; ++x) {
            const u32 pixel = row * width + x;
            u8 y;
            u8 u;
            u8 v;
            if (settings.input == 4) {
                const u32 pair = (pixel & ~1u) * 2;
                y = y_plane[pixel * 2];
                u = y_plane[pair + 1];
                v = y_plane[pair + 3];
            } else {
                const u32 chroma = ((row / chroma_height) * (width / 2) + x / 2) * sample_bytes;
                y = y_plane[pixel * sample_bytes];
                u = u_plane[chroma];
                v = v_plane[chroma];
            }
            StorePixel(output.data() + OutputIndex(settings, x, row) * pixel_bytes,
                       settings.output, settings.alpha, Rgb(y, u, v, settings.coefficients));
        }
    }
}

} // namespace mh4n::y2r
