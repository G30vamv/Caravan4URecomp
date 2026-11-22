// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: gsp.h, mh4n/pacing_trace.h, gsp_test, 3dbrew GSP Shared Memory and Memory layout, golden/ipc/boot-village.ipc.trace, and black-box GSP probes.

#include "gsp.h"

#include <algorithm>

#include "mh4n/assert.h"
#include "mh4n/log.h"
#include "mh4n/pacing_trace.h"
#include "mh4n/runtime.h"

namespace mh4n::gsp {
namespace {

constexpr char kLog[] = "gsp";
constexpr char kService[] = "gsp::Gpu";
constexpr u32 kGxHeaderSize = 0x20;
constexpr u32 kSelectFramebuffer = 1;
constexpr u8 kFramebufferDirty = 1;
constexpr std::array<u32, 5> kBytesPerPixel{4, 3, 2, 2, 2};
constexpr std::array<u32, 8> kSavedVramBanks{3, 4, 5, 7, 8, 9, 10, 11};
constexpr u32 kVramUnsaved = 0;
constexpr u32 kVramSaved = 1;

bool IsVBlank(gpu::Interrupt id) {
    return id == gpu::Interrupt::PDC0 || id == gpu::Interrupt::PDC1;
}

u32 VramBankAddress(u32 bank) {
    return kVramVaddr + bank * kVramBackupBankSize;
}

}  // namespace

u64 VBlankPeriodForRate(double hz, bool* clamped) {
    const double ticks = hz > 0.0 ? static_cast<double>(kTicksPerSecond) / hz : kFrameTicks;
    const double period = std::clamp(ticks, 1.0, static_cast<double>(kMaxVBlankPeriodTicks));
    if (clamped) {
        *clamped = ticks != period;
    }
    return static_cast<u64>(period);
}

GspState::GspState() = default;

GspState::~GspState() {
    if (vblank_event_) {
        kernel_->Cancel(vblank_event_);
    }
    if (started_) {
        pacing::g_next_vblank_ns.store(0, std::memory_order_relaxed);
        pacing::g_vblank_period_ns.store(0, std::memory_order_relaxed);
    }
}

std::shared_ptr<GspState> GspState::Get(ServiceContext& context) {
    auto state = context.State<GspState>(kService);
    if (!state->shared_memory_) {
        state->Init(context);
    }
    return state;
}

void GspState::Init(ServiceContext& context) {
    kernel_ = &context.GetKernel();
    memory_ = &context.Memory();
    interfaces_ = &context.Interfaces();
    const Result result = kernel_->NewSharedMemory(&shared_memory_, {}, kSharedMemorySize,
                                                   MemoryPermission::ReadWrite,
                                                   MemoryPermission::ReadWrite, 0,
                                                   FcramRegion::Base, "GSP shared memory");
    MH4N_ASSERT(result == kResultSuccess, "GSP shared memory allocation returned {:08x}", result);
}

u32 GspState::ClaimThread() {
    for (u32 thread = 0; thread < slots_.size(); ++thread) {
        if (!slots_[thread].used) {
            slots_[thread].used = true;
            return thread;
        }
    }
    return kNoThread;
}

void GspState::ReleaseThread(u32 thread) {
    ReleaseRight(thread);
    slots_[thread] = {};
}

void GspState::ReleaseRight(u32 thread) {
    if (thread_with_rights_ == thread) {
        thread_with_rights_ = kNoThread;
    }
}

bool GspState::TakeFirstInitialization() {
    return std::exchange(first_initialization_, false);
}

u8* GspState::SharedPointer(u32 offset, u32 size) {
    if (!shared_memory_ || offset >= shared_memory_->Size() ||
        size > shared_memory_->Size() - offset) {
        return nullptr;
    }
    return shared_memory_->GetPointer(offset);
}

InterruptRelayQueue* GspState::Queue(u32 thread) {
    if (thread >= kMaxThreads) {
        return nullptr;
    }
    return reinterpret_cast<InterruptRelayQueue*>(
        SharedPointer(thread * sizeof(InterruptRelayQueue), sizeof(InterruptRelayQueue)));
}

std::pair<bool, gpu::FrameBufferInfo*> GspState::GetFrameBufferInfo(u32 thread, u32 screen,
                                                                  bool force_update) {
    auto& cached = cached_framebuffer_infos_[screen];
    if (thread >= kMaxThreads) {
        return {false, &cached};
    }

    const u32 offset = kFrameBufferUpdateOffset + (2 * thread + screen) * sizeof(FrameBufferUpdate);
    u8* update = SharedPointer(offset, sizeof(FrameBufferUpdate));
    if (!update) {
        return {false, &cached};
    }
    const bool dirty = (update[offsetof(FrameBufferUpdate, is_dirty)] & kFramebufferDirty) != 0;
    if (dirty || force_update) {
        const u32 index = update[offsetof(FrameBufferUpdate, index)] & kSelectFramebuffer;
        std::memcpy(&cached, update + offsetof(FrameBufferUpdate, framebuffer_info) +
                        index * sizeof(cached), sizeof(cached));
        update[offsetof(FrameBufferUpdate, is_dirty)] &= static_cast<u8>(~kFramebufferDirty);
    }
    return {dirty, &cached};
}

void GspState::ProcessCommandQueue() {
    auto* gpu = GetGpu();
    if (!gpu || thread_with_rights_ >= kMaxThreads) {
        return;
    }
    const u32 offset = kCommandBufferOffset + thread_with_rights_ * kCommandBufferSize;
    u8* buffer = SharedPointer(offset, kCommandBufferSize);
    if (!buffer) {
        return;
    }

    u8& index = buffer[0];
    u8& pending = buffer[1];
    u8& status = buffer[2];
    const u8& should_stop = buffer[3];
    while (pending != 0 && (status & kCommandBufferStatusStopped) == 0) {
        if (should_stop != 0) {
            status |= kCommandBufferStatusStopped;
            break;
        }

        const u32 slot = index % kCommandBufferCommands;
        gpu::GxCommand command;
        std::memcpy(&command, buffer + kGxHeaderSize + slot * sizeof(command), sizeof(command));
        index = static_cast<u8>((slot + 1) % kCommandBufferCommands);
        --pending;
        if ((command.words[0] & 0x00FF0000) != 0) {
            status |= kCommandBufferStatusStopped;
        }
        pacing::Mark(pacing::kGxCommand, command.words[0] & 0xFF);
        gpu->Execute(command);
    }
}

void GspState::SignalInterrupt(gpu::Interrupt id, u64 delay_ns, bool apply_swap) {
    if (id > gpu::Interrupt::DMA) {
        return;
    }
    if (IsVBlank(id)) {
        for (u32 thread = 0; thread < kMaxThreads; ++thread) {
            SignalInterruptForThread(id, thread, TicksToNs(kFrameTicks), apply_swap);
        }
    } else if (thread_with_rights_ < kMaxThreads) {
        SignalInterruptForThread(id, thread_with_rights_, delay_ns, apply_swap);
    }
}

void GspState::SignalInterruptForThread(gpu::Interrupt id, u32 thread, u64 delay_ns, bool apply_swap) {
    PerfUpdate(id, delay_ns);
    auto& slot = slots_[thread];
    if (slot.registered) {
        ProcessInterrupt(id, thread, apply_swap);
    }
    if (id == gpu::Interrupt::PDC1 && !slot.vblank_waiters.empty()) {
        auto waiters = std::exchange(slot.vblank_waiters, {});
        for (const auto& waiter : waiters) {
            Reply reply(waiter.command_id, 1, 0);
            reply.Push(kResultSuccess);
            Complete(waiter.request, std::move(reply));
        }
    }
}

void GspState::ApplyVBlankSwap(gpu::Interrupt id, u32 thread) {
    const u32 screen = id == gpu::Interrupt::PDC0 ? 0 : 1;
    const auto [dirty, info] = GetFrameBufferInfo(thread, screen);
    if (auto* gpu = GetGpu(); gpu && (dirty || force_buffer_swap_[screen])) {
        gpu->SetBufferSwap(screen, *info);
        force_buffer_swap_[screen] = false;
        if (screen == 0) {
            pacing::Mark(pacing::kSwapApplied, pacing::g_game_frame.load(std::memory_order_relaxed));
            pacing::Mark(pacing::kSwapCount, ++top_swaps_);
        }
    }
}

void GspState::ProcessInterrupt(gpu::Interrupt id, u32 thread, bool apply_swap) {
    auto& slot = slots_[thread];
    if (IsVBlank(id) && apply_swap) {
        ApplyVBlankSwap(id, thread);
    }

    auto* queue = Queue(thread);
    if (!queue || !slot.interrupt_event) {
        return;
    }
    if (IsVBlank(id)) {
        if (queue->IgnorePdc()) {
            return;
        }
        if (queue->number_interrupts >= InterruptRelayQueue::kStopQueuingPdcThreshold) {
            auto& missed = id == gpu::Interrupt::PDC0 ? queue->missed_pdc0 : queue->missed_pdc1;
            ++missed;
            return;
        }
    }
    if (queue->number_interrupts >= InterruptRelayQueue::kMaxSlots) {
        queue->error_code = InterruptRelayQueue::kQueueFullError;
        return;
    }

    const u32 index = (queue->index + queue->number_interrupts) % InterruptRelayQueue::kMaxSlots;
    queue->slot[index] = static_cast<u8>(id);
    ++queue->number_interrupts;
    slot.interrupt_event->Signal();
}

void GspState::Start() {
    if (!started_) {
        started_ = true;
        next_vblank_tick_ = kernel_->NowTicks() + kFrameTicks;
        ScheduleVBlank();
    }
}

void GspState::SetRefreshRate(double hz) {
    bool clamped = false;
    const u64 period = VBlankPeriodForRate(hz, &clamped);
    if (clamped) {
        MH4N_LOG(Warning, kLog, "refresh rate {} Hz is outside the usable range", hz);
    }
    if (period != asked_period_ticks_) {
        MH4N_LOG(Info, kLog, "vblank period {} ticks", period);
        asked_period_ticks_ = period;
        period_ticks_ = period;
    }
}

void GspState::ScheduleVBlank() {
    vblank_event_ = kernel_->ScheduleAt(next_vblank_tick_, [this] { OnVBlank(); });
    PublishVBlank();
}

void GspState::OnVBlank() {
    ++vblank_count_;
    pacing::Mark(pacing::kVBlank, static_cast<u32>(vblank_count_));
    auto* gpu = GetGpu();
    // With the GPU on its own thread the game must not see a PDC before the GX commands it queued earlier are done
    // (a PDC may not overtake them): the buffer swaps are applied now, in their place in the queue, and the PDC is
    // delivered to the game once the queue reaches this point, before this tick's VBlanks are queued.
    const bool deferred = gpu && gpu->CanPostAfterQueued();
    if (deferred) {
        for (u32 thread = 0; thread < kMaxThreads; ++thread) {
            if (!slots_[thread].registered) continue;
            ApplyVBlankSwap(gpu::Interrupt::PDC0, thread);
            ApplyVBlankSwap(gpu::Interrupt::PDC1, thread);
        }
        gpu->PostAfterQueuedWork([this] {
            SignalInterrupt(gpu::Interrupt::PDC0, 0, false);
            SignalInterrupt(gpu::Interrupt::PDC1, 0, false);
        });
    } else {
        SignalInterrupt(gpu::Interrupt::PDC0);
        SignalInterrupt(gpu::Interrupt::PDC1);
    }
    if (gpu) {
        gpu->VBlank(0);
        gpu->VBlank(1);
    }

    FollowDisplay();
    ScheduleVBlank();
}

void GspState::FollowDisplay() {
    next_vblank_tick_ += period_ticks_;
    const s64 phase_ns = pacing::g_vblank_lock_ns.load(std::memory_order_relaxed);
    const u64 display_ns = pacing::g_display_period_ns.load(std::memory_order_relaxed);
    const u64 vsync_ns = pacing::g_display_vsync_ns.load(std::memory_order_relaxed);
    const bool have_display = phase_ns >= 0 && display_ns != 0 && vsync_ns != 0 &&
                              display_ns <= TicksToNs(kMaxVBlankPeriodTicks);
    const u64 display_ticks = have_display ? (NsToTicks(2 * display_ns) + 1) / 2 : 0;
    const u64 rate_difference = std::max(display_ticks, asked_period_ticks_) -
                                std::min(display_ticks, asked_period_ticks_);
    // An explicitly different rate must keep its own cadence, including max_refresh.
    if (!have_display || rate_difference > asked_period_ticks_ / 100) {
        period_ticks_ = asked_period_ticks_;
        return;
    }

    period_ticks_ = display_ticks;
    const auto host_deadline = kernel_->Clock().HostNsFor(next_vblank_tick_);
    if (!host_deadline || next_vblank_tick_ <= kernel_->NowTicks()) {
        return;
    }

    // Compare the phases of the two deadlines in the display's monotonic time domain.
    const s64 wait_ns = std::max<s64>(0, *host_deadline - kernel_->Clock().Source().NowNs());
    const u64 deadline_ns = pacing::NowNs() + static_cast<u64>(wait_ns);
    const auto display_period = static_cast<s64>(display_ns);
    const s64 wanted_phase = static_cast<s64>(vsync_ns % display_period) + phase_ns % display_period;
    s64 phase_error = (wanted_phase - static_cast<s64>(deadline_ns % display_period)) % display_period;
    if (phase_error > display_period / 2) {
        phase_error -= display_period;
    } else if (phase_error < -display_period / 2) {
        phase_error += display_period;
    }

    const s64 limit = display_period / 32;
    const s64 shift_ns = std::clamp(phase_error / 4, -limit, limit);
    const u64 advance = NsToTicks(static_cast<u64>(shift_ns < 0 ? -shift_ns : shift_ns));
    if (advance != 0) {
        if (shift_ns < 0) {
            next_vblank_tick_ = std::max(next_vblank_tick_ - advance, kernel_->NowTicks() + 1);
        } else {
            next_vblank_tick_ += advance;
        }
        const s64 trace_shift = std::clamp(shift_ns / 1000, s64{-1000000}, s64{1000000});
        pacing::Mark(pacing::kVBlankAdjust, static_cast<u32>(1000000 + trace_shift));
    }
}

void GspState::PublishVBlank() {
    const auto deadline = kernel_->Clock().HostNsFor(next_vblank_tick_);
    u64 next_ns = 0;
    if (deadline) {
        const s64 wait_ns = std::max<s64>(0, *deadline - kernel_->Clock().Source().NowNs());
        next_ns = pacing::NowNs() + static_cast<u64>(wait_ns);
    }
    pacing::g_vblank_period_ns.store(TicksToNs(period_ticks_), std::memory_order_relaxed);
    pacing::g_next_vblank_ns.store(next_ns, std::memory_order_relaxed);
}

void GspState::SetPerfLogEnabled(bool enabled) {
    perf_enabled_ = enabled;
    if (enabled) {
        perf_.fill(0);
    }
}

void GspState::PerfUpdate(gpu::Interrupt id, u64 nanoseconds) {
    if (perf_enabled_) {
        const u32 index = 2 * static_cast<u32>(id);
        const u32 delay = static_cast<u32>(nanoseconds);
        perf_[index] = delay;
        perf_[index + 1] += delay;
    }
}

void GspState::CopyFrameBuffer(u32 dst, u32 src, u32 dst_stride, u32 src_stride, u32 lines) {
    if (src_stride == 0 || dst_stride == 0) {
        return;
    }
    // The applet capture areas are sized for RGB8 or a narrower pixel format.
    if (src == 0 || dst_stride > kFramebufferWidth * 3) {
        ClearFramebuffer(dst, dst_stride, lines);
        return;
    }
    std::array<u8, kFramebufferWidth * kBytesPerPixel[0]> row;
    for (u32 line = 0; line < lines; ++line) {
        const u64 read_address = static_cast<u64>(src) + static_cast<u64>(line) * src_stride;
        if (read_address > UINT32_MAX ||
            !memory_->ReadBlock(row.data(), static_cast<u32>(read_address), dst_stride)) {
            row.fill(0);
        }
        memory_->WriteBlock(dst + line * dst_stride, row.data(), dst_stride);
    }
}

void GspState::ClearFramebuffer(u32 dst, u32 dst_stride, u32 lines) {
    const std::array<u8, kFramebufferWidth * kBytesPerPixel[0]> zero{};
    for (u32 line = 0; line < lines; ++line) {
        memory_->WriteBlock(dst + line * dst_stride, zero.data(), dst_stride);
    }
}

void GspState::SaveVramSysArea() {
    if (vram_state_ == kVramSaved) {
        return;
    }
    SaveVramWithState(kVramSaved);

    for (u32 screen = 0; screen < cached_framebuffer_infos_.size(); ++screen) {
        auto* info = GetFrameBufferInfo(thread_with_rights_, screen, true).second;
        const u32 format = info->format & 7;
        const bool top = screen == 0;
        const u32 stride = format < kBytesPerPixel.size() ? kFramebufferWidth * kBytesPerPixel[format] : 0;
        const u32 lines = top ? kTopFramebufferHeight : kBottomFramebufferHeight;
        const u32 left = top ? kFramebufferSaveAreaTopLeft : kFramebufferSaveAreaBottom;
        CopyFrameBuffer(left, info->address_left, stride, info->stride, lines);
        info->address_left = left;
        if (top) {
            CopyFrameBuffer(kFramebufferSaveAreaTopRight, info->address_right, stride, info->stride, lines);
            info->address_right = kFramebufferSaveAreaTopRight;
        }
        info->stride = stride;
        ForceBufferSwap(screen);
    }
}

void GspState::RestoreVramSysArea() {
    if (vram_state_ == kVramUnsaved) {
        return;
    }
    RestoreVramWithState(kVramUnsaved);
    for (u32 screen = 0; screen < cached_framebuffer_infos_.size(); ++screen) {
        GetFrameBufferInfo(thread_with_rights_, screen, true);
        ForceBufferSwap(screen);
    }
}

void GspState::SaveVramWithState(u32 state) {
    vram_backup_.resize(kVramBackupBanks);
    for (const u32 bank : kSavedVramBanks) {
        auto& bytes = vram_backup_[bank];
        bytes.resize(kVramBackupBankSize);
        if (!memory_->ReadBlock(bytes.data(), VramBankAddress(bank), kVramBackupBankSize)) {
            bytes.clear();
        }
    }
    vram_state_ = state;
}

void GspState::RestoreVramWithState(u32 state) {
    for (const u32 bank : kSavedVramBanks) {
        const auto& bytes = vram_backup_[bank];
        if (!bytes.empty()) {
            memory_->WriteBlock(VramBankAddress(bank), bytes.data(), kVramBackupBankSize);
        }
    }
    vram_state_ = state;
}

MH4N_REGISTER(BootStep, "services.ipc.gsp", BootStep{Phase::Services, [](Runtime& runtime) {
    auto* services = FindServiceManager(runtime);
    MH4N_ASSERT(services != nullptr);
    const auto state = GspState::Get(services->Context());
    state->Start();
    const std::weak_ptr<GspState> weak_state = state;
    auto& gpu_host = runtime.GpuHost();
    runtime.SetGpuInterruptSink([weak_state, &gpu_host](gpu::Interrupt id) {
        if (const auto active = weak_state.lock()) {
            active->SignalInterrupt(id, gpu_host.InterruptDelayNs());
        }
    });
    runtime.SetRefreshRateSink([weak_state](double hz) {
        if (const auto active = weak_state.lock()) {
            active->SetRefreshRate(hz);
        }
    });
}});

}  // namespace mh4n::gsp
