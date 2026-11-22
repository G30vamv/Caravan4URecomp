// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: Google CityHash 1.1 algorithm (github.com/google/cityhash, MIT) and published CityHash64 vectors.

/*
 * Copyright (c) 2011 Google, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "cityhash.h"

#include <bit>
#include <cstring>

namespace mh4n::gpu::texture {
namespace {

constexpr u64 k0 = 0xc3a5c85c97cb3127ULL;
constexpr u64 k1 = 0xb492b66fbe98f273ULL;
constexpr u64 k2 = 0x9ae16a3b2f90404fULL;

template <typename Word>
Word LoadLittle(const char* data) {
    if constexpr (std::endian::native == std::endian::little) {
        Word value;
        std::memcpy(&value, data, sizeof(value));
        return value;
    } else {
        Word value = 0;
        for (std::size_t offset = 0; offset < sizeof(value); ++offset) {
            value |= static_cast<Word>(static_cast<u8>(data[offset])) << (8 * offset);
        }
        return value;
    }
}

u64 ReverseBytes(u64 word) {
    const u64 halves = (word << 32) | (word >> 32);
    const u64 pairs = ((halves & 0x0000ffff0000ffffULL) << 16) |
                      ((halves >> 16) & 0x0000ffff0000ffffULL);
    return ((pairs & 0x00ff00ff00ff00ffULL) << 8) | ((pairs >> 8) & 0x00ff00ff00ff00ffULL);
}

u64 Fold(u64 word) {
    return word ^ (word >> 47);
}

u64 Mix(u64 left, u64 right, u64 factor = 0x9ddfea08eb382d69ULL) {
    return Fold((Fold((left ^ right) * factor) ^ right) * factor) * factor;
}

struct BlockState {
    u64 sum;
    u64 mixed;
};

BlockState MixBlock(const char* block, u64 seed, u64 salt) {
    const u64 first = seed + LoadLittle<u64>(block);
    const u64 middle = first + LoadLittle<u64>(block + 8) + LoadLittle<u64>(block + 16);
    const u64 last = LoadLittle<u64>(block + 24);
    return {middle + last, first + std::rotr(salt + first + last, 21) + std::rotr(middle, 44)};
}

u64 HashThrough32(const char* data, std::size_t length) {
    if (length == 0) {
        return k2;
    }
    const u64 factor = k2 + 2 * static_cast<u64>(length);
    if (length < 4) {
        const u64 ends = static_cast<u8>(data[0]) + 256u * static_cast<u8>(data[length / 2]);
        const u64 tail = length + 4u * static_cast<u8>(data[length - 1]);
        return Fold((ends * k2) ^ (tail * k0)) * k2;
    }
    const char* end = data + length;
    if (length < 8) {
        return Mix(length + 8 * static_cast<u64>(LoadLittle<u32>(data)),
                   LoadLittle<u32>(end - 4), factor);
    }
    if (length <= 16) {
        const u64 first = LoadLittle<u64>(data) + k2;
        const u64 last = LoadLittle<u64>(end - 8);
        return Mix(first + factor * std::rotr(last, 37),
                   factor * (last + std::rotr(first, 25)), factor);
    }

    const u64 first = LoadLittle<u64>(data) * k1;
    const u64 second = LoadLittle<u64>(data + 8);
    const u64 last = LoadLittle<u64>(end - 8) * factor;
    return Mix(std::rotr(first + second, 43) + std::rotr(last, 30) +
                   LoadLittle<u64>(end - 16) * k2,
               first + std::rotr(second + k2, 18) + last, factor);
}

u64 HashThrough64(const char* data, std::size_t length) {
    const auto* end = data + length;
    const u64 factor = k2 + 2 * static_cast<u64>(length);
    const u64 edge = LoadLittle<u64>(data) * k2 + LoadLittle<u64>(end - 8);
    const u64 second = LoadLittle<u64>(data + 8);
    const u64 tail_third = LoadLittle<u64>(end - 24);
    const u64 tail_fourth = LoadLittle<u64>(end - 32);
    const u64 middle = LoadLittle<u64>(data + 16) * k2 + 9 * LoadLittle<u64>(data + 24);
    const u64 tail_first = LoadLittle<u64>(end - 8);
    const u64 tail_second = LoadLittle<u64>(end - 16) * factor;

    const u64 edge_rotated = std::rotr(edge, 43) + 9 * (std::rotr(second, 30) + tail_third);
    const u64 edge_xored = (edge ^ tail_fourth) + 9 * LoadLittle<u64>(data + 24) + 1;
    const u64 first_mix = ReverseBytes((edge_rotated + edge_xored) * factor) + tail_second;
    const u64 second_mix = (ReverseBytes((edge_xored + first_mix) * factor) + tail_first) * factor;
    const u64 middle_rotated = std::rotr(middle, 42) + tail_third;
    const u64 middle_sum = middle + tail_third;
    const u64 folded = ReverseBytes((middle_rotated + middle_sum) * factor + second_mix) + second;
    return middle_rotated + Fold((middle_sum + folded) * factor + tail_fourth + tail_second) * factor;
}

} // namespace

u64 CityHash64(const char* data, std::size_t length) {
    if (length <= 32) {
        return HashThrough32(data, length);
    }
    if (length <= 64) {
        return HashThrough64(data, length);
    }

    // Seed from the final 64 bytes, then consume complete blocks from the start.
    const char* end = data + length;
    const u64 tail_seed = LoadLittle<u64>(end - 40);
    u64 center = LoadLittle<u64>(end - 16) + LoadLittle<u64>(end - 56);
    u64 right = Mix(LoadLittle<u64>(end - 48) + length, LoadLittle<u64>(end - 24));
    u64 left = tail_seed * k1 + LoadLittle<u64>(data);
    auto front = MixBlock(end - 64, length, right);
    auto back = MixBlock(end - 32, center + k1, tail_seed);

    const std::size_t blocks = (length - 1) / 64;
    for (std::size_t index = 0; index < blocks; ++index, data += 64) {
        const u64 next_right =
            (std::rotr(left + center + front.sum + LoadLittle<u64>(data + 8), 37) * k1) ^
            back.mixed;
        const u64 next_left = std::rotr(right + back.sum, 33) * k1;
        center = std::rotr(center + front.mixed + LoadLittle<u64>(data + 48), 42) * k1 +
                 front.sum + LoadLittle<u64>(data + 40);
        front = MixBlock(data, front.mixed * k1, next_right + back.sum);
        back = MixBlock(data + 32, next_left + back.mixed, center + LoadLittle<u64>(data + 16));
        left = next_left;
        right = next_right;
    }
    return Mix(Mix(front.sum, back.sum) + Fold(center) * k1 + right,
               Mix(front.mixed, back.mixed) + left);
}

} // namespace mh4n::gpu::texture
