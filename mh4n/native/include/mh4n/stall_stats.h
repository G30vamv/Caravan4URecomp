// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code. Counters the game thread fills while it works and the display reads at each VBlank, to say what a stall
// (a frame gap in the log) was spent on.
#pragma once

#include <atomic>
#include <mutex>
#include <utility>

#include "mh4n/types.h"

namespace mh4n::stall {

// Reading the game's files since the last VBlank. Time and bytes are one pair: they are added and taken under one mutex,
// so a sample never has the time of a read without its bytes.
struct FsReadSample {
    u64 us = 0;  // microseconds
    u64 bytes = 0;
};
inline std::mutex fs_read_mutex;
inline FsReadSample fs_reads;
inline void RecordFsRead(u64 us, u64 bytes) {
    std::lock_guard lock(fs_read_mutex);
    fs_reads.us += us;
    fs_reads.bytes += bytes;
}
inline FsReadSample TakeFsReads() {
    std::lock_guard lock(fs_read_mutex);
    return std::exchange(fs_reads, FsReadSample{});
}

inline std::atomic<u64> hd_upload_bytes{0};  // HD texture bytes uploaded

}  // namespace mh4n::stall
