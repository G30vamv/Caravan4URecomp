// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: fragment register and table contracts and the fog, lighting and procedural-texture black-box tests.

#pragma once

#include <bit>
#include <cmath>
#include <cstring>

#include "tables.h"

namespace mh4n::gpu::fragment::detail {

inline s32 SignExtend(u32 word, u32 bits) {
    const u32 sign = 1u << (bits - 1);
    return static_cast<s32>(((word & (2 * sign - 1)) ^ sign) - sign);
}

inline float DecodeFloat(u32 word, u32 fraction_bits, u32 exponent_bits) {
    const u32 fraction = Bits(word, 0, fraction_bits);
    const u32 exponent = Bits(word, fraction_bits, exponent_bits);
    const u32 sign = Bits(word, fraction_bits + exponent_bits, 1) << 31;
    if (exponent == (1u << exponent_bits) - 1) {
        return std::bit_cast<float>(sign | 0x7F800000u | (fraction << (23 - fraction_bits)));
    }
    const u32 significand = exponent == 0 ? fraction : fraction | (1u << fraction_bits);
    const int bias = (1 << (exponent_bits - 1)) - 1;
    const float value = std::ldexp(static_cast<float>(significand),
                                   static_cast<int>(exponent == 0 ? 1 : exponent) - bias
                                       - static_cast<int>(fraction_bits));
    return sign == 0 ? value : -value;
}

template <typename Entry, size_t Size, typename Decode>
void UploadLut(TableUpload& upload, TableRing ring, const std::array<u32, Size>& words,
               int& offset, Decode decode) {
    const auto slice = upload.rings.Take(ring, Size * sizeof(Entry));
    if (!slice) {
        return;
    }
    for (size_t i = 0; i < Size; ++i) {
        const Entry entry = decode(words[i]);
        std::memcpy(slice->data.data() + i * sizeof(Entry), &entry, sizeof(entry));
    }
    offset = slice->ElementOffset(sizeof(Entry));
    upload.dirty.Mark();
}

} // namespace mh4n::gpu::fragment::detail
