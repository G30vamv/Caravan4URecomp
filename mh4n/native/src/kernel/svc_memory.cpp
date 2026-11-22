// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: kernel and memory contracts, svc_test.cpp, memory_test.cpp, and spec/kernel_test_vectors.tsv.

#include "svc_internal.h"

#include <algorithm>
#include <utility>

#include "mh4n/assert.h"
#include "mh4n/log.h"
#include "../cpu/recomp_c.h"

namespace mh4n {
namespace {

constexpr char kLog[] = "Kernel.SVC";

enum class MemoryOperation : u32 {
    Free = 1,
    Commit = 3,
    Map = 4,
    Unmap = 5,
    Protect = 6,
};

constexpr u32 kMemoryOperationMask = 0xFF;
constexpr u32 kLinearFlag = 0x10000;
constexpr u32 kReadWrite = static_cast<u32>(MemPerm::ReadWrite);
constexpr u32 kDontCare = static_cast<u32>(MemoryPermission::DontCare);

bool InRange(u32 address, u32 size, u32 begin, u32 end) {
    return address >= begin && address < end && size <= end - address;
}

bool HasState(const GuestMemory& memory, u32 address, u32 size, MemState state) {
    const u32 end = address + size;
    while (address < end) {
        const auto block = memory.Query(address);
        if (!block || block->state != state) {
            return false;
        }
        address = block->base + block->size;
    }
    return true;
}

bool ValidBlockPermission(u32 permission) {
    return permission <= kReadWrite || permission == kDontCare;
}

void AccountMemory(Process& process, u32 size, bool allocate) {
    if (allocate) {
        process.memory_used += size;
        process.resource_limit->Reserve(ResourceLimitType::Commit, static_cast<s32>(size));
    } else {
        process.memory_used -= size;
        process.resource_limit->Release(ResourceLimitType::Commit, static_cast<s32>(size));
    }
}

}  // namespace

void SvcTable::Impl::ControlMemory(SvcFrame& f) {
    auto& ctx = f.ctx;
    u32 address = 0;
    ctx.r[0] = ControlMemoryOp(ctx.r[0], ctx.r[1], ctx.r[2], ctx.r[3], ctx.r[4], &address);
    ctx.r[1] = address;
}

Result SvcTable::Impl::ControlMemoryOp(u32 operation, u32 addr0, u32 addr1, u32 size,
                                       u32 permissions, u32* out_addr) {
    if (((addr0 | addr1) & kPageMask) != 0) {
        return kResultMisalignedAddress;
    }
    if ((size & kPageMask) != 0) {
        return kResultMisalignedSize;
    }
    if (permissions > kReadWrite) {
        return kResultInvalidCombination;
    }

    const auto perm = static_cast<MemPerm>(permissions);
    const auto op = static_cast<MemoryOperation>(operation & kMemoryOperationMask);
    switch (op) {
    case MemoryOperation::Free: {
        const Result result = addr0 >= LinearHeapArea() ? LinearFree(addr0, size) : HeapFree(addr0, size);
        if (result == kResultSuccess) {
            *out_addr = addr0;
        }
        return result;
    }
    case MemoryOperation::Commit:
        if ((operation & kLinearFlag) != 0) {
            return LinearAllocate(out_addr, addr0, size, perm);
        }
        return HeapAllocate(out_addr, addr0, size, perm);
    case MemoryOperation::Map:
    case MemoryOperation::Unmap:
        if (!InRange(addr0, size, kHeapVaddr, kHeapVaddrEnd) ||
            !InRange(addr1, size, kHeapVaddr, kHeapVaddrEnd)) {
            return kResultInvalidAddress;
        }
        if (addr0 < addr1 + size && addr1 < addr0 + size) {
            return kResultInvalidAddressState;
        }
        if (op == MemoryOperation::Map &&
            (!HasState(memory, addr0, size, MemState::Free) ||
             !HasState(memory, addr1, size, MemState::Private))) {
            return kResultInvalidAddressState;
        }
        if (!map_unmap_logged) {
            map_unmap_logged = true;
            MH4N_LOG(Warning, kLog, "ControlMemory alias mappings are unsupported");
        }
        return kResultInvalidAddressState;
    case MemoryOperation::Protect:
        return memory.Protect(addr0, size, perm);
    default:
        return kResultInvalidCombination;
    }
}

Result SvcTable::Impl::HeapAllocate(u32* out_addr, u32 target, u32 size, MemPerm perm) {
    if (!InRange(target, size, kHeapVaddr, kHeapVaddrEnd)) {
        return kResultInvalidAddress;
    }
    if (!HasState(memory, target, size, MemState::Free)) {
        return kResultInvalidAddressState;
    }
    auto& process = *kernel.CurrentProcess();
    auto& regions = kernel.Regions();
    const auto pieces = regions.HeapAllocate(process.memory_region, size);
    if (pieces.empty()) {
        return kResultOutOfHeapMemory;
    }

    u32 mapped_end = target;
    for (const auto& piece : pieces) {
        const Result result = memory.Map(mapped_end, piece.size, perm, MemState::Private, piece.offset);
        if (result != kResultSuccess) {
            if (mapped_end != target) {
                MH4N_ASSERT(memory.Unmap(target, mapped_end - target) == kResultSuccess);
            }
            for (const auto& reserved : pieces) {
                MH4N_ASSERT(regions.Free(process.memory_region, reserved.offset, reserved.size));
            }
            return result;
        }
        mapped_end += piece.size;
    }
    u32 address = target;
    for (const auto& piece : pieces) {
        heap_runs.emplace(address, HeapRun{piece.size, piece.offset});
        address += piece.size;
    }
    AccountMemory(process, size, true);
    *out_addr = target;
    return kResultSuccess;
}

Result SvcTable::Impl::HeapFree(u32 target, u32 size) {
    if (!InRange(target, size, kHeapVaddr, kHeapVaddrEnd)) {
        return kResultInvalidAddress;
    }
    if (size == 0) {
        return kResultSuccess;
    }
    if (!HasState(memory, target, size, MemState::Private)) {
        return kResultInvalidAddressState;
    }

    const u32 end = target + size;
    auto first = heap_runs.upper_bound(target);
    if (first == heap_runs.begin()) {
        return kResultInvalidAddressState;
    }
    --first;
    auto last = first;
    for (u32 address = target; address < end; ++last) {
        if (last == heap_runs.end() || address < last->first ||
            address >= last->first + last->second.size) {
            return kResultInvalidAddressState;
        }
        address = last->first + last->second.size;
    }

    const Result result = memory.Unmap(target, size);
    if (result != kResultSuccess) {
        return result;
    }
    auto& process = *kernel.CurrentProcess();
    auto& regions = kernel.Regions();
    for (auto run = first; run != last;) {
        const auto [base, piece] = *run;
        run = heap_runs.erase(run);
        const u32 limit = base + piece.size;
        const u32 freed_begin = std::max(base, target);
        const u32 freed_end = std::min(limit, end);
        const u32 offset = piece.fcram_offset + (freed_begin - base);
        MH4N_ASSERT(regions.Free(process.memory_region, offset, freed_end - freed_begin));
        if (base < freed_begin) {
            heap_runs.emplace(base, HeapRun{freed_begin - base, piece.fcram_offset});
        }
        if (freed_end < limit) {
            const u32 remaining_offset = piece.fcram_offset + (freed_end - base);
            heap_runs.emplace(freed_end, HeapRun{limit - freed_end, remaining_offset});
        }
    }
    AccountMemory(process, size, false);
    return kResultSuccess;
}

u32 SvcTable::Impl::LinearHeapArea() const {
    return LinearHeapAreaForKernelVersion(kernel.CurrentProcess()->kernel_version);
}

Result SvcTable::Impl::LinearAllocate(u32* out_addr, u32 target, u32 size, MemPerm perm) {
    auto& process = *kernel.CurrentProcess();
    const auto region = process.memory_region;
    auto& regions = kernel.Regions();
    const u32 area = LinearHeapArea();
    const u32 begin = area + regions.Base(region);
    const u32 end = begin + regions.Size(region);
    const bool fixed = target != 0;
    if (fixed && !InRange(target, size, begin, end)) {
        return kResultInvalidAddress;
    }

    u32 offset;
    if (fixed) {
        offset = target - area;
        if (!regions.LinearAllocate(region, offset, size)) {
            return kResultInvalidAddressState;
        }
    } else {
        const auto allocated = regions.LinearAllocate(region, size);
        if (!allocated) {
            return kResultOutOfHeapMemory;
        }
        offset = *allocated;
        target = area + offset;
    }
    const Result result = memory.Map(target, size, perm, MemState::Continuous, offset);
    if (result != kResultSuccess) {
        MH4N_ASSERT(regions.Free(region, offset, size));
        return result;
    }
    AccountMemory(process, size, true);
    *out_addr = target;
    return kResultSuccess;
}

Result SvcTable::Impl::LinearFree(u32 target, u32 size) {
    auto& process = *kernel.CurrentProcess();
    const auto region = process.memory_region;
    auto& regions = kernel.Regions();
    const u32 area = LinearHeapArea();
    const u32 begin = area + regions.Base(region);
    if (!InRange(target, size, begin, begin + regions.Size(region))) {
        return kResultInvalidAddress;
    }
    if (size == 0) {
        return kResultSuccess;
    }
    if (!HasState(memory, target, size, MemState::Continuous)) {
        return kResultInvalidAddressState;
    }
    const Result result = memory.Unmap(target, size);
    if (result != kResultSuccess) {
        return result;
    }
    MH4N_ASSERT(regions.Free(region, target - area, size));
    AccountMemory(process, size, false);
    return kResultSuccess;
}

void SvcTable::Impl::QueryMemory(SvcFrame& f) {
    auto& ctx = f.ctx;
    const auto block = memory.Query(ctx.r[2]);
    if (!block) {
        ctx.r[0] = kResultInvalidAddress;
        std::fill_n(ctx.r + 1, 5, 0u);
        return;
    }

    u32 base = block->base;
    u32 end = base + block->size;
    while (base != 0) {
        const auto prev = memory.Query(base - 1);
        if (!prev || prev->perm != block->perm || prev->state != block->state) {
            break;
        }
        base = prev->base;
    }
    while (end != kManagedEnd) {
        const auto next = memory.Query(end);
        if (!next || next->perm != block->perm || next->state != block->state) {
            break;
        }
        end += next->size;
    }
    ctx.r[0] = kResultSuccess;
    ctx.r[1] = base;
    ctx.r[2] = end - base;
    ctx.r[3] = static_cast<u32>(block->perm);
    ctx.r[4] = static_cast<u32>(block->state);
    ctx.r[5] = 0;
}

void SvcTable::Impl::CreateMemoryBlock(SvcFrame& f) {
    auto& ctx = f.ctx;
    const u32 address = ctx.r[1];
    const u32 size = ctx.r[2];
    const u32 permissions = ctx.r[3];
    const u32 other = ctx.r[0];
    ctx.r[1] = 0;
    if ((size & kPageMask) != 0) {
        ctx.r[0] = kResultMisalignedSize;
        return;
    }
    if (!ValidBlockPermission(permissions) || !ValidBlockPermission(other)) {
        ctx.r[0] = kResultInvalidCombination;
        return;
    }
    if (address != 0 && !InRange(address, size, kProcessImageVaddr, kLinearHeapVaddr)) {
        ctx.r[0] = kResultInvalidAddress;
        return;
    }
    const auto& process = kernel.CurrentProcess();
    if (!process->resource_limit->Reserve(ResourceLimitType::SharedMemory, 1)) {
        ctx.r[0] = kResultOutOfSharedMems;
        return;
    }
    std::shared_ptr<SharedMemory> block;
    const Result result = kernel.NewSharedMemory(&block, process, size,
                                                 static_cast<MemoryPermission>(permissions),
                                                 static_cast<MemoryPermission>(other),
                                                 address, FcramRegion::Base);
    if (result != kResultSuccess) {
        ctx.r[0] = result;
        return;
    }
    Handle handle = 0;
    ctx.r[0] = kernel.AddHandle(&handle, std::move(block));
    ctx.r[1] = handle;
}

void SvcTable::Impl::MapMemoryBlock(SvcFrame& f) {
    auto& ctx = f.ctx;
    const auto block = kernel.GetObject<SharedMemory>(ctx.r[0]);
    if (!block) {
        ctx.r[0] = kResultInvalidHandle;
        return;
    }
    if (ctx.r[2] == 0 || ctx.r[2] > kReadWrite || !ValidBlockPermission(ctx.r[3])) {
        ctx.r[0] = kResultInvalidCombination;
        return;
    }
    ctx.r[0] = block->Map(*kernel.CurrentProcess(), ctx.r[1], static_cast<MemoryPermission>(ctx.r[2]),
                          static_cast<MemoryPermission>(ctx.r[3]));
}

void SvcTable::Impl::UnmapMemoryBlock(SvcFrame& f) {
    auto& ctx = f.ctx;
    const auto block = kernel.GetObject<SharedMemory>(ctx.r[0]);
    ctx.r[0] = block ? block->Unmap(*kernel.CurrentProcess(), ctx.r[1]) : kResultInvalidHandle;
}

void SvcTable::Impl::ControlProcessMemory(SvcFrame& f) {
    auto& ctx = f.ctx;
    if (!kernel.GetObject<Process>(ctx.r[0])) {
        ctx.r[0] = kResultInvalidHandle;
    } else if (((ctx.r[1] | ctx.r[2]) & kPageMask) != 0) {
        ctx.r[0] = kResultMisalignedAddress;
    } else if ((ctx.r[3] & kPageMask) != 0) {
        ctx.r[0] = kResultMisalignedSize;
    } else if (ctx.r[4] != static_cast<u32>(MemoryOperation::Protect) ||
               ctx.r[5] > static_cast<u32>(MemPerm::ReadWriteExecute)) {
        ctx.r[0] = kResultInvalidCombination;
    } else {
        ctx.r[0] = memory.Protect(ctx.r[1], ctx.r[3], static_cast<MemPerm>(ctx.r[5]));
    }
}

}  // namespace mh4n
