// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: hash.h and gpu_texture_test.cpp.

#include "hash.h"
#include "cityhash.h"

#include <fmt/format.h>

namespace mh4n::gpu::texture {

std::optional<CustomHashes> ComputeCustomHashes(const u8* data, std::size_t size, u32 width,
                                                u32 height, Format format) {
    std::vector<u8> legacy;
    if (!DecodeForHash(data, size, width, height, format, legacy)) {
        return std::nullopt;
    }
    return CustomHashes{CityHash64(data, EncodedBytes(format, width, height)),
                        CityHash64(legacy.data(), legacy.size())};
}

std::string CustomTextureStem(u32 width, u32 height, u64 hash, Format format) {
    return fmt::format("tex1_{}x{}_{:016X}_{}", width, height, hash, static_cast<u32>(format));
}

} // namespace mh4n::gpu::texture
