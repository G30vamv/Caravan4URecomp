// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/loader.h and loader_test.cpp.

#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::loader_detail {

inline u64 ReadLe(const u8* bytes, unsigned width) {
    u64 value = 0;
    for (unsigned i = 0; i < width; ++i) {
        value |= u64{bytes[i]} << (8 * i);
    }
    return value;
}

inline void WriteLe(u8* bytes, unsigned width, u64 value) {
    for (unsigned i = 0; i < width; ++i) {
        bytes[i] = static_cast<u8>(value >> (8 * i));
    }
}

inline bool Fail(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
    return false;
}

inline bool ReadFile(const std::filesystem::path& path, const std::string& name,
                     std::vector<u8>& bytes, std::string* error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return Fail(error, "cannot open " + name);
    }
    const auto size = input.tellg();
    if (size < 0 || static_cast<u64>(size) > bytes.max_size()) {
        return Fail(error, "cannot size " + name);
    }
    bytes.resize(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return Fail(error, "cannot read " + name);
    }
    return true;
}

inline u64 RoundPage(u64 size) {
    return (size + 0xFFF) & ~u64{0xFFF};
}

}  // namespace mh4n::loader_detail
