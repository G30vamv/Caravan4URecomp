// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: cpu_limiter.h, threads.h, threads_test.cpp, determinism_test.cpp and 3dbrew KResourceLimit.

#include "cpu_limiter.h"

#include <algorithm>

#include "deterministic.h"

namespace mh4n {

Core1Limiter::Core1Limiter(Kernel& kernel, GuestScheduler& scheduler)
    : kernel_(kernel), scheduler_(scheduler) {}

Core1Limiter::~Core1Limiter() {
    End();
}

void Core1Limiter::SetScheduleMode(Core1ScheduleMode mode) {
    End();
    mode_ = mode;
    Start();
}

void Core1Limiter::Start() {
    phase_ = Core1Phase::App;
    ready_ = true;
    scheduler_.SetSelectionFilter(1, [this](Thread& thread) { return DoTimeLimit(thread); });
}

void Core1Limiter::End() {
    if (!ready_) {
        return;
    }
    ready_ = false;
    active_ = false;
    CancelTick();
    scheduler_.SetSelectionFilter(1, {});
    WakeupSleepingThreads();
}

void Core1Limiter::UpdateAppCpuLimit() {
    if (!ready_) {
        return;
    }
    app_cpu_time_ = kernel_.ApplicationResourceLimit()->GetCurrentValue(ResourceLimitType::CpuTime);
    CancelTick();
    if (app_cpu_time_ == kCore1CpuTimeDisabled) {
        active_ = false;
        phase_ = Core1Phase::App;
        WakeupSleepingThreads();
        return;
    }
    active_ = true;
    ChangeState(0);
    WakeupSleepingThreads();
}

bool Core1Limiter::DoTimeLimit(Thread& thread) {
    if (!active_ || phase_ != Core1Phase::Sys || app_cpu_time_ >= 100) {
        return false;
    }
    const auto process = thread.owner_process.lock();
    if (!process || !process->resource_limit ||
        process->resource_limit->GetCategory() != ResourceLimitCategory::Application ||
        process->resource_limit->GetLimitValue(ResourceLimitType::CpuTime) == kCore1CpuTimeExempted) {
        return false;
    }
    thread.status = ThreadStatus::WaitSleep;
    sleeping_.emplace_back(std::static_pointer_cast<Thread>(thread.shared_from_this()));
    return true;
}

std::size_t Core1Limiter::Held() const {
    return static_cast<std::size_t>(std::count_if(sleeping_.begin(), sleeping_.end(), [](const auto& slot) {
        const auto thread = slot.lock();
        return thread && thread->status == ThreadStatus::WaitSleep;
    }));
}

void Core1Limiter::OnTick() {
    const u64 now = kernel_detail::CoreTicksOr(1, scheduler_.NowTicks());
    const s64 late = now > tick_due_ ? static_cast<s64>(now - tick_due_) : 0;
    tick_event_ = 0;
    ChangeState(late);
    WakeupSleepingThreads();
    scheduler_.PrepareReschedule();
}

void Core1Limiter::ChangeState(s64 cycles_late) {
    phase_ = phase_ == Core1Phase::App ? Core1Phase::Sys : Core1Phase::App;
    if (app_cpu_time_ >= 100) {
        return;
    }
    const auto app = static_cast<s64>(static_cast<float>(kBaseTickInterval) *
                                      (static_cast<float>(app_cpu_time_) / 100.0f));
    const s64 interval = phase_ == Core1Phase::App ? app : static_cast<s64>(kBaseTickInterval) - app;
    const s64 delay = interval > cycles_late ? interval - cycles_late : interval;
    tick_due_ = kernel_detail::CoreTicksOr(1, scheduler_.NowTicks()) + static_cast<u64>(delay);
    kernel_detail::ScheduleOnCore(1);
    tick_event_ = scheduler_.ScheduleAt(tick_due_, [this] { OnTick(); });
}

void Core1Limiter::WakeupSleepingThreads() {
    if (active_ && phase_ == Core1Phase::Sys && app_cpu_time_ < 100) {
        return;
    }
    for (const auto& slot : sleeping_) {
        if (const auto thread = slot.lock(); thread && thread->status == ThreadStatus::WaitSleep) {
            thread->ResumeFromWait(std::nullopt);
        }
    }
    sleeping_.clear();
}

void Core1Limiter::CancelTick() {
    if (tick_event_ != 0) {
        scheduler_.Cancel(std::exchange(tick_event_, 0));
    }
}

}  // namespace mh4n
