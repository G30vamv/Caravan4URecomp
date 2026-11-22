// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: image.h, hdpack_test.cpp, LodePNG's public API and Microsoft's DDS Programming Guide.

#include "image.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <utility>

#include <lodepng.h>

#include "bytes.h"

namespace mh4n::hdpack {
namespace {

bool ValidSize(u32 width, u32 height) {
    return width != 0 && height != 0 && width <= kMaxDimension && height <= kMaxDimension;
}

struct DdsFormat {
    PixelFormat format;
    bool swap_rb = false;
    bool opaque = false;
};

constexpr u32 FourCC(char a, char b, char c, char d) {
    return static_cast<u32>(a) | (static_cast<u32>(b) << 8) |
           (static_cast<u32>(c) << 16) | (static_cast<u32>(d) << 24);
}

std::optional<DdsFormat> DxgiFormat(u32 value) {
    switch (value) {
    case 28: case 29: return DdsFormat{PixelFormat::RGBA8};
    case 87: case 91: return DdsFormat{PixelFormat::RGBA8, true};
    case 88: case 93: return DdsFormat{PixelFormat::RGBA8, true, true};
    case 71: case 72: return DdsFormat{PixelFormat::BC1};
    case 77: case 78: return DdsFormat{PixelFormat::BC3};
    case 83: return DdsFormat{PixelFormat::BC5};
    case 98: case 99: return DdsFormat{PixelFormat::BC7};
    default: return std::nullopt;
    }
}

std::optional<DdsFormat> LegacyFormat(const u8* data) {
    const u32 flags = Get32(data + 80);
    if (flags & 0x4) {
        switch (Get32(data + 84)) {
        case FourCC('D', 'X', 'T', '1'): return DdsFormat{PixelFormat::BC1};
        case FourCC('D', 'X', 'T', '5'): return DdsFormat{PixelFormat::BC3};
        case FourCC('A', 'T', 'I', '2'):
        case FourCC('B', 'C', '5', 'U'): return DdsFormat{PixelFormat::BC5};
        default: return std::nullopt;
        }
    }

    const u32 red = Get32(data + 92);
    const u32 green = Get32(data + 96);
    const u32 blue = Get32(data + 100);
    const u32 alpha = Get32(data + 104);
    if (!(flags & 0x40) || Get32(data + 88) != 32 || green != 0xFF00 ||
        (alpha != 0 && alpha != 0xFF000000)) {
        return std::nullopt;
    }
    if (red == 0xFF && blue == 0xFF0000) {
        return DdsFormat{PixelFormat::RGBA8, false, alpha == 0};
    }
    if (red == 0xFF0000 && blue == 0xFF) {
        return DdsFormat{PixelFormat::RGBA8, true, alpha == 0};
    }
    return std::nullopt;
}

}  // namespace

const char* PixelFormatName(PixelFormat format) {
    switch (format) {
    case PixelFormat::RGBA8: return "RGBA8";
    case PixelFormat::BC1: return "BC1";
    case PixelFormat::BC3: return "BC3";
    case PixelFormat::BC5: return "BC5";
    case PixelFormat::BC7: return "BC7";
    }
    return "unknown";
}

u32 BlockBytes(PixelFormat format) {
    switch (format) {
    case PixelFormat::BC1: return 8;
    case PixelFormat::BC3: case PixelFormat::BC5: case PixelFormat::BC7: return 16;
    default: return 0;
    }
}

u64 LevelBytes(PixelFormat format, u32 width, u32 height) {
    const auto block_bytes = BlockBytes(format);
    if (block_bytes == 0) {
        return u64{4} * width * height;
    }
    return ((u64{width} + 3) / 4) * ((u64{height} + 3) / 4) * block_bytes;
}

u32 FullMipCount(u32 width, u32 height) {
    return static_cast<u32>(std::bit_width(std::max(width, height)));
}

void FlipRows(u8* rgba, u32 width, u32 height) {
    if (!width || height < 2) {
        return;
    }
    const auto stride = std::size_t{4} * width;
    for (u32 row = 0; row < height / 2; ++row) {
        u8* top = rgba + row * stride;
        u8* bottom = rgba + (height - 1 - row) * stride;
        std::swap_ranges(top, top + stride, bottom);
    }
}

bool DecodePng(const u8* data, std::size_t size, bool flip, Image& out, std::string& error) {
    if (!data || size < 33) {
        error = "truncated PNG header";
        return false;
    }

    lodepng::State state;
    unsigned width = 0, height = 0;
    const auto header_result = lodepng_inspect(&width, &height, &state, data, size);
    if (header_result != 0) {
        error = lodepng_error_text(header_result);
        return false;
    }
    if (!ValidSize(width, height)) {
        error = "PNG dimensions out of range";
        return false;
    }

    Image decoded{PixelFormat::RGBA8, width, height, {{}}};
    auto& pixels = decoded.levels.front();
    const auto result = lodepng::decode(pixels, width, height, data, size);
    if (result != 0) {
        error = lodepng_error_text(result);
        return false;
    }
    if (flip) {
        FlipRows(pixels.data(), width, height);
    }
    out = std::move(decoded);
    return true;
}

bool DecodeDds(const u8* data, std::size_t size, Image& out, std::string& error) {
    constexpr std::size_t header_size = 128;
    constexpr std::size_t dx10_size = 20;
    if (!data || size < header_size || std::memcmp(data, "DDS ", 4) != 0 ||
        Get32(data + 4) != 124 || Get32(data + 76) != 32) {
        error = "invalid DDS header";
        return false;
    }

    const u32 width = Get32(data + 16);
    const u32 height = Get32(data + 12);
    if (!ValidSize(width, height)) {
        error = "DDS dimensions out of range";
        return false;
    }
    if ((Get32(data + 112) & 0x20FE00) || Get32(data + 24) > 1) {
        error = "DDS cube maps and volumes are unsupported";
        return false;
    }

    std::size_t offset = header_size;
    std::optional<DdsFormat> format;
    if ((Get32(data + 80) & 0x4) && Get32(data + 84) == FourCC('D', 'X', '1', '0')) {
        offset += dx10_size;
        if (size < offset) {
            error = "truncated DDS DX10 header";
            return false;
        }
        if (Get32(data + 132) != 3 || (Get32(data + 136) & 0x4) || Get32(data + 140) != 1) {
            error = "DDS resource must be a single 2D texture";
            return false;
        }
        format = DxgiFormat(Get32(data + 128));
    } else {
        format = LegacyFormat(data);
    }
    if (!format) {
        error = "unsupported DDS pixel format";
        return false;
    }

    Image decoded{format->format, width, height, {}};
    const u32 mips = std::max(1u, Get32(data + 28));
    if (mips > FullMipCount(width, height)) {
        error = "invalid DDS mip count";
        return false;
    }
    if (format->format == PixelFormat::RGBA8) {
        const auto row_bytes = std::size_t{4} * width;
        std::size_t stride = row_bytes;
        if (Get32(data + 8) & 0x8) {
            stride = std::max(row_bytes, static_cast<std::size_t>(Get32(data + 20)));
        }
        if (stride > (size - offset) / height) {
            error = "truncated DDS pixels";
            return false;
        }
        auto& pixels = decoded.levels.emplace_back(row_bytes * height);
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(pixels.data() + y * row_bytes, data + offset + y * stride, row_bytes);
        }
        if (format->swap_rb || format->opaque) {
            for (std::size_t pos = 0; pos < pixels.size(); pos += 4) {
                if (format->swap_rb) {
                    std::swap(pixels[pos], pixels[pos + 2]);
                }
                if (format->opaque) {
                    pixels[pos + 3] = 255;
                }
            }
        }
    } else {
        for (u32 level = 0; level < mips; ++level) {
            const auto bytes = LevelBytes(format->format, std::max(1u, width >> level),
                                          std::max(1u, height >> level));
            if (bytes > size - offset) {
                error = "truncated DDS mip chain";
                return false;
            }
            decoded.levels.emplace_back(data + offset, data + offset + bytes);
            offset += bytes;
        }
    }
    out = std::move(decoded);
    return true;
}

}  // namespace mh4n::hdpack
