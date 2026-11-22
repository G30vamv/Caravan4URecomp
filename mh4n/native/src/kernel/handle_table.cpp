// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the comments of mh4n/kernel.h (the HandleTable rules), 3dbrew "Handles" and the black-box
// specification of tests/kernel_objects_test.cpp.
#include "mh4n/kernel.h"

namespace mh4n {

namespace {

constexpr u32 kSlotShift = 15;
constexpr u32 kGenerationMask = (1u << kSlotShift) - 1;  // 0x7FFF

}  // namespace

HandleTable::HandleTable(Kernel& kernel) : kernel_(kernel) {
    Clear();
}

HandleTable::~HandleTable() {
    Clear();
}

void HandleTable::Clear() {
    for (auto& object : objects_) {
        object.reset();
    }
    // empty slots form a singly linked free list through generations_: slot i points to slot i + 1
    for (std::size_t i = 0; i < kMaxCount; ++i) {
        generations_[i] = static_cast<u16>(i + 1);
    }
    next_free_slot_ = 0;
}

Result HandleTable::Create(Handle* out, std::shared_ptr<Object> object) {
    const u16 slot = next_free_slot_;
    if (slot >= kMaxCount) {
        return kResultOutOfHandles;
    }
    next_free_slot_ = generations_[slot];

    const u16 generation = next_generation_;
    next_generation_ = static_cast<u16>(next_generation_ + 1);
    if (next_generation_ >= (1u << kSlotShift)) {
        next_generation_ = 1;
    }

    objects_[slot] = std::move(object);
    generations_[slot] = generation;
    *out = static_cast<Handle>(generation) | (static_cast<Handle>(slot) << kSlotShift);
    return kResultSuccess;
}

Result HandleTable::Duplicate(Handle* out, Handle handle) {
    std::shared_ptr<Object> object = GetGeneric(handle);
    if (object == nullptr) {
        return kResultInvalidHandle;
    }
    return Create(out, std::move(object));
}

Result HandleTable::Close(Handle handle) {
    if (!IsValid(handle)) {
        return kResultInvalidHandle;
    }
    const u32 slot = handle >> kSlotShift;
    // the slot joins the free list at its head: the last slot freed is the first one reused
    generations_[slot] = next_free_slot_;
    next_free_slot_ = static_cast<u16>(slot);
    // (the object may be the last reference: take it out of the table before it is destroyed)
    std::shared_ptr<Object> released = std::move(objects_[slot]);
    objects_[slot].reset();
    return kResultSuccess;
}

bool HandleTable::IsValid(Handle handle) const {
    const u32 slot = handle >> kSlotShift;
    const u32 generation = handle & kGenerationMask;
    if (slot >= kMaxCount) {
        return false;  // also the pseudo handles
    }
    return objects_[slot] != nullptr && generations_[slot] == generation;
}

std::shared_ptr<Object> HandleTable::GetGeneric(Handle handle) const {
    if (handle == kHandleCurrentThread) {
        Thread* thread = kernel_.CurrentThread();
        return thread != nullptr ? std::static_pointer_cast<Object>(thread->shared_from_this()) : nullptr;
    }
    if (handle == kHandleCurrentProcess) {
        return kernel_.CurrentProcess();
    }
    if (!IsValid(handle)) {
        return nullptr;
    }
    return objects_[handle >> kSlotShift];
}

const Object* HandleTable::PeekGeneric(Handle handle) const {
    if (handle == kHandleCurrentThread) {
        return kernel_.CurrentThread();
    }
    if (handle == kHandleCurrentProcess) {
        return kernel_.CurrentProcess().get();
    }
    if (!IsValid(handle)) {
        return nullptr;
    }
    return objects_[handle >> kSlotShift].get();
}

}  // namespace mh4n
