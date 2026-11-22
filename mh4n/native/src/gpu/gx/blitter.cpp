// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the GX contracts, gpu_gx_test, and 3dbrew GPU/External Registers.

#include "blitter.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "mh4n/log.h"

namespace mh4n::gpu::gx {
namespace {

constexpr char kLog[] = "GPU.GX";

bool OutsideMainRam(u32 address) {
    return address < 0x20000000 || address >= 0x28000000;
}

using Color = std::array<u32, 4>; // R, G, B, A

// Entries give the component widths from R to A, in descending bit order.
constexpr std::array<std::array<u32, 4>, 5> kChannels{{
    {{8, 8, 8, 8}}, {{8, 8, 8, 0}}, {{5, 6, 5, 0}}, {{5, 5, 5, 1}}, {{4, 4, 4, 4}},
}};

u32 ExpandComponent(u32 value, u32 bits) {
    if (bits == 1) {
        return value * 255;
    }
    return (value << (8 - bits)) | (value >> (2 * bits - 8));
}

Color ReadPixel(std::span<const u8> input, u64 offset, u32 format, u32 size) {
    u32 word = 0;
    if (offset < input.size()) {
        const size_t count = std::min<size_t>(size, input.size() - static_cast<size_t>(offset));
        for (size_t i = 0; i < count; ++i) {
            word |= u32{input[static_cast<size_t>(offset) + i]} << (8 * i);
        }
    }
    Color color{};
    u32 shift = size * 8;
    for (size_t i = 0; i < color.size(); ++i) {
        const u32 bits = kChannels[format][i];
        shift -= bits;
        color[i] = bits == 0 ? 255 : ExpandComponent((word >> shift) & ((1u << bits) - 1), bits);
    }
    return color;
}

void WritePixel(std::span<u8> output, size_t offset, const Color& color, u32 format, u32 size) {
    u32 word = 0;
    for (size_t i = 0; i < color.size(); ++i) {
        const u32 bits = kChannels[format][i];
        word = (word << bits) | (color[i] >> (8 - bits));
    }
    const size_t count = std::min<size_t>(size, output.size() - offset);
    for (size_t i = 0; i < count; ++i) {
        output[offset + i] = static_cast<u8>(word >> (8 * i));
    }
}

u64 PixelOffset(bool linear, u32 x, u32 y, u32 width, u32 bpp) {
    return linear ? (u64{y} * width + x) * bpp :
                    u64{y & ~7u} * width * bpp + MortonOffset(x, y, bpp);
}

struct CopyRows {
    u32 width;
    u32 gap;
    u32 remaining;
    u64 offset = 0;

    CopyRows(u32 config, u32 size)
        : width(config >> 16 ? (config & 0xFFFF) * 16 : size),
          gap((config >> 16) * 16), remaining(width) {}

    u64 Extent(u32 size) const {
        return u64{size} + u64{(size - 1) / width} * gap;
    }

    void Advance(u32 amount) {
        offset += amount;
        remaining -= amount;
        if (remaining == 0) {
            offset += gap;
            remaining = width;
        }
    }
};

} // namespace

u32 BytesPerPixel(u32 format) {
    constexpr std::array<u32, 5> sizes{4, 3, 2, 2, 2};
    return format < sizes.size() ? sizes[format] : 0;
}

u32 MortonOffset(u32 x, u32 y, u32 bytes_per_pixel) {
    u32 pixel = (x & ~7u) * 8;
    for (u32 bit = 0; bit < 3; ++bit) {
        pixel |= ((x >> bit) & 1u) << (2 * bit);
        pixel |= ((y >> bit) & 1u) << (2 * bit + 1);
    }
    return pixel * bytes_per_pixel;
}

bool FillRegs::IsVram() const {
    return OutsideMainRam(StartAddress());
}

bool TransferRegs::IsInputVram() const {
    return OutsideMainRam(InputAddress());
}

bool TransferRegs::IsOutputVram() const {
    return OutsideMainRam(OutputAddress());
}

bool IsValidPhysicalAddress(Host& host, u32 paddr) {
    return host.PhysicalPointer(paddr, 0) != nullptr;
}

std::span<u8> Window(Host& host, u32 paddr, u64 size) {
    constexpr u64 max_request = std::numeric_limits<u32>::max();
    u32 high = static_cast<u32>(std::min({size, max_request, max_request + 1 - paddr}));
    if (auto* data = host.PhysicalPointer(paddr, high)) {
        return {data, high};
    }
    u8* const base = host.PhysicalPointer(paddr, 0);
    if (base == nullptr) {
        return {};
    }
    u32 low = 0;
    while (low < high) {
        const u32 mid = low + static_cast<u32>((u64{high} - low + 1) / 2);
        if (host.PhysicalPointer(paddr, mid) != nullptr) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return {base, low};
}

void MemoryFill(Host& host, const FillRegs& config) {
    const u32 start = config.StartAddress();
    const u32 end = config.EndAddress();
    if (start >= end || !IsValidPhysicalAddress(host, end)) {
        return;
    }
    const u32 unit = (config.control & 0x100) ? 3 : (config.control & 0x200) ? 4 : 2;
    const u64 range = u64{end} - start;
    const auto dest = Window(host, start, ((range + unit - 1) / unit) * unit);
    std::array<u8, 4> pattern{};
    for (u32 i = 0; i < unit; ++i) {
        pattern[i] = static_cast<u8>(config.value >> (8 * i));
    }
    size_t offset = 0;
    for (; offset + unit <= dest.size(); offset += unit) {
        std::memcpy(dest.data() + offset, pattern.data(), unit);
    }
    if (offset < dest.size()) {
        std::memcpy(dest.data() + offset, pattern.data(), dest.size() - offset);
    }
}

void DisplayTransfer(Host& host, const TransferRegs& config) {
    const u32 scaling = config.Scaling();
    const bool input_linear = config.InputLinear();
    const bool output_linear = config.DontSwizzle() == input_linear;
    const u32 in_format = config.InputFormat();
    const u32 out_format = config.OutputFormat();
    const u32 in_bpp = BytesPerPixel(in_format);
    const u32 out_bpp = BytesPerPixel(out_format);
    const u32 in_width = config.InputWidth();
    const u32 out_width = config.OutputWidth() >> (scaling != 0);
    const u32 out_height = config.OutputHeight() >> (scaling == 2);
    if (in_width == 0 || config.InputHeight() == 0 || out_width == 0 || out_height == 0 ||
        in_bpp == 0 || out_bpp == 0 || scaling == 3 || (scaling != 0 && input_linear)) {
        MH4N_LOG(Debug, kLog, "Unsupported display transfer");
        return;
    }

    const u32 x_step = scaling == 0 ? 1 : 2;
    const u32 y_step = scaling == 2 ? 2 : 1;
    const bool flip = config.FlipVertically();
    const u64 skew = flip && config.CropInputLines() && in_width > out_width
                         ? u64{in_width - out_width} * (out_height - 1) * out_bpp
                         : 0;

    const u32 last_x = (out_width - 1) * x_step;
    const u32 last_y = (out_height - 1) * y_step;
    const u64 input_size = PixelOffset(input_linear, last_x, input_linear ? last_y : last_y | 7u,
                                       in_width, in_bpp) + x_step * y_step * in_bpp;
    const u64 output_size = skew + PixelOffset(output_linear, out_width - 1,
                                               output_linear ? out_height - 1 : (out_height - 1) | 7u,
                                               out_width, out_bpp) + out_bpp;
    const auto input = Window(host, config.InputAddress(), input_size);
    const auto output = Window(host, config.OutputAddress(), output_size);
    if (input.data() == nullptr || skew >= output.size()) {
        return;
    }

    for (u32 y = 0; y < out_height; ++y) {
        const u32 dest_y = flip ? out_height - 1 - y : y;
        for (u32 x = 0; x < out_width; ++x) {
            const u64 dest = skew + PixelOffset(output_linear, x, dest_y, out_width, out_bpp);
            if (dest >= output.size()) {
                break;
            }
            const u64 src = PixelOffset(input_linear, x * x_step, y * y_step, in_width, in_bpp);
            Color color = ReadPixel(input, src, in_format, in_bpp);
            const u32 samples = x_step * y_step;
            for (u32 i = 1; i < samples; ++i) {
                const Color sample = ReadPixel(input, src + i * in_bpp, in_format, in_bpp);
                for (size_t channel = 0; channel < color.size(); ++channel) {
                    color[channel] += sample[channel];
                }
            }
            for (u32& channel : color) {
                channel /= samples;
            }
            WritePixel(output, static_cast<size_t>(dest), color, out_format, out_bpp);
        }
    }
}

void TextureCopy(Host& host, const TransferRegs& config) {
    u32 remaining = config.texture_copy_size & ~15u;
    CopyRows source(config.texture_copy_input_size, remaining);
    CopyRows dest(config.texture_copy_output_size, remaining);
    if (remaining == 0 || source.width == 0 || dest.width == 0) {
        MH4N_LOG(Debug, kLog, "Unsupported texture copy dimensions");
        return;
    }
    const auto input = Window(host, config.InputAddress(), source.Extent(remaining));
    const auto output = Window(host, config.OutputAddress(), dest.Extent(remaining));
    while (remaining && source.offset < input.size() && dest.offset < output.size()) {
        const u32 chunk = std::min({remaining, source.remaining, dest.remaining});
        const u64 count = std::min({u64{chunk}, input.size() - source.offset,
                                    output.size() - dest.offset});
        std::memmove(output.data() + static_cast<size_t>(dest.offset),
                     input.data() + static_cast<size_t>(source.offset), static_cast<size_t>(count));
        remaining -= chunk;
        source.Advance(chunk);
        dest.Advance(chunk);
    }
}

} // namespace mh4n::gpu::gx
