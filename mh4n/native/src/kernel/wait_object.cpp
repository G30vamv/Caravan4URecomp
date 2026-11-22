// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the comments of mh4n/kernel.h (WaitObject, Event, Semaphore, Thread's wait API), 3dbrew
// "SVC" (WaitSynchronization1/N, SignalEvent, ClearEvent, ReleaseSemaphore) and the black-box specification of
// tests/kernel_objects_test.cpp.
#include <algorithm>

#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {

// ---- WaitObject ------------------------------------------------------------------------------------------

void WaitObject::AddWaitingThread(std::shared_ptr<Thread> thread) {
    // an object listed twice in one wait has one entry in the waiter list
    const auto it = std::find(waiting_.begin(), waiting_.end(), thread);
    if (it == waiting_.end()) {
        waiting_.push_back(std::move(thread));
    }
}

void WaitObject::RemoveWaitingThread(Thread* thread) {
    const auto it = std::find_if(waiting_.begin(), waiting_.end(),
                                 [thread](const std::shared_ptr<Thread>& t) { return t.get() == thread; });
    if (it != waiting_.end()) {
        waiting_.erase(it);
    }
}

std::shared_ptr<Thread> WaitObject::GetHighestPriorityReadyThread() const {
    std::shared_ptr<Thread> candidate;
    u32 candidate_priority = kThreadPrioLowest + 1;
    for (const std::shared_ptr<Thread>& thread : waiting_) {
        MH4N_ASSERT(thread->status == ThreadStatus::WaitSynchAny || thread->status == ThreadStatus::WaitSynchAll ||
                        thread->status == ThreadStatus::WaitHleEvent,
                    "a waiter list holds a thread that is not waiting (status {})", static_cast<int>(thread->status));
        if (thread->status == ThreadStatus::WaitSynchAll) {
            // a wait on all can only end when every object it waits for is available
            const bool all_available = std::none_of(
                thread->wait_objects.begin(), thread->wait_objects.end(),
                [&thread](const std::shared_ptr<WaitObject>& object) { return object->ShouldWait(thread.get()); });
            if (!all_available) {
                continue;
            }
        }
        // strictly better only: among equals the first in the list stays the candidate
        if (thread->current_priority >= candidate_priority) {
            continue;
        }
        if (ShouldWait(thread.get())) {
            continue;
        }
        candidate = thread;
        candidate_priority = thread->current_priority;
    }
    return candidate;
}

void WaitObject::WakeupAllWaitingThreads() {
    while (std::shared_ptr<Thread> thread = GetHighestPriorityReadyThread()) {
        const bool wait_all = thread->IsSleepingOnWaitAll();
        if (!wait_all) {
            Acquire(thread.get());
        } else {
            for (const std::shared_ptr<WaitObject>& object : thread->wait_objects) {
                object->Acquire(thread.get());
            }
        }
        // what the blocked call returns: success and, for a wait on any, the position of this object (a wait on
        // all has no index)
        std::optional<Result> result = kResultSuccess;
        s32 index = wait_all ? -1 : thread->GetWaitObjectIndex(this);
        if (thread->wake_hook) {
            thread->wake_hook(ThreadWakeupReason::Signal, this, result, index);
        }
        // the thread leaves every list it was on, so finished waits leave no cycles
        for (const std::shared_ptr<WaitObject>& object : thread->wait_objects) {
            object->RemoveWaitingThread(thread.get());
        }
        thread->wait_objects.clear();
        thread->ResumeFromWait(result, index);
    }
    if (hle_notifier_) {
        hle_notifier_();
    }
}

// ---- Event -----------------------------------------------------------------------------------------------

Event::Event(Kernel& kernel, ResetType reset_type) : WaitObject(kernel), reset_type_(reset_type) {}

Event::~Event() {
    if (resource_limit != nullptr) {
        resource_limit->Release(ResourceLimitType::Event, 1);
    }
}

bool Event::ShouldWait(const Thread*) const {
    return !signaled_;
}

void Event::Acquire(Thread* thread) {
    MH4N_ASSERT(!ShouldWait(thread), "Event::Acquire of a thread that has to wait");
    if (reset_type_ == ResetType::OneShot) {
        signaled_ = false;
    }
}

void Event::Signal() {
    signaled_ = true;
    WakeupAllWaitingThreads();
}

void Event::Clear() {
    signaled_ = false;
}

void Event::WakeupAllWaitingThreads() {
    WaitObject::WakeupAllWaitingThreads();
    if (reset_type_ == ResetType::Pulse) {
        signaled_ = false;
    }
}

// ---- Semaphore -------------------------------------------------------------------------------------------

Semaphore::Semaphore(Kernel& kernel, s32 initial_count, s32 max_count)
    : WaitObject(kernel), max_count_(max_count), available_count_(initial_count) {}

Semaphore::~Semaphore() {
    if (resource_limit != nullptr) {
        resource_limit->Release(ResourceLimitType::Semaphore, 1);
    }
}

bool Semaphore::ShouldWait(const Thread*) const {
    return available_count_ <= 0;
}

void Semaphore::Acquire(Thread* thread) {
    MH4N_ASSERT(!ShouldWait(thread), "Semaphore::Acquire of a thread that has to wait");
    --available_count_;
}

Result Semaphore::Release(s32* out_count, s32 release_count) {
    if (max_count_ - available_count_ < release_count) {
        return kResultOutOfRangeKernel;
    }
    *out_count = available_count_;
    available_count_ += release_count;
    WakeupAllWaitingThreads();
    return kResultSuccess;
}

}  // namespace mh4n
