// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Memory layout, mh4n/memory.h, and memory_test.cpp.

#include "mh4n/memory.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>

#include "mh4n/assert.h"

namespace mh4n {
namespace {

constexpr std::array<u32, 8> kApplicationSizes{
    0x04000000, 0, 0x06000000, 0x05000000, 0x04800000, 0x02000000,
    0x07C00000, 0x0B200000,
};

} // namespace

bool FcramRegions::ValidMemoryMode(u32 memory_mode) {
    return memory_mode < kApplicationSizes.size() && kApplicationSizes[memory_mode] != 0;
}

FcramRegions::FcramRegions(u32 memory_mode) : mode_(memory_mode) {
    MH4N_ASSERT(ValidMemoryMode(mode_));
    const u32 application = kApplicationSizes[mode_];
    const u32 base = mode_ >= 6 ? 0x02000000 : 0x01400000;
    const std::array sizes{application, TotalSize() - application - base, base};
    u32 offset = 0;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        Region& region = regions_[i];
        region.base = offset;
        region.size = sizes[i];
        offset += region.size;
        region.free.emplace(region.base, offset);
    }
}

u32 FcramRegions::TotalSize() const {
    return mode_ >= 6 ? kFcramSizeNew3ds : kFcramSize;
}

std::vector<FcramRange> FcramRegions::FreeBlocks(FcramRegion region) const {
    const Region& r = At(region);
    std::vector<FcramRange> blocks;
    blocks.reserve(r.free.size());
    for (const auto& [lo, hi] : r.free) {
        blocks.push_back({lo, hi - lo});
    }
    return blocks;
}

void FcramRegions::RemoveFree(Region& r, u32 lo, u32 hi) {
    auto block = std::prev(r.free.upper_bound(lo));
    const u32 block_end = block->second;
    if (hi < block_end) {
        r.free.emplace(hi, block_end);
    }
    if (block->first == lo) {
        r.free.erase(block);
    } else {
        block->second = lo;
    }
    r.used += hi - lo;
}

void FcramRegions::AddFree(Region& r, u32 lo, u32 hi) {
    auto right = r.free.lower_bound(lo);
    auto left = right == r.free.begin() ? r.free.end() : std::prev(right);
    const bool joins_right = right != r.free.end() && right->first == hi;
    const u32 end = joins_right ? right->second : hi;
    if (left != r.free.end() && left->second == lo) {
        left->second = end;
    } else {
        r.free.emplace_hint(right, lo, end);
    }
    if (joins_right) {
        r.free.erase(right);
    }
    r.used -= hi - lo;
}

std::vector<FcramRange> FcramRegions::HeapAllocate(FcramRegion region, u32 size) {
    Region& r = At(region);
    if (size == 0 || size > r.size - r.used) {
        return {};
    }
    std::vector<FcramRange> blocks;
    u32 remaining = size;
    for (auto it = r.free.rbegin(); remaining != 0; ++it) {
        const auto [lo, hi] = *it;
        const u32 take = std::min(remaining, hi - lo);
        blocks.push_back({hi - take, take});
        remaining -= take;
    }
    std::reverse(blocks.begin(), blocks.end());
    for (const auto& [offset, length] : blocks) {
        RemoveFree(r, offset, offset + length);
    }
    return blocks;
}

bool FcramRegions::LinearAllocate(FcramRegion region, u32 offset, u32 size) {
    Region& r = At(region);
    const auto after = r.free.upper_bound(offset);
    if (size == 0 || after == r.free.begin() ||
        u64{offset} + size > std::prev(after)->second) {
        return false;
    }
    RemoveFree(r, offset, offset + size);
    return true;
}

std::optional<u32> FcramRegions::LinearAllocate(FcramRegion region, u32 size) {
    Region& r = At(region);
    if (size == 0) {
        return std::nullopt;
    }
    for (const auto& [lo, hi] : r.free) {
        if (hi - lo >= size) {
            const u32 offset = lo;
            RemoveFree(r, offset, offset + size);
            return offset;
        }
    }
    return std::nullopt;
}

std::optional<u32> FcramRegions::RLinearAllocate(FcramRegion region, u32 size) {
    Region& r = At(region);
    if (size == 0) {
        return std::nullopt;
    }
    for (auto it = r.free.rbegin(); it != r.free.rend(); ++it) {
        const auto [lo, hi] = *it;
        if (hi - lo >= size) {
            RemoveFree(r, hi - size, hi);
            return hi - size;
        }
    }
    return std::nullopt;
}

bool FcramRegions::Free(FcramRegion region, u32 offset, u32 size) {
    Region& r = At(region);
    const u64 end = u64{offset} + size;
    if (size == 0 || offset < r.base || end > r.base + r.size) {
        return false;
    }
    const auto right = r.free.lower_bound(offset);
    if ((right != r.free.end() && right->first < end) ||
        (right != r.free.begin() && std::prev(right)->second > offset)) {
        return false;
    }
    AddFree(r, offset, static_cast<u32>(end));
    return true;
}

} // namespace mh4n
