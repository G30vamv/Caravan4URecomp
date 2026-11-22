// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: cache.h, gpu_texture_test.cpp and the bundled xxHash API.

#include "cache.h"
#include "decode.h"
#include "mh4n/log.h"

#include <algorithm>
#include <cstring>
#include <utility>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace mh4n::gpu::texture {
namespace {

constexpr auto kLog = "GPU.Texture";
constexpr std::size_t kSpotPieces = 16;
constexpr std::size_t kSpotPieceBytes = 32;
constexpr std::size_t kSpotBytes = kSpotPieces * kSpotPieceBytes;

u32 KeyBytes(const CacheKey& key) {
    if (!IsValidSize(key.width, key.height) || !IsValidFormat(static_cast<u32>(key.format))) {
        return 0;
    }
    return static_cast<u32>(EncodedBytes(key.format, key.width, key.height));
}

u64 SliceHash(const u8* memory, std::size_t bytes, std::size_t slice) {
    const auto begin = bytes / kSlices * slice;
    const auto end = slice + 1 == kSlices ? bytes : begin + bytes / kSlices;
    return XXH3_64bits(memory + begin, end - begin);
}

u64 SpotHash(const u8* memory, std::size_t bytes) {
    if (bytes <= kSpotBytes) {
        return XXH3_64bits(memory, bytes);
    }
    std::array<u8, kSpotBytes> sample;
    for (std::size_t piece = 0; piece < kSpotPieces; ++piece) {
        const auto offset = piece * (bytes - kSpotPieceBytes) / (kSpotPieces - 1);
        std::memcpy(sample.data() + piece * kSpotPieceBytes, memory + offset, kSpotPieceBytes);
    }
    return XXH3_64bits(sample.data(), sample.size());
}

void RecordContent(CachedTexture& texture, u64 hash, const std::array<u64, kSlices>& slices,
                   const u8* memory, std::size_t bytes, u64 frame) {
    texture.content_hash = hash;
    texture.slices = slices;
    texture.used_frame = frame;
    texture.spot_frame = frame;
    texture.spot_hash = SpotHash(memory, bytes);
    texture.maybe_changed = false;
    texture.roll = 0;
}

} // namespace

u64 ContentHash(const u8* memory, std::size_t bytes, std::array<u64, kSlices>* slices) {
    if (bytes <= kSpotBytes) {
        if (slices) {
            slices->fill(0);
        }
        return XXH3_64bits(memory, bytes);
    }
    std::array<u64, kSlices> local;
    auto& hashes = slices ? *slices : local;
    for (std::size_t slice = 0; slice < kSlices; ++slice) {
        hashes[slice] = SliceHash(memory, bytes, slice);
    }
    return XXH3_64bits(hashes.data(), sizeof(hashes));
}

const CachedTexture* TextureCache::Get(const CacheKey& key) {
    const auto it = entries_.find(key);
    const u32 bytes = KeyBytes(key);
    if (it == entries_.end()) {
        const auto* memory = bytes ? host_.PhysicalPointer(key.paddr, bytes) : nullptr;
        if (!memory) {
            ++stats_.failures;
            return nullptr;
        }
        std::vector<u8> pixels;
        if (!DecodeToRgba8(memory, bytes, key.width, key.height, key.format, pixels)) {
            ++stats_.failures;
            return nullptr;
        }
        std::array<u64, kSlices> slices;
        const auto hash = ContentHash(memory, bytes, &slices);
        auto& entry = entries_.try_emplace(key).first->second;
        entry.key = key;
        entry.generation = ++next_generation_;
        entry.decoded_frame = frame_;
        RecordContent(entry, hash, slices, memory, bytes, frame_);
        SetPixels(entry, std::move(pixels));
        ++stats_.misses;
        max_bytes_ = std::max(max_bytes_, bytes);
        return &entry;
    }

    auto& entry = it->second;
    entry.used_frame = frame_;
    const bool spot_due = config_.spot_check && entry.spot_frame != frame_;
    const bool notified = entry.maybe_changed;
    if (!notified && !spot_due && !config_.verify) {
        ++stats_.hits;
        return &entry;
    }

    const auto* memory = host_.PhysicalPointer(key.paddr, bytes);
    if (!memory) {
        ++stats_.failures;
        Forget(it);
        return nullptr;
    }

    bool spotted = false;
    if (notified) {
        ++stats_.rehashes;
    } else if (spot_due) {
        entry.spot_frame = frame_;
        stats_.spot_bytes += std::min<std::size_t>(bytes, kSpotBytes);
        spotted = SpotHash(memory, bytes) != entry.spot_hash;
        if (!spotted && config_.rolling_check && bytes > kSpotBytes) {
            const auto slice = entry.roll;
            entry.roll = (slice + 1) % kSlices;
            stats_.spot_bytes += bytes / kSlices;
            spotted = SliceHash(memory, bytes, slice) != entry.slices[slice];
            stats_.rolled += spotted;
        }
        stats_.spotted += spotted;
    }

    if (notified || spotted || config_.verify) {
        std::array<u64, kSlices> slices;
        const auto hash = ContentHash(memory, bytes, &slices);
        if (hash != entry.content_hash) {
            if (config_.verify && !notified && !spotted && ++stats_.stale <= 40) {
                MH4N_LOG(Warning, kLog, "Texture at {:08X} ({}x{}, format {}) changed without a notification",
                         key.paddr, key.width, key.height, static_cast<u32>(key.format));
            }
            if (!Decode(entry, memory, bytes)) {
                ++stats_.failures;
                Forget(it);
                return nullptr;
            }
            entry.changed_once = true;
            entry.generation = ++next_generation_;
            entry.decoded_frame = frame_;
            RecordContent(entry, hash, slices, memory, bytes, frame_);
            ++stats_.redecodes;
            return &entry;
        }
        if (notified) {
            entry.maybe_changed = false;
            entry.spot_frame = frame_;
            ++stats_.unchanged;
            return &entry;
        }
    }

    ++stats_.hits;
    return &entry;
}

void TextureCache::Notify(u32 paddr, u32 size) {
    if (size == 0) {
        return;
    }
    if (config_.check_all_on_notify) {
        for (auto& [key, texture] : entries_) {
            texture.maybe_changed = true;
        }
        return;
    }

    const u32 start = paddr > max_bytes_ ? paddr - max_bytes_ : 0;
    const u64 end = static_cast<u64>(paddr) + size;
    for (auto it = entries_.lower_bound(CacheKey{start});
         it != entries_.end() && it->first.paddr < end; ++it) {
        const auto& key = it->first;
        if (static_cast<u64>(key.paddr) + KeyBytes(key) > paddr) {
            it->second.maybe_changed = true;
        }
    }
}

bool TextureCache::Insert(const CacheKey& key, u64 hash, std::vector<u8>&& rgba8) {
    const u32 bytes = KeyBytes(key);
    const auto* memory = bytes ? host_.PhysicalPointer(key.paddr, bytes) : nullptr;
    if (!memory || rgba8.size() != static_cast<std::size_t>(key.width) * key.height * 4) {
        return false;
    }

    std::array<u64, kSlices> slices;
    if (ContentHash(memory, bytes, &slices) != hash) {
        return false;
    }
    auto [it, inserted] = entries_.try_emplace(key);
    auto& texture = it->second;
    texture.key = key;
    if (inserted || texture.content_hash != hash) {
        texture.generation = ++next_generation_;
        texture.decoded_frame = frame_;
        texture.changed_once = !inserted;
        if (inserted) {
            ++stats_.misses;
        } else {
            ++stats_.redecodes;
        }
    }
    RecordContent(texture, hash, slices, memory, bytes, frame_);
    SetPixels(texture, std::move(rgba8));
    max_bytes_ = std::max(max_bytes_, bytes);
    return true;
}

const CachedTexture* TextureCache::Pixels(const CacheKey& key) {
    const auto* cached = Get(key);
    if (!cached || !cached->rgba8.empty()) {
        return cached;
    }
    const auto it = entries_.find(key);
    auto& texture = it->second;
    const auto bytes = KeyBytes(key);
    const auto* memory = host_.PhysicalPointer(key.paddr, bytes);
    if (!memory || !Decode(texture, memory, bytes)) {
        ++stats_.failures;
        Forget(it);
        return nullptr;
    }
    std::array<u64, kSlices> slices;
    const auto hash = ContentHash(memory, bytes, &slices);
    if (hash != texture.content_hash) {
        texture.changed_once = true;
        texture.generation = ++next_generation_;
        texture.decoded_frame = frame_;
    }
    RecordContent(texture, hash, slices, memory, bytes, frame_);
    ++stats_.repixels;
    return &texture;
}

void TextureCache::ReleasePixels(const CacheKey& key) {
    if (config_.release_pixels) {
        const auto it = entries_.find(key);
        if (it != entries_.end() && !it->second.changed_once) {
            SetPixels(it->second, {});
        }
    }
}

std::size_t TextureCache::Sweep(u64 unused_frames, u64 settled_frames) {
    std::size_t swept = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto current = it++;
        auto& texture = current->second;
        if (frame_ - texture.used_frame > unused_frames) {
            Forget(current);
            ++swept;
        } else if (config_.release_pixels && texture.changed_once &&
                   frame_ - texture.decoded_frame > settled_frames) {
            SetPixels(texture, {});
        }
    }
    stats_.swept += swept;
    return swept;
}

bool TextureCache::Decode(CachedTexture& texture, const u8* memory, std::size_t bytes) {
    const auto previous_bytes = texture.rgba8.size();
    const auto& key = texture.key;
    if (!DecodeToRgba8(memory, bytes, key.width, key.height, key.format, texture.rgba8)) {
        return false;
    }
    pixel_bytes_ = pixel_bytes_ - previous_bytes + texture.rgba8.size();
    return true;
}

void TextureCache::SetPixels(CachedTexture& texture, std::vector<u8>&& rgba8) {
    pixel_bytes_ -= texture.rgba8.size();
    texture.rgba8 = std::move(rgba8);
    pixel_bytes_ += texture.rgba8.size();
}

void TextureCache::Forget(std::map<CacheKey, CachedTexture>::iterator it) {
    pixel_bytes_ -= it->second.rgba8.size();
    entries_.erase(it);
}

} // namespace mh4n::gpu::texture
