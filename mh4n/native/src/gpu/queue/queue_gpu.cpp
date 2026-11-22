// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the QueueGpu and QueueHost contracts, gpu_queue_test, and WP68 GSP state.

#include "queue_gpu.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

#include "gpu/backend/rhi.h"
#include "mh4n/host_thread_name.h"
#include "mh4n/log.h"
#include "mh4n/pacing_trace.h"

namespace mh4n::gpu::queue {
namespace {

constexpr char kLog[] = "GPU.Queue";
using Clock = std::chrono::steady_clock;

std::mutex start_hook_mutex;
std::function<void()> start_hook;

QueueConfig Normalize(QueueConfig config) {
    config.max_frames_in_flight =
        std::clamp(config.max_frames_in_flight, 1u, std::numeric_limits<u32>::max() - 1);
    config.max_frames_queued = std::max(config.max_frames_queued, config.max_frames_in_flight + 1);
    return config;
}

u64 Elapsed(Clock::time_point from) {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - from).count());
}

void RecordWait(std::atomic<u64>& wait_ns, Clock::time_point from) {
    const u64 ns = Elapsed(from);
    wait_ns.fetch_add(ns, std::memory_order_relaxed);
    pacing::Mark(pacing::kQueueWait, static_cast<u32>(ns / 1000));
}

} // namespace

u8* QueueHost::GuestPointer(u32 vaddr, u32 size) {
    // DMA may refer to an application mapping with no physical address.
    return real_.GuestPointer(vaddr, size);
}

void QueueHost::Run(Task& task) {
    if (!task.taken.exchange(true, std::memory_order_relaxed)) {
        auto work = std::move(task.work);
        work();
    }
}

void QueueHost::Trim() {
    while (!pending_.empty() && pending_.front()->taken.load(std::memory_order_relaxed)) {
        pending_.pop_front();
        ++base_;
    }
}

void QueueHost::Post(std::function<void()> work) {
    auto task = std::make_shared<Task>();
    task->work = std::move(work);
    {
        std::lock_guard lock(mutex_);
        Trim();
        pending_.push_back(task);
        ++posted_;
    }
    // Some hosts run Post inline. Release the list mutex before calling the host.
    real_.Post([task = std::move(task)] { Run(*task); });
}

void QueueHost::PostCompletion(std::function<void()> work) {
    real_.Post(std::move(work));
}

u64 QueueHost::PostedMark() const {
    std::lock_guard lock(mutex_);
    return posted_;
}

void QueueHost::RunPostedUpTo(u64 mark) {
    std::vector<std::shared_ptr<Task>> work;
    {
        std::lock_guard lock(mutex_);
        Trim();
        const u64 end = std::min(mark, posted_);
        for (u64 i = base_; i < end; ++i) {
            work.push_back(pending_[static_cast<size_t>(i - base_)]);
        }
    }
    for (const auto& task : work) {
        Run(*task);
    }
    std::lock_guard lock(mutex_);
    Trim();
}

size_t QueueHost::PendingCount() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}

void SetRendererThreadStartHook(std::function<void()> hook) {
    std::lock_guard lock(start_hook_mutex);
    start_hook = std::move(hook);
}

QueueGpu::QueueGpu(Host& host, const InnerMaker& make_inner, QueueConfig config)
    : config_(Normalize(config)), proxy_(std::make_unique<QueueHost>(host)) {
    Start([&] { inner_ = make_inner(*proxy_); });
}

QueueGpu::QueueGpu(std::unique_ptr<Gpu> inner, QueueConfig config)
    : config_(Normalize(config)), inner_(std::move(inner)) {
    Start([] {});
}

QueueGpu::~QueueGpu() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    work_cv_.notify_one();
    thread_.join();
}

void QueueGpu::Start(std::function<void()> make_inner) {
    thread_ = std::thread([this, make_inner = std::move(make_inner)] {
        renderer_id_ = std::this_thread::get_id();
        try {
            SetHostThreadName("mh4n-render");
            std::function<void()> hook;
            {
                std::lock_guard lock(start_hook_mutex);
                hook = start_hook;
            }
            if (hook) {
                hook();
            }
            make_inner();
            if (!inner_) {
                throw std::invalid_argument("GPU queue requires a wrapped GPU");
            }
            inner_debug_ = dynamic_cast<GpuDebug*>(inner_.get());
        } catch (...) {
            start_error_ = std::current_exception();
            inner_.reset();
        }
        {
            std::lock_guard lock(mutex_);
            started_ = true;
        }
        done_cv_.notify_all();
        if (!start_error_) {
            RendererMain();
            inner_debug_ = nullptr;
            inner_.reset();
        }
    });

    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [this] { return started_; });
    if (start_error_) {
        lock.unlock();
        thread_.join();
        std::rethrow_exception(start_error_);
    }
}

void QueueGpu::RendererMain() {
    for (;;) {
        std::function<void()> item;
        {
            std::unique_lock lock(mutex_);
            work_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                return;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }

        const auto start = Clock::now();
        try {
            item();
        } catch (const std::exception& error) {
            MH4N_LOG(Error, kLog, "Renderer call failed: {}", error.what());
        } catch (...) {
            MH4N_LOG(Error, kLog, "Renderer call failed with an unknown exception");
        }
        busy_ns_.fetch_add(Elapsed(start), std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex_);
            ++completed_;
        }
        done_cv_.notify_all();
    }
}

u64 QueueGpu::Enqueue(std::function<void()> item, bool frame_end, bool limit_next_frame) {
    if (frame_end) {
        item = [this, frame = std::move(item)] {
            std::exception_ptr error;
            try {
                frame();
            } catch (...) {
                error = std::current_exception();
            }
            {
                std::lock_guard lock(mutex_);
                --frames_in_flight_;
            }
            done_cv_.notify_all();
            if (error) {
                std::rethrow_exception(error);
            }
        };
    }

    std::unique_lock lock(mutex_);
    if (frame_end && frames_in_flight_ >= config_.max_frames_in_flight) {
        frame_waits_.fetch_add(1, std::memory_order_relaxed);
        WaitForFrameSlot(lock);
    }
    queue_.push_back(std::move(item));
    frames_in_flight_ += static_cast<u32>(frame_end);
    const u64 seq = ++enqueued_;
    if (frame_end && limit_next_frame && frames_in_flight_ >= config_.max_frames_queued) {
        work_cv_.notify_one();
        WaitForFrameSlot(lock);
        return seq;
    }
    lock.unlock();
    work_cv_.notify_one();
    return seq;
}

void QueueGpu::WaitForFrameSlot(std::unique_lock<std::mutex>& lock) {
    if (frames_in_flight_ < config_.max_frames_queued) return;
    const auto start = Clock::now();
    ++blocked_;
    done_cv_.wait(lock, [this] { return frames_in_flight_ < config_.max_frames_queued; });
    --blocked_;
    RecordWait(wait_ns_, start);
}

void QueueGpu::WaitFor(u64 seq, bool count) {
    std::unique_lock lock(mutex_);
    if (completed_ >= seq) {
        return;
    }
    const auto start = Clock::now();
    ++blocked_;
    done_cv_.wait(lock, [this, seq] { return completed_ >= seq; });
    --blocked_;
    if (count) {
        RecordWait(wait_ns_, start);
    }
}

void QueueGpu::WaitIdle() {
    u64 mark;
    {
        std::lock_guard lock(mutex_);
        mark = enqueued_;
    }
    WaitFor(mark, false);
}

void QueueGpu::PostAfterQueued(std::function<void()> work) {
    if (!proxy_) throw std::logic_error("GPU queue has no scheduler host");
    Finished(Enqueue([this, work = std::move(work)]() mutable { proxy_->PostCompletion(std::move(work)); }));
}

void QueueGpu::RunSync(const std::function<void()>& fn, bool count) {
    std::exception_ptr error;
    const u64 seq = Enqueue([&] {
        try {
            fn();
        } catch (...) {
            error = std::current_exception();
        }
    });
    WaitFor(seq, count);
    if (error) {
        std::rethrow_exception(error);
    }
}

void QueueGpu::Finished(u64 seq) {
    if (config_.lockstep) {
        WaitFor(seq);
    }
}

u32 QueueGpu::BlockedCallers() const {
    std::lock_guard lock(mutex_);
    return blocked_;
}

Gpu& QueueGpu::ForwardTarget(const char* call) {
#if MH4N_RHI_CHECKS
    if (std::this_thread::get_id() != renderer_id_) {
        rhi::ReportOwnerRuleBreach(rhi::OwnerRule::WrongThread,
                                  fmt::format("GPU queue forwarded {} off the renderer thread", call));
    }
#else
    (void)call;
#endif
    return *inner_;
}

void QueueGpu::Execute(const GxCommand& command) {
    Finished(Enqueue([this, command] { ForwardTarget("Execute").Execute(command); }));
}

void QueueGpu::WriteReg(u32 addr, u32 value) {
    u64 seq;
    {
        std::lock_guard lock(regs_mutex_);
        seq = Enqueue([this, addr, value] { ForwardTarget("WriteReg").WriteReg(addr, value); });
        regs_[addr] = value;
    }
    Finished(seq);
}

u32 QueueGpu::ReadReg(u32 addr) {
    {
        std::lock_guard lock(regs_mutex_);
        if (const auto it = regs_.find(addr); it != regs_.end()) {
            return it->second;
        }
    }
    u32 value = 0;
    RunSync([&] { value = ForwardTarget("ReadReg").ReadReg(addr); }, true);
    return value;
}

void QueueGpu::SetBufferSwap(u32 screen, const FrameBufferInfo& info) {
    Finished(Enqueue([this, screen, info] {
        ForwardTarget("SetBufferSwap").SetBufferSwap(screen, info);
    }));
}

void QueueGpu::SetColorFill(u32 fill) {
    Finished(Enqueue([this, fill] { ForwardTarget("SetColorFill").SetColorFill(fill); }));
}

void QueueGpu::FlushRegion(u32 vaddr, u32 size) {
    Finished(Enqueue([this, vaddr, size] {
        ForwardTarget("FlushRegion").FlushRegion(vaddr, size);
    }));
}

void QueueGpu::InvalidateRegion(u32 vaddr, u32 size) {
    u64 mark = 0;
    const u64 seq = Enqueue([&] {
        try {
            ForwardTarget("InvalidateRegion").InvalidateRegion(vaddr, size);
        } catch (...) {
            if (proxy_) {
                mark = proxy_->PostedMark();
            }
            throw;
        }
        if (proxy_) {
            mark = proxy_->PostedMark();
        }
    });
    WaitFor(seq);
    if (proxy_) {
        proxy_->RunPostedUpTo(mark);
    }
}

void QueueGpu::VBlank(u32 screen) {
    EnqueueVBlank(screen, [screen](Gpu& target) { target.VBlank(screen); });
}

void QueueGpu::EnqueueVBlank(u32 screen, std::function<void(Gpu&)> vblank, bool limit_next_frame) {
    Finished(Enqueue([this, screen, vblank = std::move(vblank)] {
        vblank(ForwardTarget("VBlank"));
        if (screen == 0 && !config_.lockstep) {
            CollectFrameStats();
        }
    }, screen == 0, limit_next_frame));
}

void QueueGpu::LoadInitState(const InitState& state) {
    if (Debug()) {
        RunSync([&] {
            ForwardTarget("LoadInitState");
            Debug()->LoadInitState(state);
        }, config_.lockstep);
        std::lock_guard lock(regs_mutex_);
        regs_.clear();
    }
}

std::optional<std::vector<RegisterWrite>> QueueGpu::TakeRegisterWrites() {
    std::optional<std::vector<RegisterWrite>> value;
    if (Debug()) {
        RunSync([&] {
            ForwardTarget("TakeRegisterWrites");
            value = Debug()->TakeRegisterWrites();
        }, config_.lockstep);
    }
    return value;
}

void QueueGpu::WriteBackColorBuffers() {
    if (Debug()) {
        u64 mark = 0;
        RunSync([&] {
            ForwardTarget("WriteBackColorBuffers");
            Debug()->WriteBackColorBuffers();
            if (proxy_) mark = proxy_->PostedMark();
        }, config_.lockstep);
        if (proxy_) proxy_->RunPostedUpTo(mark);
    }
}

void QueueGpu::CollectFrameStats() {
    if (!Debug()) {
        return;
    }
    ForwardTarget("TakeFrameCounters");
    auto counters = Debug()->TakeFrameCounters();
    ForwardTarget("TakeFrameGpuTimeNs");
    const auto gpu_time = Debug()->TakeFrameGpuTimeNs();
    std::lock_guard lock(stash_mutex_);
    for (auto& counter : counters) {
        const auto existing = std::find_if(stash_counters_.begin(), stash_counters_.end(),
                                           [&](const Counter& c) { return c.name == counter.name; });
        if (existing == stash_counters_.end()) {
            stash_counters_.push_back(std::move(counter));
        } else if (counter.name.starts_with("vma_")) {
            existing->value = counter.value;
        } else {
            existing->value += counter.value;
        }
    }
    if (gpu_time) {
        stash_gpu_ns_ = stash_gpu_ns_.value_or(0) + *gpu_time;
    }
}

std::vector<Counter> QueueGpu::TakeFrameCounters() {
    std::vector<Counter> counters;
    if (config_.lockstep) {
        if (Debug()) {
            RunSync([&] {
                ForwardTarget("TakeFrameCounters");
                counters = Debug()->TakeFrameCounters();
            }, true);
        }
    } else {
        std::lock_guard lock(stash_mutex_);
        counters.swap(stash_counters_);
    }
    counters.push_back({"translate_us", busy_ns_.exchange(0, std::memory_order_relaxed) / 1000});
    counters.push_back({"queue_wait_us", wait_ns_.exchange(0, std::memory_order_relaxed) / 1000});
    counters.push_back({"queue_frame_waits", frame_waits_.exchange(0, std::memory_order_relaxed)});
    return counters;
}

std::optional<u64> QueueGpu::TakeFrameGpuTimeNs() {
    std::optional<u64> value;
    if (config_.lockstep) {
        if (Debug()) {
            RunSync([&] {
                ForwardTarget("TakeFrameGpuTimeNs");
                value = Debug()->TakeFrameGpuTimeNs();
            }, true);
        }
    } else {
        std::lock_guard lock(stash_mutex_);
        value = std::exchange(stash_gpu_ns_, std::nullopt);
    }
    return value;
}

} // namespace mh4n::gpu::queue
