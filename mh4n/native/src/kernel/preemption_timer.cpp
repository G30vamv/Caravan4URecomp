// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: scheduler.h, clock.h and scheduler_test.cpp.

#include "mh4n/scheduler.h"

namespace mh4n {
namespace {

class TimerPreemption final : public PreemptionSource {
public:
    explicit TimerPreemption(TimeSource& source) : source_(source) {}
    ~TimerPreemption() override { Stop(); }

    void Start(std::function<void()> raise) override {
        std::lock_guard lock(mutex_);
        raise_ = std::move(raise);
        worker_ = std::thread([this] { Run(); });
    }

    void Stop() override {
        {
            std::unique_lock lock(mutex_);
            raise_ = {};
            deadline_.reset();
            changed_.notify_all();
            finished_.wait(lock, [this] { return immediate_ == 0; });
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void Arm(std::optional<s64> deadline_ns) override {
        std::function<void()> raise_now;
        {
            std::lock_guard lock(mutex_);
            if (!raise_) {
                return;
            }
            if (deadline_ns && *deadline_ns <= source_.NowNs()) {
                raise_now = raise_;
                ++immediate_;
                deadline_.reset();
            } else {
                deadline_ = deadline_ns;
            }
            ++generation_;
        }
        changed_.notify_one();
        if (raise_now) {
            raise_now();
            std::lock_guard lock(mutex_);
            --immediate_;
            finished_.notify_all();
        }
    }

private:
    void Run() {
        std::unique_lock lock(mutex_);
        while (raise_) {
            const u64 generation = generation_;
            const auto deadline = deadline_;
            const bool revised = source_.WaitUntil(changed_, lock, deadline, [&] {
                return !raise_ || generation_ != generation;
            });
            if (!revised) {
                deadline_.reset();
                const auto raise = raise_;
                lock.unlock();
                raise();
                lock.lock();
            }
        }
    }

    TimeSource& source_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::condition_variable finished_;
    std::function<void()> raise_;
    std::optional<s64> deadline_;
    u64 generation_ = 0;
    std::size_t immediate_ = 0;
    std::thread worker_;
};

}  // namespace

std::unique_ptr<PreemptionSource> MakeTimerThreadPreemption(TimeSource& source) {
    return std::make_unique<TimerPreemption>(source);
}

}  // namespace mh4n
