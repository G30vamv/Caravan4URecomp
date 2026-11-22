// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: deterministic.h, determinism_test.cpp, Runtime::Prepare and SvcCycles black-box measurements.

#include "deterministic.h"

#include <limits>
#include <utility>

#include "mh4n/assert.h"
#include "mh4n/log.h"
#include "../cpu/recomp_c.h"

namespace mh4n {
namespace {

thread_local CycleDriver* current_driver = nullptr;

constexpr u64 kSharedPagePeriod = kTicksPerSecond * 3600;
constexpr u64 kCheatPeriod = 50000000;

void ScheduleTimingMarker(GuestScheduler& scheduler, VirtualTimeSource& time, u64 due, u64 period) {
    scheduler.ScheduleAt(due, [&scheduler, &time, due, period] {
        if (time.Lockstep().enabled) {
            ScheduleTimingMarker(scheduler, time, due + period, period);
        }
    });
}

constexpr auto kSvcCosts = [] {
    std::array<u32, 0xB4> costs{};
    for (const u32 number : {
             0x01, 0x02, 0x03, 0x09, 0x13, 0x15, 0x1A, 0x1C, 0x1D, 0x1E, 0x1F,
             0x20, 0x21, 0x27, 0x29, 0x2A, 0x2C, 0x2D, 0x33, 0x34, 0x35, 0x36,
             0x38, 0x39, 0x3A, 0x3C, 0x3D, 0x47, 0x65, 0x76, 0x79, 0x7C, 0x7D,
             0x90, 0x93, 0x94, 0xA0, 0xA1, 0xB3,
         }) {
        costs[number] = 1000;
    }
    costs[0x08] = 5214;
    costs[0x0A] = 946;
    costs[0x0B] = 616;
    costs[0x0C] = 1812;
    costs[0x14] = 1324;
    costs[0x16] = 2713;
    costs[0x17] = 4329;
    costs[0x18] = 3285;
    costs[0x19] = 1389;
    costs[0x1B] = 5163;
    costs[0x22] = 5664;
    costs[0x23] = 2937;
    costs[0x24] = 4005;
    costs[0x25] = 6918;
    costs[0x28] = 340;
    costs[0x2B] = 1510;
    costs[0x32] = 5825;
    costs[0x37] = 677;
    costs[0x48] = 5122;
    costs[0x49] = 3492;
    costs[0x4A] = 1842;
    costs[0x4F] = 8762;
    costs[0x52] = 9609;
    costs[0x53] = 7174;
    costs[0x54] = 9084;
    return costs;
}();

}  // namespace

u32 SvcCycles(u32 number) {
    return number < kSvcCosts.size() ? kSvcCosts[number] : 0;
}

u64 VirtualTimeSource::TicksAtOrAfterNs(s64 ns) {
    // NowNs rounds up. To reach ns, the unrounded reading must exceed ns - 1.
    return ns > 0 ? NsToTicks(static_cast<u64>(ns - 1)) + 1 : 0;
}

void VirtualTimeSource::AdvanceCoreToTick(int core, u64 tick) {
    auto& clock = ticks_[core];
    u64 previous = clock.load(std::memory_order_acquire);
    while (previous < tick &&
           !clock.compare_exchange_weak(previous, tick, std::memory_order_acq_rel)) {
    }
}

bool VirtualTimeSource::WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                                  std::optional<s64> deadline_ns, const std::function<bool()>& pred) {
    if (!deadline_ns) {
        cv.wait(lock, [&] {
            const bool ready = pred();
            if (!ready && !warned_idle_) {
                warned_idle_ = true;
                MH4N_LOG(Warning, "Kernel", "Deterministic scheduler is idle without a timer deadline");
            }
            return ready;
        });
        return true;
    }
    if (!pred()) {
        const u64 deadline = TicksAtOrAfterNs(*deadline_ns);
        if (lockstep_.enabled) {
            AdvanceCoreToTick(0, deadline);
            AdvanceCoreToTick(1, deadline);
        } else {
            AdvanceToTick(deadline);
        }
    }
    return pred();
}

void DeterministicPreemption::Start(std::function<void()> raise) {
    std::lock_guard lock(mutex_);
    raise_ = std::move(raise);
}

void DeterministicPreemption::Stop() {
    std::lock_guard lock(mutex_);
    raise_ = {};
    deadline_.reset();
}

void DeterministicPreemption::SetEpochSource(std::function<u64()> preempt_checks) {
    std::lock_guard lock(mutex_);
    epoch_ = std::move(preempt_checks);
    armed_at_epoch_ = epoch_ ? epoch_() : 0;
}

void DeterministicPreemption::Arm(std::optional<s64> deadline_ns) {
    std::lock_guard lock(mutex_);
    deadline_ = deadline_ns;
    armed_at_epoch_ = epoch_ ? epoch_() : 0;
    // Serialize the immediate raise with Stop, which waits for any active raise.
    if (raise_ && deadline_ && *deadline_ <= time_.NowNs()) {
        raise_();
    }
}

std::optional<s64> DeterministicPreemption::Armed() const {
    std::lock_guard lock(mutex_);
    return epoch_ && epoch_() != armed_at_epoch_ ? std::nullopt : deadline_;
}

CycleDriver::CycleDriver(CodeHost& host, GuestScheduler& scheduler, SvcTable& svc,
                         VirtualTimeSource& time, DeterministicPreemption& preemption,
                         CycleDriverOptions options)
    : host_(host), scheduler_(scheduler), svc_(svc), time_(time), preemption_(preemption),
      options_(options), rng_state_(options.seed), last_slice_us_(options.slice_us) {}

CycleDriver::~CycleDriver() {
    if (installed_) {
        if (current_driver == this) {
            current_driver = nullptr;
        }
        time_.Lockstep().enabled = false;
        preemption_.SetEpochSource({});
        svc_.Install(host_);
        host_.SetBudgetHandler([scheduler = &scheduler_](RCtx&) { scheduler->Preempt(); });
    }
}

void CycleDriver::Install() {
    if (installed_) {
        return;
    }
    preemption_.SetEpochSource([this] { return scheduler_.Counters().preempt_checks; });
    host_.SetBudgetHandler([this](RCtx& ctx) { OnBudget(ctx); });
    host_.SetSvcHandler([this](RCtx& ctx, u32 imm, u32 next_pc) { OnSvc(ctx, imm, next_pc); });
    time_.Lockstep().enabled = true;
    current_driver = this;
    installed_ = true;

    // Keep the timing boundaries of the console's shared page and cheat engine.
    // Runtime owns the actual shared page updates.
    const u64 start = time_.CoreTicks(0);
    kernel_detail::ScheduleOnCore(0);
    ScheduleTimingMarker(scheduler_, time_, start, kSharedPagePeriod);
    kernel_detail::ScheduleOnCore(0);
    ScheduleTimingMarker(scheduler_, time_, start + kCheatPeriod, kCheatPeriod);
}

u64 CycleDriver::Next() {
    // SplitMix64, with its state advanced by one Weyl increment per draw.
    rng_state_ += 0x9e3779b97f4a7c15ull;
    u64 bits = rng_state_;
    bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ull;
    bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebull;
    return bits ^ (bits >> 31);
}

void CycleDriver::DrawSlice() {
    if (!options_.seeded) {
        return;
    }
    const u64 width = options_.slice_us;
    const u64 lower = std::max<u64>(1, width / 2);
    const u64 upper = std::clamp<u64>(width + width / 2, lower, std::numeric_limits<u32>::max());
    last_slice_us_ = static_cast<u32>(lower + Next() % (upper - lower + 1));
    scheduler_.SetSliceUs(last_slice_us_);
    ++draws_;
}

u64 CycleDriver::DrawSliceTicks() {
    if (!options_.seeded && options_.slice_us == kDeterministicSliceUs) {
        return kDeterministicSliceTicks;
    }
    return std::max<u64>(1, NsToTicks(static_cast<u64>(last_slice_us_) * 1000));
}

u32 CycleDriver::KeyOf(const RCtx& ctx) {
    return static_cast<const GuestThread*>(ctx.user)->Id();
}

void CycleDriver::BeginStretch(GuestThread& thread, s64 budget) {
    auto& ctx = thread.Context();
    ctx.budget = budget;
    given_[thread.Id()] = budget;
}

void CycleDriver::Sync(RCtx& ctx) {
    auto& given = given_[KeyOf(ctx)];
    if (given > ctx.budget) {
        Charge(static_cast<u64>(given) - static_cast<u64>(ctx.budget));
    }
    given = ctx.budget;
}

s64 CycleDriver::NextBudget() const {
    const u64 now = time_.Ticks();
    const auto& state = time_.Lockstep();
    if (state.in_stretch) {
        return state.stretch_end > now ? static_cast<s64>(state.stretch_end - now) : 0;
    }
    const auto deadline = preemption_.Armed();
    if (!deadline) {
        return static_cast<s64>(kMaxQuantumTicks);
    }
    const u64 due = VirtualTimeSource::TicksAtOrAfterNs(*deadline);
    return due > now ? static_cast<s64>(std::min(kMaxQuantumTicks, due - now)) : 0;
}

void CycleDriver::Refill(RCtx& ctx) {
    ctx.budget = NextBudget();
    given_[KeyOf(ctx)] = ctx.budget;
}

void CycleDriver::OnBudget(RCtx& ctx) {
    Sync(ctx);
    DrawSlice();
    scheduler_.Preempt();
    Refill(ctx);
}

void CycleDriver::OnSvc(RCtx& ctx, u32 imm, u32 next_pc) {
    Sync(ctx);
    DrawSlice();
    // The SVC table records the entry tick before charging the call.
    svc_.CallSvc(ctx, imm, next_pc);
    Refill(ctx);
}

namespace kernel_detail {

void ChargeCycles(u64 ticks) {
    if (current_driver) {
        current_driver->Charge(ticks);
    }
}

u64 TraceTicks(u64 running_core_ticks) {
    return current_driver ? current_driver->GlobalTicks() : running_core_ticks;
}

u64 CoreTicksOr(int core, u64 fallback) {
    return current_driver ? current_driver->Time().CoreTicks(core) : fallback;
}

void ScheduleOnCore(int core) {
    if (current_driver) {
        current_driver->Time().Lockstep().schedule_core = core;
    }
}

void BeginStretch(GuestThread& thread, s64 budget) {
    if (current_driver) {
        current_driver->BeginStretch(thread, budget);
    }
}

u64 DrawSliceTicks() {
    return current_driver ? current_driver->DrawSliceTicks() : kDeterministicSliceTicks;
}

bool ScheduleThreadSafe(int core, std::function<void()> fn) {
    if (!current_driver) {
        return false;
    }
    auto& time = current_driver->Time();
    auto& state = time.Lockstep();
    MH4N_ASSERT(state.scheduler_thread == std::this_thread::get_id());
    state.schedule_core = core;
    state.schedule_pending = true;
    current_driver->Scheduler().ScheduleAt(time.CoreTicks(core) + 2 * kDeterministicSliceTicks,
                                           std::move(fn));
    return true;
}

}  // namespace kernel_detail
}  // namespace mh4n
