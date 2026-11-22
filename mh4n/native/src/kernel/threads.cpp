// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: threads.h, kernel.h, threads_test.cpp, kernel_usage.md and 3dbrew Memory layout.

#include "mh4n/threads.h"

#include "cpu_limiter.h"
#include "mh4n/assert.h"

extern "C" {
#include "recomp.h"
}

namespace mh4n {

Core1CpuSetting ComputeAppMaxCpuSetting(u64 program_id, u32 exh_mode, u32 exh_cpu_limit) {
    const auto mode = exh_mode == 0 ? Core1ScheduleMode::Single : Core1ScheduleMode::Multi;
    if (exh_mode != 0 || exh_cpu_limit != 0) {
        return {mode, static_cast<s32>(exh_cpu_limit), 0};
    }
    switch ((program_id >> 8) & 0xFFFFF) {
    case 0x205:
    case 0x491:
    case 0x562:
        return {mode, kCore1CpuTimeExempted, kCore1CpuTimeExempted};
    case 0x334:
        return {mode, 30, 30};
    default:
        return {mode, 80, 25};
    }
}

ThreadManager::ThreadManager(Kernel& kernel, GuestScheduler& scheduler)
    : kernel_(kernel), scheduler_(scheduler), limiter_(std::make_unique<Core1Limiter>(kernel, scheduler)) {}

ThreadManager::~ThreadManager() = default;

Result ThreadManager::CreateMainThread(const ProcessImage& image, std::shared_ptr<Thread>* out,
                                       GuestThread::Entry host_entry) {
    const u32 priority = static_cast<u32>(image.main_thread_priority);
    if (priority > kThreadPrioLowest) {
        return kResultOutOfRange;
    }
    if (image.ideal_processor > kThreadProcessorIdMax) {
        return kResultOutOfRangeKernel;
    }
    if (image.ideal_processor > kThreadProcessorId1) {
        return kResultOutOfRange;
    }

    auto process = kernel_.CurrentProcess();
    if (!process) {
        process = kernel_.NewProcess(image.name);
    }
    process->kernel_version = static_cast<u16>(image.kernel_version);
    process->ideal_processor = static_cast<u8>(image.ideal_processor);
    process->memory_region = image.memory_region;
    if (image.apply_app_max_cpu &&
        process->resource_limit->GetCategory() == ResourceLimitCategory::Application) {
        ApplyAppMaxCpuSetting(image.program_id, image.core1_schedule_mode, image.core1_max_cpu);
    }
    if (!process->resource_limit->Reserve(ResourceLimitType::Thread, 1)) {
        return kResultOutOfThreads;
    }
    const Result result = MakeThread(process->Name() + "-main", image.entry, priority, 0,
                                     process->ideal_processor, image.stack_top,
                                     image.main_thread_fpscr, process, std::move(host_entry), out);
    if (IsSuccess(result)) {
        process->status = ProcessStatus::Running;
    }
    return result;
}

Result ThreadManager::CreateThread(const ThreadSpec& spec, Handle* out_handle,
                                   std::shared_ptr<Thread>* out_thread) {
    if (spec.priority > kThreadPrioLowest) {
        return kResultOutOfRange;
    }
    const auto process = kernel_.CurrentProcess();
    MH4N_ASSERT(process && process->resource_limit);
    auto& limit = *process->resource_limit;
    if (!no_thread_restrictions_ &&
        static_cast<s32>(spec.priority) < limit.GetLimitValue(ResourceLimitType::Priority)) {
        return kResultNotAuthorized;
    }

    s32 core = spec.processor_id;
    if (core == kThreadProcessorIdDefault) {
        core = process->ideal_processor;
    } else if (core == kThreadProcessorIdAll) {
        core = kThreadProcessorId0;
    }
    if (core < kThreadProcessorId0 || core > kThreadProcessorId1) {
        return kResultOutOfRange;
    }
    if (!limit.Reserve(ResourceLimitType::Thread, 1)) {
        return kResultOutOfThreads;
    }

    std::shared_ptr<Thread> thread;
    const Result result = MakeThread("thread", spec.entry_point, spec.priority, spec.arg, core,
                                     spec.stack_top, kThreadFpscr, process, spec.host_entry, &thread);
    if (IsError(result)) {
        return result;
    }
    if (out_thread) {
        *out_thread = thread;
    }
    return kernel_.AddHandle(out_handle, std::move(thread));
}

Result ThreadManager::MakeThread(std::string name, u32 entry_point, u32 priority, u32 arg,
                                 s32 processor_id, u32 stack_top, u32 fpscr,
                                 const std::shared_ptr<Process>& process, GuestThread::Entry host_entry,
                                 std::shared_ptr<Thread>* out) {
    if (!kernel_.Memory().IsMapped(entry_point & ~1u, 1)) {
        return kResultInvalidEntryAddress;
    }
    auto thread = kernel_.NewThread(std::move(name), priority, processor_id, process);
    const Result result = AllocateThreadLocalStorage(process, thread);
    if (IsError(result)) {
        return result;
    }

    thread->entry_point = entry_point;
    thread->stack_top = stack_top;
    auto& context = scheduler_.Attach(thread, std::move(host_entry)).Context();
    context.r[0] = arg;
    context.r[13] = stack_top;
    context.r[15] = entry_point;
    context.cpsr_misc = 0x10 | ((entry_point & 1u) << 5);
    context.fpscr = fpscr;
    context.tpidruro = thread->tls_address;
    scheduler_.Start(thread);
    scheduler_.PrepareReschedule();
    if (out) {
        *out = std::move(thread);
    }
    return kResultSuccess;
}

bool ThreadManager::TlsSlotInUse(u32 page, u32 slot) const {
    if (page >= tls_pages_.size() || slot >= kTlsSlotsPerPage) {
        return false;
    }
    const auto thread = tls_pages_[page].owner[slot].lock();
    return thread && thread->status != ThreadStatus::Dead;
}

Result ThreadManager::AllocateThreadLocalStorage(const std::shared_ptr<Process>& process,
                                                 const std::shared_ptr<Thread>& thread) {
    u32 page = 0;
    u32 slot = 0;
    for (; page < tls_pages_.size(); ++page) {
        for (slot = 0; slot < kTlsSlotsPerPage; ++slot) {
            if (!TlsSlotInUse(page, slot)) {
                break;
            }
        }
        if (slot < kTlsSlotsPerPage) {
            break;
        }
    }

    auto& memory = kernel_.Memory();
    if (page == tls_pages_.size()) {
        auto& regions = kernel_.Regions();
        const auto offset = regions.LinearAllocate(FcramRegion::Base, kPageSize);
        if (!offset) {
            return kResultOutOfMemory;
        }
        const Result mapped = memory.Map(TlsSlotAddress(page, 0), kPageSize, MemPerm::ReadWrite,
                                         MemState::Locked, kFcramPaddr + *offset);
        if (IsError(mapped)) {
            regions.Free(FcramRegion::Base, *offset, kPageSize);
            return mapped;
        }
        tls_pages_.emplace_back();
        process->memory_used += kPageSize;
        slot = 0;
    }

    const u32 address = TlsSlotAddress(page, slot);
    constexpr std::array<u8, kTlsEntrySize> zero{};
    MH4N_ASSERT(memory.WriteBlock(address, zero.data(), kTlsEntrySize));
    tls_pages_[page].owner[slot] = thread;
    thread->tls_address = address;
    return kResultSuccess;
}

void ThreadManager::ApplyAppMaxCpuSetting(u64 program_id, u32 exh_mode, u32 exh_cpu_limit) {
    const auto setting = ComputeAppMaxCpuSetting(program_id, exh_mode, exh_cpu_limit);
    auto& limit = *kernel_.ApplicationResourceLimit();
    limit.SetLimitValue(ResourceLimitType::CpuTime, setting.limit_value);
    limit.SetCurrentValue(ResourceLimitType::CpuTime, setting.current_value);
    limiter_->SetScheduleMode(setting.mode);
    limiter_->UpdateAppCpuLimit();
}

Result ThreadManager::SetAppCpuTimeLimit(u32 percent) {
    auto& limit = *kernel_.ApplicationResourceLimit();
    if (percent <= static_cast<u32>(limit.GetLimitValue(ResourceLimitType::CpuTime))) {
        limit.SetCurrentValue(ResourceLimitType::CpuTime, static_cast<s32>(percent));
        limiter_->UpdateAppCpuLimit();
    }
    return kResultSuccess;
}

u32 ThreadManager::AppCpuTimeLimit() const {
    return static_cast<u32>(kernel_.ApplicationResourceLimit()->GetCurrentValue(ResourceLimitType::CpuTime));
}

Core1ScheduleMode ThreadManager::ScheduleMode() const {
    return limiter_->ScheduleMode();
}

bool ThreadManager::Core1LimiterReady() const {
    return limiter_->Ready();
}

bool ThreadManager::Core1LimiterActive() const {
    return limiter_->Active();
}

Core1Phase ThreadManager::Core1CurrentPhase() const {
    return limiter_->Phase();
}

std::size_t ThreadManager::Core1HeldThreads() const {
    return limiter_->Held();
}

}  // namespace mh4n
