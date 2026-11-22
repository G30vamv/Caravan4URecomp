// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: clock.h, scheduler_test.cpp, determinism_test.cpp and kernel_usage.md.

#include "mh4n/clock.h"

#include <algorithm>
#include <limits>

#include "deterministic.h"

namespace mh4n {
namespace {

s64 AddHostTime(s64 base, u64 delta) {
    const u64 room = static_cast<u64>(std::numeric_limits<s64>::max()) - static_cast<u64>(base);
    if (delta > room) {
        return std::numeric_limits<s64>::max();
    }
    return static_cast<s64>(static_cast<u64>(base) + delta);
}

}  // namespace

s64 SteadyTimeSource::NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - epoch_).count();
}

bool SteadyTimeSource::WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                                 std::optional<s64> deadline_ns, const std::function<bool()>& pred) {
    if (deadline_ns) {
        const auto room = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::time_point::max() - epoch_).count();
        const auto deadline = epoch_ + std::chrono::nanoseconds(std::clamp(*deadline_ns, s64{0}, room));
        return cv.wait_until(lock, deadline, pred);
    }
    cv.wait(lock, pred);
    return true;
}

bool ManualTimeSource::WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                                 std::optional<s64> deadline_ns, const std::function<bool()>& pred) {
    if (!deadline_ns) {
        cv.wait(lock, pred);
        return true;
    }
    if (!pred() && NowNs() < *deadline_ns) {
        lock.unlock();
        Set(*deadline_ns);
        lock.lock();
    }
    return pred();
}

void ManualTimeSource::Advance(s64 ns) {
    Set(AddHostTime(NowNs(), static_cast<u64>(std::max(ns, s64{0}))));
}

void ManualTimeSource::Set(s64 ns) {
    s64 previous = now_.load(std::memory_order_relaxed);
    while (previous < ns &&
           !now_.compare_exchange_weak(previous, ns, std::memory_order_release, std::memory_order_relaxed)) {
    }
    if (hook_) {
        hook_();
    }
}

GameClock::GameClock(TimeSource& source)
    : source_(source), base_host_ns_(dynamic_cast<VirtualTimeSource*>(&source) ? 0 : source.NowNs()) {}

u64 GameClock::NowLocked() const {
    u64 tick = base_ticks_;
    if (running_) {
        const s64 now = source_.NowNs();
        if (now > base_host_ns_) {
            tick += NsToTicks(static_cast<u64>(now) - static_cast<u64>(base_host_ns_));
        }
    }
    return limit_ ? std::min(tick, *limit_) : tick;
}

u64 GameClock::Now() const {
    std::lock_guard lock(mutex_);
    return NowLocked();
}

void GameClock::Snapshot() {
    const s64 now = source_.NowNs();
    if (running_ && now > base_host_ns_) {
        base_ticks_ += NsToTicks(static_cast<u64>(now) - static_cast<u64>(base_host_ns_));
        if (limit_) {
            base_ticks_ = std::min(base_ticks_, *limit_);
        }
    }
    base_host_ns_ = now;
}

void GameClock::Pause() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        Snapshot();
        running_ = false;
    }
    Notify();
}

void GameClock::Resume() {
    {
        std::lock_guard lock(mutex_);
        if (running_) {
            return;
        }
        Snapshot();
        running_ = true;
    }
    Notify();
}

bool GameClock::Paused() const {
    std::lock_guard lock(mutex_);
    return !running_;
}

void GameClock::Hold(u64 until_tick) {
    {
        std::lock_guard lock(mutex_);
        const u64 limit = std::max(until_tick, NowLocked());
        if (limit_ == limit) {
            return;
        }
        if (limit_ && NowLocked() == *limit_) {
            Snapshot();
        }
        limit_ = limit;
    }
    Notify();
}

void GameClock::ReleaseHold() {
    {
        std::lock_guard lock(mutex_);
        if (!limit_) {
            return;
        }
        if (NowLocked() == *limit_) {
            Snapshot();
        }
        limit_.reset();
    }
    Notify();
}

std::optional<s64> GameClock::HostNsFor(u64 tick) const {
    std::lock_guard lock(mutex_);
    if (tick <= NowLocked()) {
        return source_.NowNs();
    }
    if (!running_ || (limit_ && tick > *limit_)) {
        return std::nullopt;
    }
    return AddHostTime(base_host_ns_, TicksToNsCeil(tick - base_ticks_));
}

std::optional<u64> GameClock::HeldAt() const {
    std::lock_guard lock(mutex_);
    return limit_;
}

void GameClock::SetChangeCallback(std::function<void()> callback) {
    std::lock_guard lock(mutex_);
    on_change_ = std::move(callback);
}

void GameClock::Notify() {
    std::function<void()> callback;
    {
        std::lock_guard lock(mutex_);
        callback = on_change_;
    }
    if (callback) {
        callback();
    }
}

}  // namespace mh4n
