// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew IRUSER Shared Memory, CRC-8-CCITT and Circle Pad Pro, boot-village.ipc.trace and MH4U's calibration reader.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <vector>

#include "mh4n/host.h"
#include "mh4n/types.h"

namespace mh4n::ir_user {

inline constexpr u32 kStatusSize = 0x10;
inline constexpr u32 kBufferInfoSize = 0x10;
inline constexpr u32 kPacketInfoSize = 8;
inline constexpr u32 kMaxPayloadSize = 0x3fff;
inline constexpr u8 kNetworkId = 1;

inline void WriteWord(std::span<u8> bytes, u32 offset, u32 value) {
    for (u32 i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (8 * i));
    }
}

inline u8 Crc8(std::span<const u8> bytes) {
    u8 crc = 0;
    for (const u8 byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<u8>((u32{crc} << 1) ^ ((crc & 0x80) != 0 ? 0x07u : 0u));
        }
    }
    return crc;
}

inline std::vector<u8> Frame(std::span<const u8> payload) {
    const auto size = static_cast<u32>(payload.size());
    std::vector<u8> bytes;
    bytes.reserve(size + 5);
    bytes.push_back(0xa5);
    bytes.push_back(kNetworkId);
    if (size < 0x40) {
        bytes.push_back(static_cast<u8>(size));
    } else {
        bytes.push_back(static_cast<u8>(0x40 | (size >> 8)));
        bytes.push_back(static_cast<u8>(size));
    }
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    bytes.push_back(Crc8(bytes));
    return bytes;
}

class PacketQueue {
public:
    static bool ValidLayout(u32 size, u32 slots) {
        return slots != 0 && u64{slots} * kPacketInfoSize < size;
    }

    PacketQueue() = default;
    PacketQueue(u32 info_offset, u32 size, u32 slots)
        : info_offset_(info_offset), capacity_(size - slots * kPacketInfoSize), sizes_(slots) {}

    bool Push(std::span<u8> shared, std::span<const u8> packet) {
        if (count_ == sizes_.size() || packet.size() > capacity_ - used_) {
            return false;
        }

        const auto slots = static_cast<u32>(sizes_.size());
        const auto length = static_cast<u32>(packet.size());
        const u32 table = info_offset_ + kBufferInfoSize;
        const u32 data = table + slots * kPacketInfoSize;
        const u32 tail = std::min(length, capacity_ - write_);
        std::copy_n(packet.begin(), tail, shared.begin() + data + write_);
        std::copy(packet.begin() + tail, packet.end(), shared.begin() + data);
        WriteWord(shared, table + end_ * kPacketInfoSize, write_);
        WriteWord(shared, table + end_ * kPacketInfoSize + 4, length);

        sizes_[end_] = length;
        used_ += length;
        write_ = static_cast<u32>((u64{write_} + length) % capacity_);
        end_ = (end_ + 1) % slots;
        ++count_;
        Publish(shared);
        return true;
    }

    bool Release(std::span<u8> shared, u32 count) {
        if (count > count_) {
            return false;
        }
        const auto slots = static_cast<u32>(sizes_.size());
        for (u32 i = 0; i < count; ++i) {
            used_ -= sizes_[start_];
            start_ = (start_ + 1) % slots;
        }
        count_ -= count;
        Publish(shared);
        return true;
    }

    void Clear(std::span<u8> shared) {
        start_ = end_ = count_ = write_ = used_ = 0;
        Publish(shared);
    }

private:
    void Publish(std::span<u8> shared) const {
        WriteWord(shared, info_offset_, start_);
        WriteWord(shared, info_offset_ + 4, end_);
        WriteWord(shared, info_offset_ + 8, count_);
    }

    u32 info_offset_ = 0;
    u32 capacity_ = 0;
    std::vector<u32> sizes_;
    u32 start_ = 0;
    u32 end_ = 0;
    u32 count_ = 0;
    u32 write_ = 0;
    u32 used_ = 0;
};

inline constexpr u32 kCenter = 0x800;  // MH4U calibration fallback at 0x0047db28.

inline void WriteAxes(std::span<u8, 3> bytes, u32 x, u32 y) {
    const u32 axes = x | (y << 12);
    for (u32 i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<u8>(axes >> (8 * i));
    }
}

inline std::array<u8, 6> InputPayload(const InputState& input) {
    // The matching calibration record uses scale 1.0 on each axis.
    const auto raw = [](s16 axis) {
        return static_cast<u32>(std::clamp(s32{kCenter} + s32{axis} * 8, 0, 0xfff));
    };
    std::array<u8, 6> payload{0x10};
    WriteAxes(std::span{payload}.subspan<1, 3>(), raw(input.cstick_x), raw(input.cstick_y));
    const u8 zl = input.zl ? 0 : 0x20;
    const u8 zr = input.zr ? 0 : 0x40;
    const u8 r = (input.buttons & kPadR) != 0 ? 0 : 0x80;
    payload[4] = static_cast<u8>(0x1f | zl | zr | r);
    return payload;
}

// Circle Pad Pro request 2, boot-village seq=2105. Each 16-byte record has its own CRC.
inline std::vector<u8> CalibrationPayload(std::span<const u8, 6> request) {
    constexpr u32 kRecordSize = 0x10;
    const u32 offset = u32{request[2]} | (u32{request[3]} << 8);
    const u32 size = u32{request[4]} | (u32{request[5]} << 8);
    if (offset % kRecordSize != 0 || size % kRecordSize != 0 || size > kMaxPayloadSize - 5) {
        return {};
    }

    // MH4U accepts records starting with 0 in the initial calibration read (0x0047da90).
    std::array<u8, kRecordSize> record{};
    WriteAxes(std::span{record}.subspan<1, 3>(), kCenter, kCenter);
    WriteWord(record, 4, std::bit_cast<u32>(1.0f));
    WriteWord(record, 8, std::bit_cast<u32>(1.0f));
    record.back() = Crc8(std::span{record}.first<kRecordSize - 1>());

    std::vector<u8> payload;
    payload.reserve(5 + size);
    payload.push_back(0x11);
    payload.insert(payload.end(), request.begin() + 2, request.end());
    for (u32 i = 0; i < size; i += kRecordSize) {
        payload.insert(payload.end(), record.begin(), record.end());
    }
    return payload;
}

}  // namespace mh4n::ir_user
