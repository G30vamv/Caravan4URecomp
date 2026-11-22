// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the comments of mh4n/kernel.h (Timer), 3dbrew "SVC" (CreateTimer, SetTimer, CancelTimer,
// ClearTimer) and the black-box specification of tests/kernel_objects_test.cpp.
#include <algorithm>

#include "internal.h"
#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {

Timer::Timer(Kernel& kernel, ResetType reset_type) : WaitObject(kernel), reset_type_(reset_type) {}

Timer::~Timer() {
    Cancel();
    if (resource_limit != nullptr) {
        resource_limit->Release(ResourceLimitType::Timer, 1);
    }
}

bool Timer::ShouldWait(const Thread*) const {
    return !signaled_;
}

void Timer::Acquire(Thread* thread) {
    MH4N_ASSERT(!ShouldWait(thread), "Timer::Acquire of a thread that has to wait");
    if (reset_type_ == ResetType::OneShot) {
        signaled_ = false;
    }
}

void Timer::WakeupAllWaitingThreads() {
    WaitObject::WakeupAllWaitingThreads();
    if (reset_type_ == ResetType::Pulse) {
        signaled_ = false;
    }
}

void Timer::Set(s64 initial_ns, s64 interval_ns) {
    Cancel();
    initial_delay_ = std::max<s64>(initial_ns, 0);
    interval_delay_ = std::max<s64>(interval_ns, 0);
    const u64 now = kernel_.NowTicks();
    if (initial_delay_ == 0) {
        // the first shot is now
        deadline_ = now;
        Fire();
    } else {
        Schedule(now + kernel_detail::NsToTicks(initial_delay_));
    }
}

void Timer::Cancel() {
    if (event_ != 0) {
        kernel_.Cancel(event_);
        event_ = 0;
    }
}

void Timer::Clear() {
    signaled_ = false;
}

void Timer::Fire() {
    event_ = 0;  // the event that called this one is spent
    signaled_ = true;
    WakeupAllWaitingThreads();
    if (interval_delay_ > 0 && event_ == 0) {
        // one interval after this shot's deadline, so the period does not drift
        Schedule(deadline_ + kernel_detail::NsToTicks(interval_delay_));
    }
}

void Timer::Schedule(u64 deadline) {
    deadline_ = deadline;
    // (~Timer cancels the event, so `this` outlives it)
    event_ = kernel_.ScheduleAt(deadline, [this] { Fire(); });
}

}  // namespace mh4n
