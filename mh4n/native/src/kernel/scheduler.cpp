#include <cstdio>
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: scheduler.h, cpu.h, kernel.h, deterministic.h, scheduler_test.cpp,
// determinism_test.cpp and the boot-village scheduling observations in kernel_usage.md.

#include "mh4n/scheduler.h"

#include <algorithm>
#include <bit>
#include <limits>

#include "deterministic.h"
#include "internal.h"
#include "mh4n/assert.h"

namespace mh4n {
namespace {

void QueueEvent(LockstepState& state, int core, u64 tick, u64 id) {
    const u64 fifo = state.next_fifo[core]++;
    state.events[core].emplace(tick, fifo, id);
    state.fifo_of.emplace(id, fifo);
}

void ImportEvents(LockstepState& state, int core) {
    auto& pending = state.pending[core];
    for (const auto& event : pending) {
        QueueEvent(state, core, event.tick, event.id);
    }
    pending.clear();
}

u64 MaxStretch(const LockstepState& state, const VirtualTimeSource& time, int core) {
    const auto& events = state.events[core];
    if (events.empty()) {
        return kDeterministicSliceTicks;
    }
    const u64 next = std::get<0>(*events.begin());
    return next > time.CoreTicks(core) ? next - time.CoreTicks(core) : 0;
}

}  // namespace

struct GuestScheduler::SchedThread {
    std::unique_ptr<GuestThread> fiber;
    int queued_priority = -1;
    u32 id = 0;
    WaitOutcome outcome;
    std::optional<u32> svc;
    std::vector<SchedulerCounters::Named*> services;
};

GuestScheduler::GuestScheduler(Kernel& kernel, CodeHost& host, GameClock& clock, TimeSource& time,
                               PreemptionSource* preemption, Options options)
    : kernel_(kernel), host_(host), clock_(clock), time_(time), preemption_(preemption),
      slice_us_(options.slice_us) {
    kernel_.SetScheduler(this);
    kernel_.SetClock(&clock_);
    host_.SetBudgetHandler([this](RCtx&) { Preempt(); });
    clock_.SetChangeCallback([this] { RaiseFlag(); });
    if (preemption_) {
        preemption_->Start([this] { RaiseFlag(); });
    }
}

GuestScheduler::~GuestScheduler() {
    Shutdown();
}

GuestScheduler::SchedThread& GuestScheduler::Sd(Thread& thread) {
    return *static_cast<SchedThread*>(thread.scheduler_data.get());
}

int GuestScheduler::CoreOf(const Thread& thread) const {
    MH4N_ASSERT(thread.processor_id == 0 || thread.processor_id == 1);
    return thread.processor_id;
}

void GuestScheduler::Enqueue(Thread& thread, bool front) {
    auto& state = Sd(thread);
    auto& core = cores_[CoreOf(thread)];
    const auto priority = thread.GetPriority();
    MH4N_ASSERT(priority < core.ready.size() && state.queued_priority == -1);
    auto& queue = core.ready[priority];
    if (front) {
        queue.push_front(&thread);
    } else {
        queue.push_back(&thread);
    }
    core.mask |= u64{1} << priority;
    state.queued_priority = static_cast<int>(priority);
}

void GuestScheduler::Dequeue(Thread& thread) {
    auto& state = Sd(thread);
    if (state.queued_priority < 0) {
        return;
    }
    auto& core = cores_[CoreOf(thread)];
    auto& queue = core.ready[state.queued_priority];
    std::erase(queue, &thread);
    if (queue.empty()) {
        core.mask &= ~(u64{1} << state.queued_priority);
    }
    state.queued_priority = -1;
}

bool GuestScheduler::Runnable(int core) const {
    const auto& slot = cores_[core];
    return slot.mask != 0 || (slot.current && slot.current->status == ThreadStatus::Running);
}

bool GuestScheduler::Urgent(int core) const {
    const auto& slot = cores_[core];
    return slot.mask != 0 &&
           (!slot.current || slot.current->status != ThreadStatus::Running ||
            static_cast<u32>(std::countr_zero(slot.mask)) < slot.current->GetPriority());
}

void GuestScheduler::Normalize(Core& core) {
    if (core.current && core.current->status != ThreadStatus::Running) {
        core.current = nullptr;
    }
}

Thread* GuestScheduler::PopNextReady(Core& core) {
    while (core.mask != 0) {
        const int priority = std::countr_zero(core.mask);
        auto& queue = core.ready[priority];
        auto* thread = queue.front();
        queue.pop_front();
        if (queue.empty()) {
            core.mask &= ~(u64{1} << priority);
        }
        Sd(*thread).queued_priority = -1;
        if (!core.filter || !core.filter(*thread)) {
            return thread;
        }
    }
    return nullptr;
}

void GuestScheduler::ChooseCore(s64 now_ns) {
    const int other = 1 - active_;
    if (Runnable(other) && (!Runnable(active_) ||
        (slice_active_ && (now_ns >= slice_end_ns_ || Urgent(other))))) {
        active_ = other;
        ++counters_.core_switches;
        slice_active_ = false;
    }
    if (!slice_active_ || now_ns >= slice_end_ns_) {
        const s64 width = std::max<s64>(1, static_cast<s64>(slice_us_) * 1000);
        constexpr s64 max = std::numeric_limits<s64>::max();
        slice_end_ns_ = now_ns > max - width ? max : now_ns + width;
        slice_active_ = true;
    }
}

Thread* GuestScheduler::Dispatch() {
    for (auto& core : cores_) {
        Normalize(core);
    }
    reschedule_pending_ = false;

    const auto select = [this](int index) -> Thread* {
        auto& core = cores_[index];
        auto* current = core.current;
        if (current && Urgent(index)) {
            current->last_running_ticks = clock_.Now();
            ++counters_.preemptions;
            current->status = ThreadStatus::Ready;
            Enqueue(*current, true);
            core.current = current = nullptr;
        }
        if (current && core.filter && core.filter(*current)) {
            core.current = current = nullptr;
        }
        if (!current) {
            core.current = current = PopNextReady(core);
            if (current) {
                current->status = ThreadStatus::Running;
            }
        }
        return current;
    };

    if (auto* state = kernel_detail::LockstepOf(time_)) {
        using Stage = LockstepState::Stage;
        auto& time = static_cast<VirtualTimeSource&>(time_);
        state->scheduler_thread = std::this_thread::get_id();
        state->progressed = false;

        const auto end_stretch = [&] {
            if (state->stretch_core == 0 && state->after_stretch == Stage::Core1) {
                state->max_slice = time.CoreTicks(0) - state->stretch_start;
            }
            state->stage = state->after_stretch;
            state->in_stretch = false;
        };
        if (state->in_stretch) {
            end_stretch();
            if (state->system_pending) {
                state->system_pending = false;
                select(state->stretch_core);
            }
        }

        const auto advance = [&](int core) {
            time.SetCore(core);
            ImportEvents(*state, core);
            RunDueTimers();
            select(core);
        };
        if (state->stage == Stage::IterStart) {
            if (!Runnable(0) && !Runnable(1) && timers_.empty()) {
                return nullptr;
            }
            ++state->iterations;
            state->ran_thread = false;
            const int behind = time.CoreTicks(0) <= time.CoreTicks(1) ? 0 : 1;
            const u64 gap = time.CoreTicks(1 - behind) - time.CoreTicks(behind);
            // Small differences come from the instruction block that exhausted the budget.
            if (gap > 100) {
                advance(behind);
                state->delayed_slice[behind] = gap;
                state->stretch_core = behind;
                state->stage = Stage::Sync;
            } else {
                advance(0);
                advance(1);
                state->slice_cap = kernel_detail::DrawSliceTicks();
                state->max_slice = std::min({state->slice_cap, MaxStretch(*state, time, 0),
                                             MaxStretch(*state, time, 1)});
                state->stage = Stage::Core0;
            }
            state->system_pending = false;
        }

        while (state->stage != Stage::IterStart && !StopWanted()) {
            const Stage stage = state->stage;
            const int core = stage == Stage::Sync ? state->stretch_core : (stage == Stage::Core1 ? 1 : 0);
            const u64 cap = stage == Stage::Sync ? state->delayed_slice[core] : state->max_slice;
            // Core 1 receives the elapsed time of core 0, including its final block.
            const u64 budget = state->events[core].empty() ? cap :
                               std::min(cap, MaxStretch(*state, time, core));
            if (active_ != core) {
                active_ = core;
                ++counters_.core_switches;
            }
            time.SetCore(core);
            state->stretch_core = core;
            state->stretch_start = time.CoreTicks(core);
            state->stretch_end = state->stretch_start + budget;
            state->after_stretch = stage == Stage::Core0 ? Stage::Core1 : Stage::IterStart;
            state->in_stretch = true;
            if (auto* thread = cores_[core].current) {
                state->ran_thread = true;
                kernel_detail::BeginStretch(*Sd(*thread).fiber, static_cast<s64>(budget));
                return thread;
            }
            time.AdvanceCoreTicks(core, budget);
            counters_.idle_ns += TicksToNs(budget);
            end_stretch();
        }
        state->progressed = true;
        return nullptr;
    }

    const s64 now = time_.NowNs();
    ChooseCore(now);
    auto* thread = select(active_);
    if (!thread && Runnable(1 - active_)) {
        ChooseCore(now);
        thread = select(active_);
    }
    if (!thread) {
        slice_active_ = false;
    }
    return thread;
}

void GuestScheduler::NeedReschedule(int core) {
    if (guest_running_ && Urgent(core)) {
        PrepareReschedule();
        RaiseFlag();
    }
}

void GuestScheduler::Wake(const std::shared_ptr<Thread>& thread, std::optional<Result> result,
                          s32 index) {
    if (!thread->scheduler_data || !threads_.contains(thread.get())) {
        return;
    }
    auto& state = Sd(*thread);
    if (state.queued_priority >= 0) {
        return;  // already woken (a signal and a timeout in the same slice): the first wake wins (found by fs_romfs_test)
    }
    if (result) {
        state.outcome.result = result;
    }
    if (index >= 0) {
        state.outcome.index = index;
    }
    Enqueue(*thread, false);
    NeedReschedule(CoreOf(*thread));
}

void GuestScheduler::OnPriorityChanged(Thread& thread, u32) {
    if (!thread.scheduler_data) {
        return;
    }
    if (Sd(thread).queued_priority >= 0) {
        Dequeue(thread);
        Enqueue(thread, false);
    }
    NeedReschedule(CoreOf(thread));
}

void GuestScheduler::PrepareReschedule() {
    reschedule_pending_ = true;
    if (auto* lockstep = kernel_detail::LockstepOf(time_)) {
        lockstep->system_pending = true;
    }
}

Thread* GuestScheduler::CurrentThread() {
    auto* fiber = CodeHost::CurrentThread();
    if (!fiber || &fiber->Host() != &host_ || !guest_running_) {
        return nullptr;
    }
    auto* thread = cores_[active_].current;
    return thread && Sd(*thread).fiber.get() == fiber ? thread : nullptr;
}

std::shared_ptr<Thread> GuestScheduler::CurrentThreadPtr() {
    auto* thread = CurrentThread();
    return thread ? threads_.at(thread) : nullptr;
}

Thread* GuestScheduler::ThreadOnCore(int core) const {
    return cores_.at(core).current;
}

std::size_t GuestScheduler::ReadyCount(int core) const {
    std::size_t count = 0;
    for (const auto& queue : cores_.at(core).ready) {
        count += queue.size();
    }
    return count;
}

GuestThread& GuestScheduler::Attach(const std::shared_ptr<Thread>& thread, GuestThread::Entry entry) {
    MH4N_ASSERT(thread->status == ThreadStatus::Dormant && !thread->scheduler_data);
    auto state = std::make_shared<SchedThread>();
    state->id = thread->thread_id;
    state->fiber = host_.NewThread(thread->thread_id, std::move(entry));
    auto& fiber = *state->fiber;
    threads_.emplace(thread.get(), thread);
    thread->scheduler_data = std::move(state);
    return fiber;
}

GuestThread& GuestScheduler::AttachFunction(const std::shared_ptr<Thread>& thread,
                                            std::function<void()> body) {
    return Attach(thread, [this, body = std::move(body)](GuestThread&) {
        body();
        ExitCurrent();
    });
}

void GuestScheduler::Start(const std::shared_ptr<Thread>& thread) {
    MH4N_ASSERT(thread->status == ThreadStatus::Dormant && threads_.contains(thread.get()));
    thread->status = ThreadStatus::Ready;
    Enqueue(*thread, false);
    NeedReschedule(CoreOf(*thread));
}

[[noreturn]] void GuestScheduler::ExitCurrent() {
    auto* thread = CurrentThread();
    auto* self = CodeHost::CurrentThread();
    MH4N_ASSERT(thread && self);
    thread->last_running_ticks = clock_.Now();
    thread->Stop();
    cores_[active_].current = nullptr;
    graveyard_.push_back(std::move(threads_.at(thread)));
    threads_.erase(thread);
    SwitchFromSelf(self);
    MH4N_UNREACHABLE();
}

void GuestScheduler::Terminate(const std::shared_ptr<Thread>& thread) {
    if (thread.get() == CurrentThread()) {
        ExitCurrent();
    }
    const auto it = threads_.find(thread.get());
    if (it == threads_.end()) {
        return;
    }
    Dequeue(*thread);
    auto& core = cores_[CoreOf(*thread)];
    if (core.current == thread.get()) {
        core.current = nullptr;
    }
    thread->Stop();
    graveyard_.push_back(std::move(it->second));
    threads_.erase(it);
    PrepareReschedule();
}

void GuestScheduler::Reap() {
    auto* self = CodeHost::CurrentThread();
    std::erase_if(graveyard_, [self](const auto& thread) {
        if (Sd(*thread).fiber.get() == self) {
            return false;
        }
        thread->scheduler_data.reset();
        return true;
    });
}

WaitOutcome GuestScheduler::Block(Thread& thread, std::vector<std::shared_ptr<WaitObject>> objects,
                                  bool wait_all, s64 timeout_ns) {
    if (wait_all) {
        if (std::none_of(objects.begin(), objects.end(), [&](const auto& obj) {
                return obj->ShouldWait(&thread);
            })) {
            for (const auto& obj : objects) {
                obj->Acquire(&thread);
            }
            return {kResultSuccess, -1, false};
        }
    } else {
        for (std::size_t i = 0; i < objects.size(); ++i) {
            if (!objects[i]->ShouldWait(&thread)) {
                objects[i]->Acquire(&thread);
                return {kResultSuccess, static_cast<s32>(i), false};
            }
        }
    }
    if (timeout_ns == 0) {
        return {kResultTimeout, -1, false};
    }
    thread.BeginSynchWait(std::move(objects), wait_all);
    thread.ArmTimeout(timeout_ns);
    return Park(thread);
}

WaitOutcome GuestScheduler::Park(Thread& thread) {
    MH4N_ASSERT(CurrentThread() == &thread && thread.status != ThreadStatus::Running);
    thread.last_running_ticks = clock_.Now();
    auto& state = Sd(thread);
    state.outcome = {{}, -1, true};
    SwitchFromSelf(state.fiber.get());
    return std::exchange(state.outcome, {});
}

void GuestScheduler::Sleep(s64 ns) {
    auto* thread = CurrentThread();
    MH4N_ASSERT(thread);
    if (ns == 0) {
        auto& core = cores_[active_];
        if (core.mask == 0) {
            return;
        }
        if (static_cast<u32>(std::countr_zero(core.mask)) <= thread->GetPriority()) {
            thread->status = ThreadStatus::Ready;
            Enqueue(*thread, false);
            thread->last_running_ticks = clock_.Now();
            SwitchFromSelf(Sd(*thread).fiber.get());
            return;
        }
    }
    thread->status = ThreadStatus::WaitSleep;
    thread->ArmTimeout(ns);
    Park(*thread);
}

u64 GuestScheduler::ScheduleAt(u64 tick, std::function<void()> fn) {
    const u64 id = next_timer_id_++;
    timers_.emplace(std::pair{tick, id}, std::move(fn));
    timer_ticks_.emplace(id, tick);
    if (auto* state = kernel_detail::LockstepOf(time_)) {
        const int running = static_cast<VirtualTimeSource&>(time_).Core();
        const int requested = std::exchange(state->schedule_core, -1);
        const int core = requested >= 0 ? requested : running;
        const bool pending = std::exchange(state->schedule_pending, false);
        if (pending || core != running) {
            state->pending[core].push_back({tick, id});
        } else {
            QueueEvent(*state, core, tick, id);
            if (state->in_stretch && state->stretch_core == core) {
                state->stretch_end = std::min(state->stretch_end, tick);
            }
        }
    }
    UpdateArm();
    return id;
}

bool GuestScheduler::Cancel(u64 id) {
    const auto it = timer_ticks_.find(id);
    if (it == timer_ticks_.end()) {
        return false;
    }
    const u64 tick = it->second;
    timer_ticks_.erase(it);
    auto event = timers_.extract(std::pair{tick, id});
    if (auto* state = kernel_detail::LockstepOf(time_)) {
        auto fifo = state->fifo_of.extract(id);
        if (fifo) {
            for (auto& queue : state->events) {
                if (queue.erase({tick, fifo.mapped(), id})) {
                    break;
                }
            }
        } else {
            for (auto& pending : state->pending) {
                std::erase_if(pending, [id](const auto& entry) { return entry.id == id; });
            }
        }
    }
    UpdateArm();
    return true;
}

bool GuestScheduler::RunDueTimers() {
    if (timers_.empty()) {
        return false;
    }
    bool ran = false;
    if (auto* state = kernel_detail::LockstepOf(time_)) {
        auto& queue = state->events[static_cast<VirtualTimeSource&>(time_).Core()];
        while (!queue.empty() && std::get<0>(*queue.begin()) <= clock_.Now()) {
            const auto [tick, fifo, id] = *queue.begin();
            queue.erase(queue.begin());
            state->fifo_of.erase(id);
            timer_ticks_.erase(id);
            auto event = timers_.extract(std::pair{tick, id});
            event.mapped()();
            ran = true;
        }
        return ran;
    }
    while (!timers_.empty() && timers_.begin()->first.first <= clock_.Now()) {
        auto event = timers_.extract(timers_.begin());
        timer_ticks_.erase(event.key().second);
        ran = true;
        event.mapped()();
    }
    return ran;
}

bool GuestScheduler::ProcessPending(bool until_quiet, bool run_timers) {
    if (in_pending_) {
        return false;
    }
    in_pending_ = true;
    bool ran = false;
    bool processed;
    do {
        std::vector<std::function<void()>> work;
        {
            std::lock_guard lock(mutex_);
            __atomic_store_n(&preempt_flag_, 0, __ATOMIC_RELEASE);
            work.swap(posted_);
        }
        for (auto& fn : work) {
            fn();
        }
        const bool timers_due = run_timers && (until_quiet || !kernel_detail::LockstepOf(time_));
        processed = (timers_due && RunDueTimers()) || !work.empty();
        ran |= processed;
    } while (until_quiet && processed);
    in_pending_ = false;
    return ran;
}

void GuestScheduler::Post(std::function<void()> fn) {
    {
        std::lock_guard lock(mutex_);
        posted_.push_back(std::move(fn));
        __atomic_store_n(&preempt_flag_, 1, __ATOMIC_RELEASE);
    }
    cv_.notify_one();
}

void GuestScheduler::RaiseFlag() {
    {
        std::lock_guard lock(mutex_);
        __atomic_store_n(&preempt_flag_, 1, __ATOMIC_RELEASE);
    }
    cv_.notify_one();
}

void GuestScheduler::RequestStop() {
    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
        __atomic_store_n(&preempt_flag_, 1, __ATOMIC_RELEASE);
    }
    cv_.notify_one();
}

bool GuestScheduler::StopWanted() {
    {
        std::lock_guard lock(mutex_);
        if (stop_requested_) {
            return true;
        }
    }
    return until_ && until_();
}

void GuestScheduler::UpdateArm() {
    if (!preemption_ || in_pending_) {
        return;
    }
    std::optional<s64> deadline;
    if (auto* state = kernel_detail::LockstepOf(time_)) {
        if (state->in_stretch) {
            deadline = clock_.HostNsFor(state->stretch_end);
        }
    } else if (!timers_.empty()) {
        deadline = clock_.HostNsFor(timers_.begin()->first.first);
    }
    if (guest_running_ && slice_active_ && (Runnable(1 - active_) || !hooks_.empty() || until_)) {
        if (!deadline || slice_end_ns_ < *deadline) {
            deadline = slice_end_ns_;
        }
    }
    if (deadline != last_armed_) {
        last_armed_ = deadline;
        preemption_->Arm(deadline);
    }
}

void GuestScheduler::IdleWait() {
    const auto deadline = timers_.empty() ? std::nullopt : clock_.HostNsFor(timers_.begin()->first.first);
    const s64 start = time_.NowNs();
    std::unique_lock lock(mutex_);
    idle_waiting_ = true;
    time_.WaitUntil(cv_, lock, deadline, [this] {
        return stop_requested_ || !posted_.empty() ||
               __atomic_load_n(&preempt_flag_, __ATOMIC_ACQUIRE) != 0;
    });
    idle_waiting_ = false;
    const s64 end = time_.NowNs();
    if (end > start) {
        counters_.idle_ns += static_cast<u64>(end) - static_cast<u64>(start);
    }
}

void GuestScheduler::SwitchFromSelf(GuestThread* self) {
    auto* old = CurrentThread();
    const int previous_core = active_;
    const bool stopping = StopWanted();
    auto* next = stopping ? nullptr : Dispatch();
    if (next && Sd(*next).fiber.get() == self) {
        UpdateArm();
        return;
    }
    if (old && (old->status == ThreadStatus::Running ||
                (old->status == ThreadStatus::Ready && old != next)) &&
        previous_core != active_) {
        old->last_running_ticks = clock_.Now();
        ++counters_.preemptions;
    }
    ++counters_.thread_switches;
    if (next) {
        auto& state = Sd(*next);
        AccountTo(&state);
        UpdateArm();
        self->SwitchTo(*state.fiber);
    } else {
        // A stopped run keeps its selection for the next call to Run.
        if (stopping) {
            active_ = previous_core;
        }
        guest_running_ = false;
        AccountTo(nullptr);
        self->Yield();
    }
    Reap();
}

void GuestScheduler::Reschedule() {
    if (auto* self = CodeHost::CurrentThread()) {
        SwitchFromSelf(self);
    }
}

RunStatus GuestScheduler::Run(const std::function<bool()>& until, bool return_when_idle) {
    until_ = until;
    RunStatus result;
    for (;;) {
        Reap();
        if (host_.Failure()) {
            result = RunStatus::Failed;
            break;
        }
        ProcessPending(false);
        RunHooks();
        if (StopWanted()) {
            result = RunStatus::Stopped;
            break;
        }
        if (auto* thread = Dispatch()) {
            auto& state = Sd(*thread);
            guest_running_ = true;
            UpdateArm();
            AccountTo(&state);
            ++counters_.thread_switches;
            host_.Enter(*state.fiber);
            AccountTo(nullptr);
            guest_running_ = false;
            continue;
        }
        if (const auto* state = kernel_detail::LockstepOf(time_); state && state->progressed) {
            continue;
        }
        if (return_when_idle && timers_.empty()) {
            std::lock_guard lock(mutex_);
            if (posted_.empty() && !stop_requested_) {
                result = RunStatus::Idle;
                break;
            }
            continue;
        }
        IdleWait();
    }
    if (result == RunStatus::Stopped) {
        std::lock_guard lock(mutex_);
        stop_requested_ = false;
    }
    until_ = {};
    UpdateArm();
    return result;
}

bool GuestScheduler::RunPending(bool run_timers) {
    const bool ran = ProcessPending(true, run_timers);
    UpdateArm();
    return ran;
}

void GuestScheduler::Preempt() {
    ++counters_.preempt_checks;
    Reap();
    ProcessPending(false);
    RunHooks();
    if (const auto* state = kernel_detail::LockstepOf(time_); state && state->in_stretch &&
        !state->system_pending && static_cast<VirtualTimeSource&>(time_).Ticks() < state->stretch_end &&
        !StopWanted()) {
        UpdateArm();
        return;
    }
    Reschedule();
}

s64 GuestScheduler::AccountNowNs() {
    // Keep accounting on the same monotonic source for both production and injected clocks.
    static_cast<void>(account_counter_mult_);
    return time_.NowNs();
}

void GuestScheduler::AccountTo(SchedThread* to) {
    FlushCategory();
    acct_ = to;
}

void GuestScheduler::FlushCategory() {
    const s64 now = AccountNowNs();
    const u64 elapsed = now > acct_start_ns_ ? static_cast<u64>(now) - static_cast<u64>(acct_start_ns_) : 0;
    acct_start_ns_ = now;
    if (!acct_) {
        return;
    }
    if (!acct_->services.empty()) {
        acct_->services.back()->ns += elapsed;
        counters_.thread_service_ns[acct_->id] += elapsed;
    } else if (acct_->svc) {
        counters_.svc[*acct_->svc].ns += elapsed;
        counters_.thread_svc_ns[acct_->id] += elapsed;
    } else {
        counters_.guest_ns[acct_->id] += elapsed;
    }
}

void GuestScheduler::BeginSvc(u32 number) {
    if (acct_ && number < counters_.svc.size()) {
        FlushCategory();
        acct_->svc = number;
        ++counters_.svc[number].calls;
    }
    RunHooks();
}

void GuestScheduler::EndSvc() {
    if (reschedule_pending_ || __atomic_load_n(&preempt_flag_, __ATOMIC_ACQUIRE) != 0 ||
        (until_ && until_())) {
        Preempt();
    }
    if (acct_ && acct_->svc) {
        FlushCategory();
        acct_->svc.reset();
    }
}

void GuestScheduler::BeginService(std::string_view name) {
    if (acct_) {
        FlushCategory();
        auto& service = counters_.services.try_emplace(std::string(name)).first->second;
        ++service.calls;
        acct_->services.push_back(&service);
    }
}

void GuestScheduler::EndService() {
    if (acct_ && !acct_->services.empty()) {
        FlushCategory();
        acct_->services.pop_back();
    }
}

void GuestScheduler::RunHooks() {
    ++running_hooks_;
    for (std::size_t i = 0; i < hooks_.size(); ++i) {
        hooks_[i]();
        if (!retired_hooks_.empty()) {
            break;
        }
    }
    if (--running_hooks_ == 0) {
        retired_hooks_.clear();
    }
}

void GuestScheduler::OnSafePoint(std::function<void()> hook) {
    hooks_.push_back(std::move(hook));
    UpdateArm();
}

void GuestScheduler::ClearSafePointHooks() {
    if (running_hooks_ && !hooks_.empty()) {
        retired_hooks_.emplace_back();
        retired_hooks_.back().swap(hooks_);
    } else {
        hooks_.clear();
    }
    UpdateArm();
}

void GuestScheduler::SetSelectionFilter(int core, SelectionFilter filter) {
    cores_.at(core).filter = std::move(filter);
}

void GuestScheduler::Shutdown() {
    clock_.SetChangeCallback({});
    if (preemption_) {
        std::exchange(preemption_, nullptr)->Stop();
    }
    host_.SetBudgetHandler({});
    for (auto& [thread, owner] : threads_) {
        thread->CancelTimeout();
        thread->scheduler_data.reset();
    }
    for (auto& core : cores_) {
        for (auto& queue : core.ready) {
            queue.clear();
        }
        core.mask = 0;
        core.current = nullptr;
    }
    threads_.clear();
    Reap();
}

}  // namespace mh4n
