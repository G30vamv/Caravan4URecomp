// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Config Savegame and golden boot-village.ipc.trace.

#pragma once

#include <filesystem>
#include <map>
#include <span>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::wp11 {

inline constexpr u32 kConsoleIdBlock = 0x00090001;
inline constexpr u32 kModelBlock = 0x000F0004;
inline constexpr u16 kUserRead = 0x2;
inline constexpr u16 kSystemRead = 0x8;

class ConfigSave {
public:
    explicit ConfigSave(const std::filesystem::path& user_folder);

    std::span<const u8> ReadBlock(u32 id, u16 access) const;
    u8 Region() const { return region_; }

private:
    struct Block {
        u16 flags;
        std::vector<u8> bytes;
    };

    std::map<u32, Block> blocks_;
    u8 region_ = 1;
};

u64 TransferableId(std::span<const u8, 8> console_id, u32 salt);

}  // namespace mh4n::wp11
