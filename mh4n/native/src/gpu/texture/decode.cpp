// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: accepted WP70, decode.h, formats.h, gpu_texture_test.cpp and golden/texture/vectors.bin.

#include "decode.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>

namespace mh4n::gpu::texture {
namespace {

using std::size_t;

constexpr unsigned kTileSide = 8;

template <Format F>
constexpr size_t kTileBytes = TileBytes(F);

constexpr u8 Expand4(unsigned value) {
    return static_cast<u8>(value * 17);
}

constexpr u8 Expand5(unsigned value) {
    return static_cast<u8>((value << 3) | (value >> 2));
}

constexpr u8 Expand6(unsigned value) {
    return static_cast<u8>((value << 2) | (value >> 4));
}

static_assert(sizeof(Rgba) == 4);

constexpr unsigned Read16(const u8* bytes) {
    return bytes[0] | (static_cast<unsigned>(bytes[1]) << 8);
}

constexpr u32 Read32(const u8* bytes) {
    return Read16(bytes) | (static_cast<u32>(Read16(bytes + 2)) << 16);
}

template <Format F>
Rgba DecodeTexel(const u8* tile, unsigned index) {
    if constexpr (F == Format::RGBA8) {
        const auto* p = tile + index * 4;
        return {p[3], p[2], p[1], p[0]};
    } else if constexpr (F == Format::RGB8) {
        const auto* p = tile + index * 3;
        return {p[2], p[1], p[0], 255};
    } else if constexpr (F == Format::RGB5A1) {
        const auto word = Read16(tile + index * 2);
        return {Expand5(word >> 11), Expand5((word >> 6) & 31), Expand5((word >> 1) & 31),
                static_cast<u8>((word & 1) * 255)};
    } else if constexpr (F == Format::RGB565) {
        const auto word = Read16(tile + index * 2);
        return {Expand5(word >> 11), Expand6((word >> 5) & 63), Expand5(word & 31), 255};
    } else if constexpr (F == Format::RGBA4) {
        const auto word = Read16(tile + index * 2);
        return {Expand4(word >> 12), Expand4((word >> 8) & 15), Expand4((word >> 4) & 15),
                Expand4(word & 15)};
    } else if constexpr (F == Format::IA8) {
        const auto* p = tile + index * 2;
        return {p[1], p[1], p[1], p[0]};
    } else if constexpr (F == Format::RG8) {
        const auto* p = tile + index * 2;
        return {p[1], p[0], 0, 255};
    } else if constexpr (F == Format::I8) {
        const auto intensity = tile[index];
        return {intensity, intensity, intensity, 255};
    } else if constexpr (F == Format::A8) {
        return {0, 0, 0, tile[index]};
    } else if constexpr (F == Format::IA4) {
        const auto intensity = Expand4(tile[index] >> 4);
        return {intensity, intensity, intensity, Expand4(tile[index] & 15)};
    } else {
        static_assert(F == Format::I4 || F == Format::A4);
        const auto value = Expand4((tile[index / 2] >> ((index & 1) * 4)) & 15);
        if constexpr (F == Format::I4) {
            return {value, value, value, 255};
        } else {
            return {0, 0, 0, value};
        }
    }
}

constexpr int kEtcModifiers[8][4] = {
    {2, 8, -2, -8},     {5, 17, -5, -17},     {9, 29, -9, -29},
    {13, 42, -13, -42}, {18, 60, -18, -60},   {24, 80, -24, -80},
    {33, 106, -33, -106}, {47, 183, -47, -183},
};

std::array<int, 2> EtcBaseColors(unsigned channel, bool differential) {
    if (!differential) {
        return {Expand4(channel >> 4), Expand4(channel & 15)};
    }
    const auto base = channel >> 3;
    const int delta = static_cast<int>(channel & 3) - static_cast<int>(channel & 4);
    // The reference converts the sum to a byte before replicating bits, even
    // for out-of-range differential codes that ETC1 leaves undefined.
    const auto second = static_cast<u8>(static_cast<int>(base) + delta);
    return {Expand5(base), Expand5(second)};
}

template <bool HasAlpha>
class EtcBlock {
public:
    explicit EtcBlock(const u8* block) : block_(block) {
        const auto* color = block + (HasAlpha ? 8 : 0);
        selectors_ = Read32(color);
        const unsigned flags = color[4];
        flip_ = (flags & 1) != 0;
        const bool differential = (flags & 2) != 0;
        const auto red = EtcBaseColors(color[7], differential);
        const auto green = EtcBaseColors(color[6], differential);
        const auto blue = EtcBaseColors(color[5], differential);
        const unsigned tables[2]{flags >> 5, (flags >> 2) & 7};
        for (unsigned part = 0; part < 2; ++part) {
            for (unsigned code = 0; code < 4; ++code) {
                const int modifier = kEtcModifiers[tables[part]][code];
                const auto channel = [modifier](int base) {
                    return static_cast<u8>(std::clamp(base + modifier, 0, 255));
                };
                palette_[part][code] = {channel(red[part]), channel(green[part]),
                                        channel(blue[part]), 255};
            }
        }
    }

    Rgba Texel(unsigned x, unsigned y) const {
        const unsigned part = (flip_ ? y : x) / 2;
        const unsigned bit = x * 4 + y;
        const unsigned code = ((selectors_ >> bit) & 1) | ((selectors_ >> (bit + 15)) & 2);
        auto rgba = palette_[part][code];
        if constexpr (HasAlpha) {
            rgba.a = Expand4((block_[bit / 2] >> ((bit & 1) * 4)) & 15);
        }
        return rgba;
    }

private:
    const u8* block_;
    u32 selectors_;
    bool flip_;
    Rgba palette_[2][4];
};

template <bool HasAlpha>
void DecodeEtcBlock(const u8* block, u8* dest, size_t stride) {
    const EtcBlock<HasAlpha> decoded(block);
    for (unsigned y = 0; y < 4; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            const auto rgba = decoded.Texel(x, y);
            std::memcpy(dest + y * stride + x * 4, &rgba, sizeof(rgba));
        }
    }
}

template <Format F>
void DecodeTiles(const u8* source, size_t width, size_t height, u8* out) {
    const size_t stride = width * 4;
    for (size_t ty = 0; ty < height; ty += kTileSide) {
        for (size_t tx = 0; tx < width; tx += kTileSide) {
            auto* tile = out + ty * stride + tx * 4;
            if constexpr (F == Format::ETC1 || F == Format::ETC1A4) {
                for (unsigned block = 0; block < 4; ++block) {
                    DecodeEtcBlock<F == Format::ETC1A4>(
                        source + block * (kTileBytes<F> / 4),
                        tile + (block / 2) * 4 * stride + (block % 2) * 16, stride);
                }
            } else {
                for (unsigned y = 0; y < kTileSide; ++y) {
                    for (unsigned x = 0; x < kTileSide; ++x) {
                        const auto rgba = DecodeTexel<F>(source, MortonInterleave(x, y));
                        std::memcpy(tile + y * stride + x * 4, &rgba, sizeof(rgba));
                    }
                }
            }
            source += kTileBytes<F>;
        }
    }
}

using TileDecoder = void (*)(const u8*, size_t, size_t, u8*);
constexpr std::array<TileDecoder, kFormatCount> kDecoders{
    DecodeTiles<Format::RGBA8>, DecodeTiles<Format::RGB8>, DecodeTiles<Format::RGB5A1>,
    DecodeTiles<Format::RGB565>, DecodeTiles<Format::RGBA4>, DecodeTiles<Format::IA8>,
    DecodeTiles<Format::RG8>, DecodeTiles<Format::I8>, DecodeTiles<Format::A8>,
    DecodeTiles<Format::IA4>, DecodeTiles<Format::I4>, DecodeTiles<Format::A4>,
    DecodeTiles<Format::ETC1>, DecodeTiles<Format::ETC1A4>,
};

using TexelDecoder = Rgba (*)(const u8*, unsigned);
constexpr std::array<TexelDecoder, static_cast<size_t>(Format::ETC1)> kTexelDecoders{
    DecodeTexel<Format::RGBA8>, DecodeTexel<Format::RGB8>, DecodeTexel<Format::RGB5A1>,
    DecodeTexel<Format::RGB565>, DecodeTexel<Format::RGBA4>, DecodeTexel<Format::IA8>,
    DecodeTexel<Format::RG8>, DecodeTexel<Format::I8>, DecodeTexel<Format::A8>,
    DecodeTexel<Format::IA4>, DecodeTexel<Format::I4>, DecodeTexel<Format::A4>,
};

size_t TileIndex(u32 x, u32 y, u32 width) {
    return static_cast<size_t>(y / kTileSide) * (width / kTileSide) + x / kTileSide;
}

bool HasImage(const u8* source, size_t size, u32 width, u32 height, Format format) {
    return source && IsValidSize(width, height) && IsValidFormat(static_cast<u32>(format)) &&
           size >= EncodedBytes(format, width, height);
}

} // namespace

Rgba LookupTexelInTile(const u8* tile, u32 x, u32 y, Format format, bool disable_alpha) {
    Rgba rgba{};
    const auto index = static_cast<size_t>(format);
    if (index < kTexelDecoders.size()) {
        rgba = kTexelDecoders[index](tile, MortonInterleave(x, y));
    } else {
        const u32 block = (y / 4) * 2 + x / 4;
        if (format == Format::ETC1) {
            rgba = EtcBlock<false>(tile + block * 8).Texel(x % 4, y % 4);
        } else if (format == Format::ETC1A4) {
            rgba = EtcBlock<true>(tile + block * 16).Texel(x % 4, y % 4);
        }
    }

    if (disable_alpha) {
        if (format == Format::IA8 || format == Format::IA4) {
            return {rgba.r, rgba.a, 0, 255};
        }
        if (format == Format::A8 || format == Format::A4) {
            return {rgba.a, rgba.a, rgba.a, 255};
        }
        rgba.a = 255;
    }
    return rgba;
}

Rgba LookupTexture(const u8* source, u32 x, u32 y, u32 width, Format format,
                   bool disable_alpha) {
    const auto* tile = source + TileIndex(x, y, width) * TileBytes(format);
    return LookupTexelInTile(tile, x % kTileSide, y % kTileSide, format, disable_alpha);
}

bool DecodeToRgba8(const u8* source, size_t size, u32 width, u32 height, Format format,
                   std::vector<u8>& out) {
    if (!HasImage(source, size, width, height, format)) {
        return false;
    }

    const std::less<const u8*> before;
    const bool aliases = !out.empty() && !before(source, out.data()) &&
                         before(source, out.data() + out.size());
    std::vector<u8> temporary;
    auto& target = aliases ? temporary : out;
    target.resize(static_cast<size_t>(width) * height * sizeof(Rgba));
    kDecoders[static_cast<size_t>(format)](source, width, height, target.data());
    if (aliases) {
        out.swap(temporary);
    }
    return true;
}

bool DecodeForHash(const u8* source, size_t size, u32 width, u32 height, Format format,
                   std::vector<u8>& out) {
    if (!HasImage(source, size, width, height, format)) {
        return false;
    }
    if (static_cast<u32>(format) >= static_cast<u32>(Format::IA8)) {
        DecodeToRgba8(source, size, width, height, format, out);
        const size_t stride = static_cast<size_t>(width) * sizeof(Rgba);
        for (u32 y = 0; y < height / 2; ++y) {
            auto first = out.data() + y * stride;
            std::swap_ranges(first, first + stride, out.data() + (height - y - 1) * stride);
        }
        return true;
    }

    const size_t pixel_bytes = HashBytesPerTexel(format);
    std::vector<u8> packed(static_cast<size_t>(width) * height * pixel_bytes);
    for (u32 y = 0; y < height; ++y) {
        auto* row = packed.data() + static_cast<size_t>(height - y - 1) * width * pixel_bytes;
        for (u32 x = 0; x < width; ++x) {
            const auto* tile = source + TileIndex(x, y, width) * TileBytes(format);
            std::memcpy(row + x * pixel_bytes, tile + MortonInterleave(x, y) * pixel_bytes,
                        pixel_bytes);
        }
    }
    out.swap(packed);
    return true;
}

} // namespace mh4n::gpu::texture
