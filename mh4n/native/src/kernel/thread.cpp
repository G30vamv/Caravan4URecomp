// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the comments of mh4n/kernel.h (Thread, Scheduler::Wake/OnPriorityChanged, WakeHook),
// 3dbrew "Thread" and "SVC" (priority inheritance, wait timeouts) and the black-box specification of
// tests/kernel_objects_test.cpp and tests/threads_test.cpp.
#include <algorithm>

#include "internal.h"
#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {

namespace {

bool IsWaiting(ThreadStatus status) {
    switch (status) {
    case ThreadStatus::WaitArb:
    case ThreadStatus::WaitSleep:
    case ThreadStatus::WaitIPC:
    case ThreadStatus::WaitSynchAny:
    case ThreadStatus::WaitSynchAll:
    case ThreadStatus::WaitHleEvent:
        return true;
    default:
        return false;
    }
}

}  // namespace

Thread::Thread(Kernel& kernel, u32 thread_id, u32 priority, s32 processor_id)
    : WaitObject(kernel),
      thread_id(thread_id),
      nominal_priority(priority),
      current_priority(priority),
      processor_id(processor_id) {}

Thread::~Thread() = default;

bool Thread::ShouldWait(const Thread*) const {
    // signalled when the thread has ended
    return status != ThreadStatus::Dead;
}

void Thread::Acquire(Thread* thread) {
    MH4N_ASSERT(!ShouldWait(thread), "Thread::Acquire of a thread that has to wait");
}

void Thread::SetPriority(u32 priority) {
    MH4N_ASSERT(priority <= kThreadPrioLowest, "thread priority {} out of range", priority);
    const u32 old = current_priority;
    nominal_priority = current_priority = priority;
    kernel_.GetScheduler().OnPriorityChanged(*this, old);
    // the owners of the mutexes this thread waits for may now inherit another priority
    const std::set<std::shared_ptr<Mutex>> pending = pending_mutexes;
    for (const std::shared_ptr<Mutex>& mutex : pending) {
        mutex->UpdatePriority();
    }
}

void Thread::UpdatePriority() {
    // the best of the nominal priority and the priorities of the mutexes held
    u32 best = nominal_priority;
    for (const std::shared_ptr<Mutex>& mutex : held_mutexes) {
        best = std::min(best, mutex->Priority());
    }
    BoostPriority(best);
}

void Thread::BoostPriority(u32 priority) {
    const u32 old = current_priority;
    current_priority = priority;
    kernel_.GetScheduler().OnPriorityChanged(*this, old);
}

void Thread::BeginSynchWait(std::vector<std::shared_ptr<WaitObject>> objects, bool wait_all) {
    status = wait_all ? ThreadStatus::WaitSynchAll : ThreadStatus::WaitSynchAny;
    wait_objects = std::move(objects);
    const std::shared_ptr<Thread> self = std::static_pointer_cast<Thread>(shared_from_this());
    for (const std::shared_ptr<WaitObject>& object : wait_objects) {
        object->AddWaitingThread(self);
    }
}

void Thread::ArmTimeout(s64 ns) {
    CancelTimeout();
    if (ns == -1) {
        return;  // never
    }
    // a negative value other than -1 expires at once
    const u64 due = kernel_.NowTicks() + kernel_detail::NsToTicks(ns);
    const std::weak_ptr<Thread> weak = std::static_pointer_cast<Thread>(shared_from_this());
    // (the scheduler may run the callback inside ScheduleAt when `due` is not in the future: then it has already
    // cleared timeout_event_ and the id it returns is stale)
    const auto fired = std::make_shared<bool>(false);
    const u64 id = kernel_.ScheduleAt(due, [weak, fired] {
        *fired = true;
        if (const std::shared_ptr<Thread> thread = weak.lock()) {
            thread->timeout_event_ = 0;
            thread->OnWaitTimeout();
        }
    });
    if (!*fired) {
        timeout_event_ = id;
    }
}

void Thread::CancelTimeout() {
    if (timeout_event_ != 0) {
        kernel_.Cancel(timeout_event_);
        timeout_event_ = 0;
    }
}

void Thread::OnWaitTimeout() {
    timeout_event_ = 0;
    std::optional<Result> result;
    switch (status) {
    case ThreadStatus::WaitSynchAny:
    case ThreadStatus::WaitSynchAll:
    case ThreadStatus::WaitArb:
    case ThreadStatus::WaitHleEvent:
        result = kResultTimeout;
        break;
    case ThreadStatus::WaitSleep:
    case ThreadStatus::WaitIPC:
        break;  // these waits end without a result
    default:
        return;  // the thread is not in a wait that has a timeout
    }
    s32 index = -1;
    if (wake_hook) {
        wake_hook(ThreadWakeupReason::Timeout, nullptr, result, index);
    }
    for (const std::shared_ptr<WaitObject>& object : wait_objects) {
        object->RemoveWaitingThread(this);
    }
    wait_objects.clear();
    ResumeFromWait(result, index);
}

void Thread::ResumeFromWait(std::optional<Result> result, s32 index) {
    if (status == ThreadStatus::Ready || status == ThreadStatus::Running || status == ThreadStatus::Dead) {
        MH4N_LOG(Warning, "Kernel", "ResumeFromWait of thread {} in status {}", thread_id, static_cast<int>(status));
        return;
    }
    CancelTimeout();
    wake_hook = nullptr;
    status = ThreadStatus::Ready;
    kernel_.GetScheduler().Wake(std::static_pointer_cast<Thread>(shared_from_this()), result, index);
}

s32 Thread::GetWaitObjectIndex(const WaitObject* object) const {
    for (std::size_t i = wait_objects.size(); i > 0; --i) {
        if (wait_objects[i - 1].get() == object) {
            return static_cast<s32>(i - 1);
        }
    }
    return -1;
}

void Thread::Stop() {
    // a thread that is stopped while it waits: whoever blocked it learns that (the arbiter takes it off its list)
    if (IsWaiting(status) && wake_hook) {
        std::optional<Result> result;
        s32 index = -1;
        wake_hook(ThreadWakeupReason::Terminated, nullptr, result, index);
        wake_hook = nullptr;
    }
    const std::shared_ptr<Thread> self = std::static_pointer_cast<Thread>(shared_from_this());
    ReleaseThreadMutexes(this);
    status = ThreadStatus::Dead;
    WakeupAllWaitingThreads();
    // no dangling references in the objects the thread was waiting for
    for (const std::shared_ptr<WaitObject>& object : wait_objects) {
        object->RemoveWaitingThread(this);
    }
    wait_objects.clear();
    CancelTimeout();
    // the thread's slot of its process's resource limit is free again (the TLS slot is found free by looking at the
    // thread: Dead)
    if (const std::shared_ptr<Process> owner = owner_process.lock()) {
        if (owner->resource_limit != nullptr) {
            owner->resource_limit->Release(ResourceLimitType::Thread, 1);
        }
    }
}

}  // namespace mh4n
