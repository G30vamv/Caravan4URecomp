// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: key_codec.h, files.h and the pipeline cache's observed byte layout.
#pragma once

#include <bit>
#include <concepts>
#include <span>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::gpu::pipeline::disk {

template <std::unsigned_integral T>
void Append(std::vector<u8>& bytes, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        bytes.push_back(static_cast<u8>(value >> (8 * i)));
    }
}

class Reader {
public:
    explicit Reader(std::span<const u8> bytes) : bytes_(bytes) {}

    std::span<const u8> Take(size_t size) {
        if (size > bytes_.size()) {
            good_ = false;
            return {};
        }
        const auto result = bytes_.first(size);
        bytes_ = bytes_.subspan(size);
        return result;
    }

    template <std::unsigned_integral T>
    T Read() {
        u64 value = 0;
        size_t shift = 0;
        for (u8 byte : Take(sizeof(T))) {
            value |= static_cast<u64>(byte) << shift;
            shift += 8;
        }
        return static_cast<T>(value);
    }

    template <auto Last>
    auto Enum() {
        const u8 value = Read<u8>();
        good_ &= value <= static_cast<u8>(Last);
        return static_cast<decltype(Last)>(value);
    }

    bool Flag() {
        const u8 value = Read<u8>();
        good_ &= value <= 1;
        return value != 0;
    }

    size_t Remaining() const { return bytes_.size(); }
    bool Done() const { return good_ && bytes_.empty(); }

private:
    std::span<const u8> bytes_;
    bool good_ = true;
};

}  // namespace mh4n::gpu::pipeline::disk
