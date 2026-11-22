// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the comments of mh4n/kernel.h (Mutex, Thread::held_mutexes/pending_mutexes), 3dbrew "Mutex"
// (priority inheritance, recursion) and the black-box specification of tests/kernel_objects_test.cpp.
#include <algorithm>

#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {

Mutex::Mutex(Kernel& kernel) : WaitObject(kernel) {}

Mutex::~Mutex() {
    if (resource_limit != nullptr) {
        resource_limit->Release(ResourceLimitType::Mutex, 1);
    }
}

bool Mutex::ShouldWait(const Thread* thread) const {
    // free, or held by the thread already (recursion)
    return lock_count_ > 0 && thread != holding_thread_;
}

void Mutex::Acquire(Thread* thread) {
    MH4N_ASSERT(!ShouldWait(thread), "Mutex::Acquire of a thread that has to wait");
    if (lock_count_ == 0) {
        // the first time: the holder's priority is recomputed with this mutex's
        priority_ = thread->current_priority;
        thread->held_mutexes.insert(std::static_pointer_cast<Mutex>(shared_from_this()));
        holding_thread_ = thread;
        thread->UpdatePriority();
        kernel_.GetScheduler().PrepareReschedule();
    }
    ++lock_count_;
}

Result Mutex::Release(Thread* thread) {
    if (thread != holding_thread_) {
        return kResultWrongLockingThread;
    }
    if (lock_count_ == 0) {
        return kResultInvalidMutexState;
    }
    --lock_count_;
    if (lock_count_ == 0) {
        // (the holder's set may hold the last reference to this mutex)
        const std::shared_ptr<Mutex> self = std::static_pointer_cast<Mutex>(shared_from_this());
        Thread* holder = holding_thread_;
        holder->held_mutexes.erase(self);
        holding_thread_ = nullptr;
        holder->UpdatePriority();
        WakeupAllWaitingThreads();
        kernel_.GetScheduler().PrepareReschedule();
    }
    return kResultSuccess;
}

void Mutex::UpdatePriority() {
    if (holding_thread_ == nullptr) {
        return;
    }
    // the best priority among the waiters (no transitive propagation)
    u32 best = kThreadPrioLowest;
    for (const std::shared_ptr<Thread>& waiter : GetWaitingThreads()) {
        best = std::min(best, waiter->current_priority);
    }
    if (best != priority_) {
        priority_ = best;
        holding_thread_->UpdatePriority();
    }
}

void Mutex::AddWaitingThread(std::shared_ptr<Thread> thread) {
    WaitObject::AddWaitingThread(thread);
    thread->pending_mutexes.insert(std::static_pointer_cast<Mutex>(shared_from_this()));
    UpdatePriority();
}

void Mutex::RemoveWaitingThread(Thread* thread) {
    WaitObject::RemoveWaitingThread(thread);
    thread->pending_mutexes.erase(std::static_pointer_cast<Mutex>(shared_from_this()));
    UpdatePriority();
}

void ReleaseThreadMutexes(Thread* thread) {
    // whatever the recursion count: the thread ends. Wake-ups can touch the thread's sets, so work on a copy.
    const std::set<std::shared_ptr<Mutex>> held = std::move(thread->held_mutexes);
    thread->held_mutexes.clear();
    for (const std::shared_ptr<Mutex>& mutex : held) {
        mutex->lock_count_ = 0;
        mutex->holding_thread_ = nullptr;
        mutex->WakeupAllWaitingThreads();
    }
}

}  // namespace mh4n
