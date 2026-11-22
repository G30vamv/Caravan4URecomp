// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/kernel.h, mh4n/memory.h, kernel_objects_test.cpp,
// svc_test.cpp, and the process and resource limit vectors in spec/kernel_test_vectors.tsv.
#include "mh4n/kernel.h"

#include "mh4n/assert.h"
#include "mh4n/log.h"

namespace mh4n {

namespace {

constexpr auto kLog = "Kernel";

std::size_t LimitIndex(ResourceLimitType type) {
    const auto index = static_cast<std::size_t>(type);
    MH4N_ASSERT(index < static_cast<std::size_t>(ResourceLimitType::Max));
    return index;
}

}  // namespace

ResourceLimit::ResourceLimit(Kernel& kernel, ResourceLimitCategory category)
    : Object(kernel), category_(category) {}

s32 ResourceLimit::GetCurrentValue(ResourceLimitType type) const {
    return current_values_[LimitIndex(type)];
}

s32 ResourceLimit::GetLimitValue(ResourceLimitType type) const {
    return limit_values_[LimitIndex(type)];
}

void ResourceLimit::SetCurrentValue(ResourceLimitType type, s32 value) {
    current_values_[LimitIndex(type)] = value;
}

void ResourceLimit::SetLimitValue(ResourceLimitType type, s32 value) {
    limit_values_[LimitIndex(type)] = value;
}

bool ResourceLimit::Reserve(ResourceLimitType type, s32 amount) {
    const s64 total = static_cast<s64>(GetCurrentValue(type)) + amount;
    if (total > GetLimitValue(type)) {
        MH4N_LOG(Error, kLog, "Resource {} usage {} exceeds limit {}",
                 static_cast<u32>(type), total, GetLimitValue(type));
    }
    SetCurrentValue(type, static_cast<s32>(total));
    return true;
}

bool ResourceLimit::Release(ResourceLimitType type, s32 amount) {
    const s64 total = static_cast<s64>(GetCurrentValue(type)) - amount;
    if (total < 0) {
        MH4N_LOG(Error, kLog, "Resource {} usage would fall to {}",
                 static_cast<u32>(type), total);
    }
    SetCurrentValue(type, static_cast<s32>(total));
    return true;
}

Process::Process(Kernel& kernel, u32 id) : Object(kernel), handle_table(kernel), process_id(id) {}

Process::~Process() {
    handle_table.Clear();
}

Kernel::Kernel(GuestMemory& memory, u32 memory_mode)
    : memory_(memory), regions_(memory_mode),
      application_limit_(std::make_shared<ResourceLimit>(*this, ResourceLimitCategory::Application)) {
    auto& limit = *application_limit_;
    limit.SetLimitValue(ResourceLimitType::Priority, kThreadPrioUserlandMax);
    limit.SetLimitValue(ResourceLimitType::Commit,
                        static_cast<s32>(regions_.Size(FcramRegion::Application)));
    limit.SetLimitValue(ResourceLimitType::Thread, 0x20);
    limit.SetLimitValue(ResourceLimitType::Event, 0x20);
    limit.SetLimitValue(ResourceLimitType::Mutex, 0x20);
    limit.SetLimitValue(ResourceLimitType::Semaphore, 0x8);
    limit.SetLimitValue(ResourceLimitType::Timer, 0x8);
    limit.SetLimitValue(ResourceLimitType::SharedMemory, 0x10);
    limit.SetLimitValue(ResourceLimitType::AddressArbiter, 0x2);
}

Kernel::~Kernel() {
    if (process_) {
        process_->handle_table.Clear();
    }
}

Scheduler& Kernel::GetScheduler() {
    MH4N_ASSERT(scheduler_ != nullptr);
    return *scheduler_;
}

GameClock& Kernel::Clock() {
    MH4N_ASSERT(clock_ != nullptr);
    return *clock_;
}

std::shared_ptr<Event> Kernel::NewEvent(ResetType reset_type, std::string name) {
    auto event = std::make_shared<Event>(*this, reset_type);
    event->SetName(std::move(name));
    return event;
}

std::shared_ptr<Mutex> Kernel::NewMutex(bool initial_locked, std::string name) {
    auto mutex = std::make_shared<Mutex>(*this);
    mutex->SetName(std::move(name));
    if (initial_locked) {
        mutex->Acquire(CurrentThread());
    }
    return mutex;
}

Result Kernel::NewSemaphore(std::shared_ptr<Semaphore>* out, s32 initial_count, s32 max_count,
                            std::string name) {
    if (initial_count > max_count) {
        return kResultInvalidCombinationKernel;
    }
    auto semaphore = std::make_shared<Semaphore>(*this, initial_count, max_count);
    semaphore->SetName(std::move(name));
    *out = std::move(semaphore);
    return kResultSuccess;
}

std::shared_ptr<Timer> Kernel::NewTimer(ResetType reset_type, std::string name) {
    auto timer = std::make_shared<Timer>(*this, reset_type);
    timer->SetName(std::move(name));
    return timer;
}

std::shared_ptr<AddressArbiter> Kernel::NewAddressArbiter(std::string name) {
    auto arbiter = std::make_shared<AddressArbiter>(*this);
    arbiter->SetName(std::move(name));
    return arbiter;
}

Result Kernel::NewSharedMemory(std::shared_ptr<SharedMemory>* out, std::shared_ptr<Process> owner,
                               u32 size, MemoryPermission permissions,
                               MemoryPermission other_permissions, u32 address, FcramRegion region,
                               std::string name) {
    auto block = std::make_shared<SharedMemory>(*this);
    block->SetName(std::move(name));
    block->owner_process_ = owner;
    block->region_ = region;
    block->size_ = size;
    block->permissions_ = permissions;
    block->other_permissions_ = other_permissions;
    if (address != 0) {
        const auto perm = static_cast<MemPerm>(static_cast<u32>(permissions) & 7u);
        const Result result = memory_.ChangeState(address, size, MemState::Private,
                                                   MemPerm::ReadWrite, MemState::Locked, perm);
        if (result != kResultSuccess) {
            return result;
        }
        block->base_address_ = address;
    } else {
        block->pieces_.reserve(1);
        const auto offset = regions_.LinearAllocate(region, size);
        if (!offset) {
            return kResultOutOfMemory;
        }
        block->pieces_.push_back({*offset, size});
        block->linear_heap_phys_offset_ = *offset;
        if (owner) {
            owner->memory_used += size;
        }
        block->host_data_.resize(size);
    }
    *out = std::move(block);
    return kResultSuccess;
}

std::shared_ptr<SharedMemory> Kernel::NewSharedMemoryForApplet(u32 offset, u32 size,
                                                              MemoryPermission permissions,
                                                              MemoryPermission other_permissions,
                                                              std::string name) {
    auto block = std::make_shared<SharedMemory>(*this);
    block->SetName(std::move(name));
    block->region_ = FcramRegion::System;
    block->size_ = size;
    block->permissions_ = permissions;
    block->other_permissions_ = other_permissions;
    const auto ranges = regions_.HeapAllocate(block->region_, size);
    if (ranges.empty()) {
        return nullptr;
    }
    try {
        block->pieces_.reserve(ranges.size());
    } catch (...) {
        for (const auto& range : ranges) {
            regions_.Free(block->region_, range.offset, range.size);
        }
        throw;
    }
    for (const auto& range : ranges) {
        block->pieces_.push_back({range.offset, range.size});
    }
    block->base_address_ = kHeapVaddr + offset;
    block->host_data_.resize(size);
    return block;
}

std::shared_ptr<Thread> Kernel::NewThread(std::string name, u32 priority, s32 processor_id,
                                         std::shared_ptr<Process> owner) {
    auto thread = std::make_shared<Thread>(*this, next_thread_id_++, priority, processor_id);
    thread->SetName(std::move(name));
    thread->owner_process = owner ? owner : process_;
    return thread;
}

std::shared_ptr<Process> Kernel::NewProcess(std::string name) {
    auto process = std::make_shared<Process>(*this, ++next_process_id_);
    process->SetName(std::move(name));
    process->resource_limit = application_limit_;
    process_ = process;
    return process;
}

Result Kernel::AddHandle(Handle* out, std::shared_ptr<Object> object) {
    if (!process_) {
        return kResultInvalidHandle;
    }
    return process_->handle_table.Create(out, std::move(object));
}

}  // namespace mh4n
