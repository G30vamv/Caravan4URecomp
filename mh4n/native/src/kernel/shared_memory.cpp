// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/kernel.h, mh4n/memory.h, kernel_objects_test.cpp,
// svc_test.cpp, and the HID mapping vector in spec/kernel_test_vectors.tsv.
#include "mh4n/kernel.h"

#include "mh4n/assert.h"
#include "mh4n/log.h"

namespace mh4n {

namespace {

constexpr auto kLog = "Kernel";

bool Includes(MemoryPermission allowed, MemoryPermission requested) {
    return (static_cast<u32>(requested) & ~static_cast<u32>(allowed)) == 0;
}

MemPerm ToGuestPermission(MemoryPermission permission) {
    return static_cast<MemPerm>(static_cast<u32>(permission) & 7u);
}

}  // namespace

SharedMemory::SharedMemory(Kernel& kernel) : Object(kernel) {}

SharedMemory::~SharedMemory() {
    if (base_address_ != 0 && pieces_.empty()) {
        kernel_.Memory().ChangeState(base_address_, size_, MemState::Locked,
                                     ToGuestPermission(permissions_), MemState::Private,
                                     MemPerm::ReadWrite);
    }
    // Closing the block returns its allocation. Unmap is a separate operation.
    for (const Piece& piece : pieces_) {
        MH4N_ASSERT(kernel_.Regions().Free(region_, piece.offset, piece.size));
    }
    if (auto owner = owner_process_.lock()) {
        if (base_address_ == 0 && !pieces_.empty()) {
            owner->memory_used -= size_;
        }
        if (owner->resource_limit) {
            owner->resource_limit->Release(ResourceLimitType::SharedMemory, 1);
        }
    }
}

Result SharedMemory::Map(Process& target, u32 address, MemoryPermission permissions,
                         MemoryPermission other_permissions) {
    const bool automatic = base_address_ == 0;
    const bool dont_care = other_permissions == MemoryPermission::DontCare;
    if (automatic && !dont_care) {
        return kResultInvalidCombination;
    }
    const auto allowed = owner_process_.lock().get() == &target ? permissions_ : other_permissions_;
    if (!Includes(allowed, permissions) || (!automatic && dont_care)) {
        return kResultInvalidCombination;
    }
    if (!dont_care && !Includes(other_permissions, permissions_)) {
        return kResultWrongPermission;
    }

    if (address == 0) {
        if (automatic) {
            address = kLinearHeapVaddr + linear_heap_phys_offset_;
        }
    } else if (address < kHeapVaddr || static_cast<u64>(address) + size_ >= kLinearHeapVaddr) {
        return kResultInvalidAddress;
    }
    if (mapped_ || pieces_.empty()) {
        MH4N_LOG(Warning, kLog, "Shared memory '{}' cannot be aliased at {:#010x}", Name(), address);
        return kResultInvalidAddressState;
    }

    auto& memory = kernel_.Memory();
    const auto area = memory.Query(address);
    if (!area || area->state != MemState::Free ||
        static_cast<u64>(address) + size_ > static_cast<u64>(area->base) + area->size) {
        return kResultInvalidAddressState;
    }

    u32 cursor = address;
    for (const Piece& piece : pieces_) {
        const Result result = memory.Map(cursor, piece.size, ToGuestPermission(permissions),
                                         MemState::Shared, kFcramPaddr + piece.offset);
        if (result != kResultSuccess) {
            if (cursor != address) {
                MH4N_ASSERT(memory.Unmap(address, cursor - address) == kResultSuccess);
            }
            return result;
        }
        cursor += piece.size;
    }
    MH4N_ASSERT(memory.WriteBlock(address, host_data_.data(), size_));
    mapped_at_ = address;
    mapped_ = true;
    return kResultSuccess;
}

Result SharedMemory::Unmap(Process&, u32 address) {
    if (!mapped_ || address != mapped_at_) {
        return kResultInvalidAddressState;
    }

    auto& memory = kernel_.Memory();
    if (!memory.ReadBlock(host_data_.data(), address, size_)) {
        return kResultInvalidAddressState;
    }
    const Result result = memory.Unmap(address, size_);
    if (result == kResultSuccess) {
        mapped_ = false;
        mapped_at_ = 0;
    }
    return result;
}

u8* SharedMemory::GetPointer(u32 offset) {
    if (mapped_) {
        return kernel_.Memory().Base() + mapped_at_ + offset;
    }
    if (base_address_ != 0 && pieces_.empty()) {
        return kernel_.Memory().Base() + base_address_ + offset;
    }
    return host_data_.data() + offset;
}

}  // namespace mh4n
