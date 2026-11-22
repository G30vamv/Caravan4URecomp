// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the comments of mh4n/kernel.h (AddressArbiter, Thread::wake_hook/ArmTimeout), 3dbrew
// "SVC" (ArbitrateAddress, the five arbitration types) and the black-box specification of tests/kernel_objects_test.cpp.
// Deterministic timing from deterministic.h and determinism_test.cpp.
#include <algorithm>

#include "deterministic.h"
#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {

AddressArbiter::AddressArbiter(Kernel& kernel) : Object(kernel) {}

AddressArbiter::~AddressArbiter() {
    if (resource_limit != nullptr) {
        resource_limit->Release(ResourceLimitType::AddressArbiter, 1);
    }
}

Result AddressArbiter::ArbitrateAddress(std::shared_ptr<Thread> thread, ArbitrationType type, u32 address, s32 value,
                                        s64 timeout_ns) {
    GuestMemory& memory = kernel_.Memory();
    // the signed word at `address`; an unreadable address reads as 0
    const auto read_word = [&]() -> s32 { return static_cast<s32>(memory.Read<u32>(address).value_or(0)); };

    switch (type) {
    case ArbitrationType::Signal: {
        bool woke_thread = false;
        if (value < 0) {
            woke_thread = ResumeAllThreads(address) != 0;
        } else {
            for (s32 i = 0; i < value; ++i) {
                if (!ResumeHighestPriorityThread(address)) {
                    break;
                }
                woke_thread = true;
            }
        }
        if (!woke_thread && thread && thread->current_priority >= kArbiterIdleSignalMinPriority) {
            kernel_detail::ChargeCycles(kArbiterIdleSignalCycles);
        }
        break;
    }
    case ArbitrationType::WaitIfLessThan:
        if (read_word() < value) {
            WaitThread(std::move(thread), address, kResultSuccess, std::nullopt);
        }
        break;
    case ArbitrationType::WaitIfLessThanWithTimeout:
        if (read_word() < value) {
            WaitThread(std::move(thread), address, kResultTimeout, timeout_ns);
        }
        break;
    case ArbitrationType::DecrementAndWaitIfLessThan: {
        const s32 word = read_word();
        if (word < value) {
            // only a thread that waits changes the word
            memory.Write<u32>(address, static_cast<u32>(word - 1));
            WaitThread(std::move(thread), address, kResultSuccess, std::nullopt);
        }
        break;
    }
    case ArbitrationType::DecrementAndWaitIfLessThanWithTimeout: {
        const s32 word = read_word();
        if (word < value) {
            memory.Write<u32>(address, static_cast<u32>(word - 1));
            WaitThread(std::move(thread), address, kResultTimeout, timeout_ns);
        }
        break;
    }
    default:
        return kResultInvalidEnumValueFnd;
    }
    // the calls that have a timeout return the timeout result even when the thread did not wait
    if (type == ArbitrationType::WaitIfLessThanWithTimeout ||
        type == ArbitrationType::DecrementAndWaitIfLessThanWithTimeout) {
        return kResultTimeout;
    }
    return kResultSuccess;
}

void AddressArbiter::WaitThread(std::shared_ptr<Thread> thread, u32 address, Result wake_result,
                                std::optional<s64> timeout_ns) {
    thread->wait_address = address;
    thread->status = ThreadStatus::WaitArb;
    waiting_.push_back(thread);
    wake_results_.push_back(wake_result);
    // a timeout or the end of the thread (not a signal: that goes through ResumeHighestPriorityThread) takes it off the list
    std::weak_ptr<AddressArbiter> weak = std::static_pointer_cast<AddressArbiter>(shared_from_this());
    Thread* raw = thread.get();
    thread->wake_hook = [weak, raw](ThreadWakeupReason, WaitObject*, std::optional<Result>&, s32&) {
        if (const std::shared_ptr<AddressArbiter> arbiter = weak.lock()) {
            arbiter->RemoveThread(raw);
        }
    };
    if (timeout_ns.has_value()) {
        thread->ArmTimeout(*timeout_ns);
    }
}

bool AddressArbiter::ResumeHighestPriorityThread(u32 address) {
    // the best priority, the earliest in the list among equals
    std::size_t best = waiting_.size();
    for (std::size_t i = 0; i < waiting_.size(); ++i) {
        if (waiting_[i]->wait_address != address) {
            continue;
        }
        if (best == waiting_.size() || waiting_[i]->current_priority < waiting_[best]->current_priority) {
            best = i;
        }
    }
    if (best == waiting_.size()) {
        return false;
    }
    const std::shared_ptr<Thread> thread = waiting_[best];
    const Result result = wake_results_[best];
    waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(best));
    wake_results_.erase(wake_results_.begin() + static_cast<std::ptrdiff_t>(best));
    thread->ResumeFromWait(result);
    return true;
}

u64 AddressArbiter::ResumeAllThreads(u32 address) {
    // every thread waiting on the address, in arrival order (not by priority: that is what a negative count means)
    std::vector<std::shared_ptr<Thread>> woken;
    std::vector<Result> results;
    for (std::size_t i = 0; i < waiting_.size();) {
        if (waiting_[i]->wait_address == address) {
            woken.push_back(waiting_[i]);
            results.push_back(wake_results_[i]);
            waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(i));
            wake_results_.erase(wake_results_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (std::size_t i = 0; i < woken.size(); ++i) {
        woken[i]->ResumeFromWait(results[i]);
    }
    return woken.size();
}

void AddressArbiter::RemoveThread(Thread* thread) {
    for (std::size_t i = 0; i < waiting_.size(); ++i) {
        if (waiting_[i].get() == thread) {
            waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(i));
            wake_results_.erase(wake_results_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

}  // namespace mh4n
