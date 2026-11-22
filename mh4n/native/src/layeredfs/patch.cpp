// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: patch.h, layeredfs_test.cpp and the IPS/BPS file formats.

#include "patch.h"

#include <algorithm>
#include <limits>

#include "mh4n/crc32.h"

namespace mh4n::layeredfs::patch {
namespace {

u32 ReadBig(const u8* data, unsigned count) {
    u32 value = 0;
    for (unsigned i = 0; i < count; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

u32 ReadLittle32(const u8* bytes) {
    return u32{bytes[0]} | (u32{bytes[1]} << 8) | (u32{bytes[2]} << 16) | (u32{bytes[3]} << 24);
}

bool ReadNumber(const std::vector<u8>& patch, std::size_t end, std::size_t& cursor, u64& value) {
    constexpr u64 max = std::numeric_limits<u64>::max();
    value = 0;
    u64 factor = 1;
    while (cursor < end) {
        const u8 byte = patch[cursor++];
        const u64 digit = byte & 0x7F;
        if (digit > (max - value) / factor) {
            return false;
        }
        value += digit * factor;
        if (byte & 0x80) {
            return true;
        }
        if (factor > max / 128 || factor * 128 > max - value) {
            return false;
        }
        factor *= 128;
        value += factor;
    }
    return false;
}

bool MoveOffset(u64 encoded, u64& offset) {
    const u64 distance = encoded >> 1;
    if (encoded & 1) {
        if (distance > offset) {
            return false;
        }
        offset -= distance;
    } else {
        if (distance > std::numeric_limits<u64>::max() - offset) {
            return false;
        }
        offset += distance;
    }
    return true;
}

}  // namespace

Status ApplyIps(const std::vector<u8>& ips, std::vector<u8>& buffer) {
    if (ips.size() < 5 || !std::equal(ips.begin(), ips.begin() + 5, "PATCH")) {
        return Status::ErrorPatches;
    }
    std::size_t pos = 5;
    while (ips.size() - pos > 3) {
        const u32 offset = ReadBig(ips.data() + pos, 3);
        if (offset == 0x454F46) {
            break;
        }
        if (ips.size() - pos < 5 || offset > buffer.size()) {
            return Status::ErrorPatches;
        }

        const u32 length = ReadBig(ips.data() + pos + 3, 2);
        pos += 5;
        if (length != 0) {
            if (length > ips.size() - pos || length > buffer.size() - offset) {
                return Status::ErrorPatches;
            }
            std::copy_n(ips.data() + pos, length, buffer.begin() + offset);
            pos += length;
        } else {
            if (ips.size() - pos < 3) {
                return Status::ErrorPatches;
            }
            const u32 repeat = ReadBig(ips.data() + pos, 2);
            if (repeat > buffer.size() - offset) {
                return Status::ErrorPatches;
            }
            std::fill_n(buffer.begin() + offset, repeat, ips[pos + 2]);
            pos += 3;
        }
    }
    return Status::Success;
}

Status ApplyBps(const std::vector<u8>& bps, std::vector<u8>& buffer) {
    if (bps.size() < 19 || !std::equal(bps.begin(), bps.begin() + 4, "BPS1")) {
        return Status::ErrorPatches;
    }
    const std::size_t end = bps.size() - 12;
    std::size_t cursor = 4;
    const auto number = [&](u64& value) { return ReadNumber(bps, end, cursor, value); };
    u64 source_size = 0, target_size = 0, metadata = 0;
    if (!number(source_size) || !number(target_size) || !number(metadata) || metadata != 0 ||
        source_size > buffer.size() || target_size > buffer.max_size()) {
        return Status::ErrorPatches;
    }
    if (Crc32(buffer.data(), static_cast<std::size_t>(source_size)) != ReadLittle32(bps.data() + end)) {
        return Status::ErrorPatchesInvalidTitle;
    }

    std::vector<u8> target;
    u64 source_offset = 0, target_offset = 0;
    while (cursor < end) {
        u64 command = 0;
        if (!number(command)) {
            return Status::ErrorPatches;
        }
        const u64 length = (command >> 2) + 1;
        const std::size_t written = target.size();
        if (length > target_size - written) {
            return Status::ErrorPatches;
        }
        switch (command & 3) {
        case 0:
            if (written > source_size || length > source_size - written) {
                return Status::ErrorPatches;
            }
            target.insert(target.end(), buffer.data() + written, buffer.data() + written + length);
            break;
        case 1:
            if (length > end - cursor) {
                return Status::ErrorPatches;
            }
            target.insert(target.end(), bps.data() + cursor, bps.data() + cursor + length);
            cursor += static_cast<std::size_t>(length);
            break;
        case 2: {
            u64 delta = 0;
            if (!number(delta) || !MoveOffset(delta, source_offset) || source_offset > source_size ||
                length > source_size - source_offset) {
                return Status::ErrorPatches;
            }
            target.insert(target.end(), buffer.data() + source_offset, buffer.data() + source_offset + length);
            source_offset += length;
            break;
        }
        case 3: {
            u64 delta = 0;
            if (!number(delta) || !MoveOffset(delta, target_offset) || target_offset >= written) {
                return Status::ErrorPatches;
            }
            target.resize(written + static_cast<std::size_t>(length));
            for (std::size_t i = written; i < target.size(); ++i) {
                target[i] = target[static_cast<std::size_t>(target_offset++)];
            }
            break;
        }
        }
    }
    if (target.size() != target_size || Crc32(target.data(), target.size()) != ReadLittle32(bps.data() + end + 4)) {
        return Status::ErrorPatches;
    }
    target.resize(std::max(target.size(), buffer.size()), 0);
    buffer.swap(target);
    return Status::Success;
}

}  // namespace mh4n::layeredfs::patch
