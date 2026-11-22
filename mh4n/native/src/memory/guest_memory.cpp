// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/memory.h, host.h, memory_test.cpp, 3dbrew Memory layout, and spec/kernel_test_vectors.tsv.

#include "mh4n/memory.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "host.h"
#include "mh4n/assert.h"

namespace mh4n {
namespace {

struct CommitFailed {};

u64 PageStart(u64 address, u64 page_size) {
    return address & ~(page_size - 1);
}

u64 PageEnd(u64 address, u64 page_size) {
    return PageStart(address + page_size - 1, page_size);
}

bool ValidStartAndSize(u32 address, u32 size) {
    return address < kManagedEnd && size != 0 && ((address | size) & kPageMask) == 0;
}

} // namespace

const std::vector<ZeroRegion>& DefaultZeroRegions() {
    static const std::vector<ZeroRegion> regions{
#include "mh4n_zero_regions.inc"
    };
    return regions;
}

ZeroPolicy DefaultZeroPolicy() {
#if MH4N_DEBUG_GUARD
    return ZeroPolicy::Fault;
#else
    return ZeroPolicy::ZeroFill;
#endif
}

std::unique_ptr<GuestMemory> GuestMemory::Create(const GuestMemoryOptions& options) {
    auto memory = std::unique_ptr<GuestMemory>(new GuestMemory);
    if (!memory->Init(options)) {
        return nullptr;
    }
    return memory;
}

bool GuestMemory::Init(const GuestMemoryOptions& options) {
    const std::size_t native_page = host::PageSize();
    if (!std::has_single_bit(native_page) || native_page > kGuardSize) {
        return false;
    }
    const std::size_t min_page = std::max<std::size_t>(kPageSize, native_page);
    host_page_ = options.host_page_size == 0 ? min_page : options.host_page_size;
    if (!std::has_single_bit(host_page_) || host_page_ < min_page ||
        host_page_ > kAddressSpaceSize) {
        return false;
    }
    for (const ZeroRegion& region : options.zero_regions) {
        if (region.start > region.last || (region.start & kPageMask) != 0 ||
            (region.last & kPageMask) != kPageMask) {
            return false;
        }
    }
    zero_policy_ = options.zero_policy;
    zero_regions_ = options.zero_regions;
    blocks_.emplace(0, Block{kManagedEnd, MemPerm::None, MemState::Free, false, kNoBacking});

    static_assert(sizeof(std::size_t) >= sizeof(u64));
    reservation_size_ = kAddressSpaceSize + kGuardSize;
    auto* base = static_cast<u8*>(host::Reserve(reservation_size_));
    if (base == nullptr) {
        return false;
    }
    if (!fault::RegisterReservation(reinterpret_cast<std::uintptr_t>(base), reservation_size_)) {
        host::Release(base, reservation_size_);
        return false;
    }
    base_ = base;
    host::InstallFaultHandler();
    PrepareThreadForFaults();
    try {
        HostCommitZeroRegions(0, kAddressSpaceSize);
    } catch (const CommitFailed&) {
        return false;
    }
    return Map(kVramVaddr, kVramSize, MemPerm::ReadWrite, MemState::IO) == 0 &&
           Map(kDspRamVaddr, kDspRamSize, MemPerm::ReadWrite, MemState::IO) == 0;
}

GuestMemory::~GuestMemory() {
    if (base_ != nullptr) {
        fault::UnregisterReservation(reinterpret_cast<std::uintptr_t>(base_));
        host::Release(base_, reservation_size_);
    }
}

void GuestMemory::PrepareThreadForFaults() {
    host::InstallThreadFaultStack();
}

GuestMemory::BlockMap::iterator GuestMemory::FindBlock(u32 vaddr) {
    return std::prev(blocks_.upper_bound(vaddr));
}

GuestMemory::BlockMap::const_iterator GuestMemory::FindBlock(u32 vaddr) const {
    return std::prev(blocks_.upper_bound(vaddr));
}

GuestMemory::BlockMap::iterator GuestMemory::Split(BlockMap::iterator it, u32 offset) {
    if (offset == 0) {
        return it;
    }
    Block right = it->second;
    right.size -= offset;
    if (right.backing != kNoBacking) {
        right.backing += offset;
    }
    auto result = blocks_.emplace_hint(std::next(it), it->first + offset, right);
    it->second.size = offset;
    return result;
}

bool GuestMemory::CanMerge(const Block& a, const Block& b) {
    if (a.mapped != b.mapped || a.state != b.state || a.perm != b.perm) {
        return false;
    }
    if (a.backing == kNoBacking || b.backing == kNoBacking) {
        return a.backing == b.backing;
    }
    return b.backing >= a.backing && b.backing - a.backing == a.size;
}

GuestMemory::BlockMap::iterator GuestMemory::MergeAdjacent(BlockMap::iterator it) {
    if (it != blocks_.begin()) {
        auto left = std::prev(it);
        if (CanMerge(left->second, it->second)) {
            left->second.size += it->second.size;
            blocks_.erase(it);
            it = left;
        }
    }
    for (auto right = std::next(it);
         right != blocks_.end() && CanMerge(it->second, right->second);
         right = blocks_.erase(right)) {
        it->second.size += right->second.size;
    }
    return it;
}

bool GuestMemory::HostPageWanted(u64 page, u64 page_size) const {
    const u64 end = page + page_size;
    if (page < kManagedEnd) {
        auto it = FindBlock(static_cast<u32>(page));
        for (; it != blocks_.end() && it->first < end; ++it) {
            if (it->second.mapped) {
                return true;
            }
        }
    }
    if (zero_policy_ == ZeroPolicy::ZeroFill) {
        return std::any_of(zero_regions_.begin(), zero_regions_.end(), [page, end](ZeroRegion region) {
            return region.start < end && region.last >= page;
        });
    }
    return false;
}

void GuestMemory::HostCommitZeroRegions(u64 lo, u64 hi) {
    if (zero_policy_ == ZeroPolicy::Fault) {
        return;
    }
    for (const ZeroRegion& region : zero_regions_) {
        const u64 begin = std::max(lo, PageStart(region.start, host_page_));
        const u64 end = std::min(hi, PageEnd(u64{region.last} + 1, host_page_));
        if (begin < end && !host::Commit(base_ + begin, end - begin)) {
            throw CommitFailed{};
        }
    }
}

void GuestMemory::HostMap(u32 lo, u32 hi) {
    const u64 begin = PageStart(lo, host_page_);
    const u64 end = PageEnd(hi, host_page_);
    if (!host::Commit(base_ + begin, end - begin)) {
        throw CommitFailed{};
    }

    // The OS supplies zeros for fresh pages. Clear only bytes that could have been accessible.
    const auto clear = [&](u64 from, u64 to) {
        from = std::max<u64>(from, lo);
        to = std::min<u64>(to, hi);
        if (from < to) {
            std::memset(base_ + from, 0, to - from);
        }
    };
    if (lo != begin && HostPageWanted(begin, host_page_)) {
        clear(begin, begin + host_page_);
    }
    const u64 last = end - host_page_;
    if (hi != end && (last != begin || lo == begin) && HostPageWanted(last, host_page_)) {
        clear(last, end);
    }
    if (zero_policy_ == ZeroPolicy::ZeroFill) {
        for (const ZeroRegion& region : zero_regions_) {
            clear(PageStart(region.start, host_page_), PageEnd(u64{region.last} + 1, host_page_));
        }
    }
}

void GuestMemory::HostUnmap(u32 lo, u32 hi) {
    const u64 end = PageEnd(hi, host_page_);
    u64 decommit_start = PageStart(lo, host_page_);
    for (u64 page = decommit_start; page < end; page += host_page_) {
        if (!HostPageWanted(page, host_page_)) {
            continue;
        }
        if (decommit_start < page) {
            MH4N_ASSERT(host::Decommit(base_ + decommit_start, page - decommit_start));
        }
        const u64 clear_start = std::max<u64>(lo, page);
        const u64 clear_end = std::min<u64>(hi, page + host_page_);
        std::memset(base_ + clear_start, 0, clear_end - clear_start);
        decommit_start = page + host_page_;
    }
    if (decommit_start < end) {
        MH4N_ASSERT(host::Decommit(base_ + decommit_start, end - decommit_start));
    }
}

Result GuestMemory::Map(u32 vaddr, u32 size, MemPerm perm, MemState state, u64 backing) {
    if (!ValidStartAndSize(vaddr, size)) {
        return kResultInvalidAddress;
    }
    const std::lock_guard lock(mutex_);
    auto it = FindBlock(vaddr);
    const u32 offset = vaddr - it->first;
    if (it->second.mapped || size > it->second.size - offset) {
        return kResultInvalidAddressState;
    }
    it = Split(it, offset);
    if (size < it->second.size) {
        Split(it, size);
    }
    try {
        HostMap(vaddr, vaddr + size);
    } catch (const CommitFailed&) {
        MergeAdjacent(it);
        return kResultInvalidAddressState;
    }
    it->second = {size, perm, state, true, backing};
    MergeAdjacent(it);
    map_changes_.fetch_add(1, std::memory_order_release);
    return 0;
}

Result GuestMemory::CarveRange(u32 vaddr, u32 size, BlockMap::iterator* first) {
    if (!ValidStartAndSize(vaddr, size)) {
        return kResultInvalidAddress;
    }
    if (!IsMappedLocked(vaddr, size)) {
        return kResultInvalidAddressState;
    }
    const u32 end = vaddr + size;
    if (end < kManagedEnd) {
        const auto last = FindBlock(end);
        Split(last, end - last->first);
    }
    const auto begin = FindBlock(vaddr);
    *first = Split(begin, vaddr - begin->first);
    return 0;
}

Result GuestMemory::Unmap(u32 vaddr, u32 size) {
    const std::lock_guard lock(mutex_);
    BlockMap::iterator first;
    if (const Result result = CarveRange(vaddr, size, &first); result != 0) {
        return result;
    }
    const u32 end = vaddr + size;
    blocks_.erase(std::next(first), blocks_.lower_bound(end));
    first->second = {size, MemPerm::None, MemState::Free, false, kNoBacking};
    MergeAdjacent(first);
    HostUnmap(vaddr, end);
    map_changes_.fetch_add(1, std::memory_order_release);
    return 0;
}

Result GuestMemory::Protect(u32 vaddr, u32 size, MemPerm perm) {
    const std::lock_guard lock(mutex_);
    BlockMap::iterator first;
    if (const Result result = CarveRange(vaddr, size, &first); result != 0) {
        return result;
    }
    const u32 end = vaddr + size;
    for (auto it = first; it != blocks_.end() && it->first < end;) {
        it->second.perm = perm;
        it = std::next(MergeAdjacent(it));
    }
    return 0;
}

Result GuestMemory::ChangeState(u32 vaddr, u32 size, MemState expected_state, MemPerm expected_perm,
                                MemState new_state, MemPerm new_perm) {
    if (!ValidStartAndSize(vaddr, size)) {
        return kResultInvalidAddress;
    }
    const std::lock_guard lock(mutex_);
    if (!IsMappedLocked(vaddr, size)) {
        return kResultInvalidAddressState;
    }
    const u32 end = vaddr + size;
    for (auto it = FindBlock(vaddr); it != blocks_.end() && it->first < end; ++it) {
        const Block& block = it->second;
        if (block.state != expected_state || (block.perm & expected_perm) != expected_perm) {
            return kResultInvalidAddressState;
        }
    }
    BlockMap::iterator first;
    const Result result = CarveRange(vaddr, size, &first);
    MH4N_ASSERT(result == 0);
    for (auto it = first; it != blocks_.end() && it->first < end;) {
        it->second.state = new_state;
        it->second.perm = new_perm;
        it = std::next(MergeAdjacent(it));
    }
    return 0;
}

std::optional<MemoryInfo> GuestMemory::Query(u32 vaddr) const {
    if (vaddr >= kManagedEnd) {
        return std::nullopt;
    }
    const std::lock_guard lock(mutex_);
    const auto [start, block] = *FindBlock(vaddr);
    return MemoryInfo{start, block.size, block.perm, block.state};
}

bool GuestMemory::IsMappedLocked(u32 vaddr, u32 size) const {
    if (size == 0) {
        return true;
    }
    if (u64{vaddr} + size > kManagedEnd) {
        return false;
    }
    const u32 end = vaddr + size;
    for (auto it = FindBlock(vaddr); it != blocks_.end() && it->first < end; ++it) {
        if (!it->second.mapped) {
            return false;
        }
    }
    return true;
}

bool GuestMemory::IsMapped(u32 vaddr, u32 size) const {
    const std::lock_guard lock(mutex_);
    return IsMappedLocked(vaddr, size);
}

void GuestMemory::SetLinearHeapArea(u32 vaddr) {
    const std::lock_guard lock(mutex_);
    linear_heap_area_ = vaddr;
}

u32 GuestMemory::LinearHeapArea() const {
    const std::lock_guard lock(mutex_);
    return linear_heap_area_;
}

u8* GuestMemory::PhysicalPointer(u32 paddr, u32 size) const {
    size = std::max(size, 1u);
    const auto fits = [paddr, size](u32 start, u32 length) {
        return paddr >= start && u64{paddr} + size <= u64{start} + length;
    };
    if (fits(kVramPaddr, kVramSize)) {
        return base_ + kVramVaddr + (paddr - kVramPaddr);
    }
    if (fits(kDspRamPaddr, kDspRamSize)) {
        return base_ + kDspRamVaddr + (paddr - kDspRamPaddr);
    }
    const std::lock_guard lock(mutex_);
    if (linear_heap_area_ == 0 || !fits(kFcramPaddr, kFcramSize)) {
        return nullptr;
    }
    const u64 vaddr = u64{linear_heap_area_} + (paddr - kFcramPaddr);
    if (vaddr >= kManagedEnd || !IsMappedLocked(static_cast<u32>(vaddr), size)) {
        return nullptr;
    }
    return base_ + vaddr;
}

std::optional<u32> GuestMemory::VirtualToPhysical(u32 vaddr) const {
    struct Window {
        u32 vaddr;
        u32 paddr;
        u32 size;
    };
    constexpr Window windows[]{
        {kVramVaddr, kVramPaddr, kVramSize},
        {kDspRamVaddr, kDspRamPaddr, kDspRamSize},
        {kLinearHeapVaddr, kFcramPaddr, kFcramSize},
        {kNewLinearHeapVaddr, kFcramPaddr, kFcramSizeNew3ds},
    };
    for (const Window& window : windows) {
        if (vaddr >= window.vaddr && vaddr - window.vaddr <= window.size) {
            return window.paddr + (vaddr - window.vaddr);
        }
    }
    return std::nullopt;
}

bool GuestMemory::ReadBlock(void* dst, u32 vaddr, u32 size) const {
    const std::lock_guard lock(mutex_);
    if (!IsMappedLocked(vaddr, size)) {
        return false;
    }
    if (size != 0) {
        std::memmove(dst, base_ + vaddr, size);
    }
    return true;
}

bool GuestMemory::WriteBlock(u32 vaddr, const void* src, u32 size) {
    const std::lock_guard lock(mutex_);
    if (!IsMappedLocked(vaddr, size)) {
        return false;
    }
    if (size != 0) {
        std::memmove(base_ + vaddr, src, size);
    }
    return true;
}

std::optional<std::string> GuestMemory::ReadString(u32 vaddr, u32 max_length) const {
    const std::lock_guard lock(mutex_);
    std::string value;
    while (value.size() < max_length) {
        if (vaddr >= kManagedEnd) {
            return std::nullopt;
        }
        const auto it = FindBlock(vaddr);
        if (!it->second.mapped) {
            return std::nullopt;
        }
        const auto* begin = reinterpret_cast<const char*>(base_ + vaddr);
        const std::size_t count = std::min<std::size_t>(it->first + it->second.size - vaddr,
                                                      max_length - value.size());
        const auto* nul = static_cast<const char*>(std::memchr(begin, 0, count));
        if (nul != nullptr) {
            value.append(begin, nul);
            break;
        }
        value.append(begin, count);
        vaddr += static_cast<u32>(count);
    }
    return value;
}

} // namespace mh4n
