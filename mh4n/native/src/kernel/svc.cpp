// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: kernel contract headers, svc_test.cpp, libctru's SVC ABI, and boot-village.svc.trace via spec/kernel_usage.md.
// Deterministic timing from deterministic.h and determinism_test.cpp.

#include "deterministic.h"
#include "svc_internal.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "mh4n/assert.h"
#include "mh4n/log.h"
#include "../cpu/recomp_c.h"

namespace mh4n {
namespace {

constexpr char kLog[] = "Kernel.SVC";
constexpr char kDebugLog[] = "Debug.Emulated";

void ReturnValue(RCtx& ctx, Result result, u32 value = 0) {
    ctx.r[0] = result;
    ctx.r[1] = value;
}

void ReturnInfo(RCtx& ctx, Result result, u64 value = 0) {
    ctx.r[0] = result;
    SvcPut64(ctx, 1, 2, value);
}

template <class T>
void ReturnHandle(RCtx& ctx, Kernel& kernel, std::shared_ptr<T> object) {
    Handle handle = 0;
    const Result result = kernel.AddHandle(&handle, std::move(object));
    ReturnValue(ctx, result, handle);
}

template <class T>
void ApplyToObject(RCtx& ctx, Kernel& kernel, void (T::*action)()) {
    const auto object = kernel.GetObject<T>(ctx.r[0]);
    if (!object) {
        ctx.r[0] = kResultInvalidHandle;
        return;
    }
    (object.get()->*action)();
    ctx.r[0] = kResultSuccess;
}

}  // namespace

u64 SvcGet64(const RCtx& ctx, int low, int high) {
    return (static_cast<u64>(ctx.r[high]) << 32) | ctx.r[low];
}

void SvcPut64(RCtx& ctx, int low, int high, u64 value) {
    ctx.r[low] = static_cast<u32>(value);
    ctx.r[high] = static_cast<u32>(value >> 32);
}

SvcTable::SvcTable(Kernel& kernel, GuestScheduler& scheduler, ThreadManager& threads)
    : impl_(std::make_unique<Impl>(kernel, scheduler, threads)) {}

SvcTable::~SvcTable() = default;

void SvcTable::Install(CodeHost& host) {
    host.SetSvcHandler([this](RCtx& ctx, u32 imm, u32 next_pc) { CallSvc(ctx, imm, next_pc); });
}

void SvcTable::CallSvc(RCtx& ctx, u32 imm, u32 next_pc) {
    impl_->Call(ctx, imm, next_pc);
}

void SvcTable::SetHooks(SvcHooks hooks) {
    impl_->hooks = std::move(hooks);
}

void SvcTable::SetConnectToPortHandler(ConnectToPortHandler handler) {
    impl_->connect_to_port = std::move(handler);
}

void SvcTable::SetSendSyncRequestHandler(SendSyncRequestHandler handler) {
    impl_->send_sync_request = std::move(handler);
}

const std::optional<SvcExit>& SvcTable::Exit() const {
    return impl_->exit;
}

std::vector<SvcCall> SvcTable::PendingCalls() const {
    auto calls = impl_->pending;
    std::ranges::sort(calls, {}, &SvcCall::thread_id);
    return calls;
}

void SvcTable::Impl::Call(RCtx& ctx, u32 imm, u32 next_pc) {
    ctx.r[15] = next_pc & ~1u;
    const SvcScope scope(sched, imm);
    Thread* thread = sched.CurrentThread();
    MH4N_ASSERT(thread);
    SvcFrame frame{ctx, *thread, imm};
    if (hooks.starting) {
        hooks.starting();
    }

    // GetThreadId dominates the game's SVC traffic. It cannot suspend or call back into the host.
    const bool trace = hooks.before || hooks.complete;
    if (imm == 0x37 && !trace) {
        kernel_detail::ChargeCycles(SvcCycles(imm));
        GetThreadId(frame);
        return;
    }

    SvcCall call{
        .thread_id = thread->thread_id,
        .number = imm,
        .pc = ctx.r[15],
        .lr = ctx.r[14],
        .ticks = trace ? kernel_detail::TraceTicks(sched.NowTicks()) : 0,
    };
    std::copy_n(ctx.r, call.regs.size(), call.regs.begin());
    // A fiber can exit or be terminated with this call on its stack. Keep that entry for tracing.
    pending.push_back(call);
    if (hooks.before) {
        hooks.before(call);
    }
    kernel_detail::ChargeCycles(SvcCycles(imm));
    Dispatch(frame);
    if (hooks.complete) {
        const std::array<u32, 4> out{ctx.r[0], ctx.r[1], ctx.r[2], ctx.r[3]};
        hooks.complete(call, out);
    }
    // Another fiber may have grown or erased entries in the vector while this call was parked.
    const auto entry = std::ranges::find(pending, call.thread_id, &SvcCall::thread_id);
    MH4N_ASSERT(entry != pending.end());
    pending.erase(entry);
}

void SvcTable::Impl::Dispatch(SvcFrame& f) {
    switch (f.number) {
    case 0x01: ControlMemory(f); break;
    case 0x02: QueryMemory(f); break;
    case 0x03: EndRun(f);
    case 0x08: CreateThread(f); break;
    case 0x09: sched.ExitCurrent();
    case 0x0A: SleepThread(f); break;
    case 0x0B: GetThreadPriority(f); break;
    case 0x0C: SetThreadPriority(f); break;
    case 0x13: CreateMutex(f); break;
    case 0x14: ReleaseMutex(f); break;
    case 0x15: CreateSemaphore(f); break;
    case 0x16: ReleaseSemaphore(f); break;
    case 0x17: CreateEvent(f); break;
    case 0x18: SignalEvent(f); break;
    case 0x19: ClearEvent(f); break;
    case 0x1A: CreateTimer(f); break;
    case 0x1B: SetTimer(f); break;
    case 0x1C: CancelTimer(f); break;
    case 0x1D: ClearTimer(f); break;
    case 0x1E: CreateMemoryBlock(f); break;
    case 0x1F: MapMemoryBlock(f); break;
    case 0x20: UnmapMemoryBlock(f); break;
    case 0x21: CreateAddressArbiter(f); break;
    case 0x22: ArbitrateAddress(f); break;
    case 0x23: CloseHandle(f); break;
    case 0x24: WaitSynchronization1(f); break;
    case 0x25: WaitSynchronizationN(f); break;
    case 0x27: DuplicateHandle(f); break;
    case 0x28: GetSystemTick(f); break;
    case 0x29: GetHandleInfo(f); break;
    case 0x2A: GetSystemInfo(f); break;
    case 0x2B: GetProcessInfo(f); break;
    case 0x2C: GetThreadInfo(f); break;
    case 0x2D: ConnectToPort(f); break;
    case 0x32: SendSyncRequest(f); break;
    case 0x35: GetProcessId(f); break;
    case 0x37: GetThreadId(f); break;
    case 0x38: GetResourceLimit(f); break;
    case 0x39: GetResourceLimitValues(f, false); break;
    case 0x3A: GetResourceLimitValues(f, true); break;
    case 0x3C: Break(f); break;
    case 0x3D: OutputDebugString(f); break;
    case 0x54: FlushProcessDataCache(f); break;
    case 0x70: ControlProcessMemory(f); break;
    case 0xB3: ControlProcess(f); break;
    default: LogUnknown(f); break;
    }
}

void SvcTable::Impl::LogUnknown(const SvcFrame& f) {
    if (unknown_logged.insert(f.number).second) {
        MH4N_LOG(Error, kLog, "unimplemented svc 0x{:X} at 0x{:08X}", f.number, f.ctx.r[14]);
    }
}

[[noreturn]] void SvcTable::Impl::EndRun(const SvcFrame& f) {
    if (!exit) {
        if (f.number == 0x3C) {
            constexpr std::array reasons{"PANIC", "ASSERT", "USER"};
            const auto reason = static_cast<u8>(f.ctx.r[0]);
            exit.emplace(SvcExit{3, fmt::format("svcBreak {} on thread {}",
                                               reason < reasons.size() ? reasons[reason] : "UNKNOWN",
                                               f.thread.thread_id)});
            for (int i = 0; i < 16; ++i) {
                fmt::format_to(std::back_inserter(exit->report), "{}r{:<2}={:08X}",
                               i % 4 == 0 ? "\n" : " ", i, f.ctx.r[i]);
            }
            MH4N_LOG(Critical, kLog, "{}", exit->report);
        } else {
            exit.emplace();
        }
    }
    sched.RequestStop();
    sched.ExitCurrent();
}

void SvcTable::Impl::CreateThread(SvcFrame& f) {
    const ThreadSpec spec{
        .entry_point = f.ctx.r[1],
        .arg = f.ctx.r[2],
        .stack_top = f.ctx.r[3],
        .priority = f.ctx.r[0],
        .processor_id = static_cast<s32>(f.ctx.r[4]),
        .host_entry = {},
    };
    Handle handle = 0;
    std::shared_ptr<Thread> child;
    const Result result = threads.CreateThread(spec, &handle, hooks.thread_created ? &child : nullptr);
    ReturnValue(f.ctx, result, handle);
    if (result == kResultSuccess && hooks.thread_created) {
        hooks.thread_created({f.thread.thread_id, child->thread_id, spec.entry_point, spec.arg,
                              spec.stack_top, spec.priority, spec.processor_id, child->processor_id});
    }
}

void SvcTable::Impl::SleepThread(SvcFrame& f) {
    sched.Sleep(static_cast<s64>(SvcGet64(f.ctx, 0, 1)));
}

void SvcTable::Impl::GetThreadPriority(SvcFrame& f) {
    const auto thread = kernel.GetObject<Thread>(f.ctx.r[1]);
    if (!thread) {
        ReturnValue(f.ctx, kResultInvalidHandle);
        return;
    }
    ReturnValue(f.ctx, kResultSuccess, thread->GetPriority());
}

void SvcTable::Impl::SetThreadPriority(SvcFrame& f) {
    const u32 priority = f.ctx.r[1];
    if (priority > kThreadPrioLowest) {
        f.ctx.r[0] = kResultOutOfRange;
        return;
    }
    const auto thread = kernel.GetObject<Thread>(f.ctx.r[0]);
    if (!thread) {
        f.ctx.r[0] = kResultInvalidHandle;
        return;
    }
    const auto limit = kernel.CurrentProcess()->resource_limit->GetLimitValue(ResourceLimitType::Priority);
    if (static_cast<s32>(priority) < limit) {
        f.ctx.r[0] = kResultNotAuthorized;
        return;
    }
    thread->SetPriority(priority);
    for (const auto& mutex : thread->pending_mutexes) {
        mutex->UpdatePriority();
    }
    sched.PrepareReschedule();
    f.ctx.r[0] = kResultSuccess;
}

void SvcTable::Impl::CreateMutex(SvcFrame& f) {
    const auto& limit = kernel.CurrentProcess()->resource_limit;
    if (!limit->Reserve(ResourceLimitType::Mutex, 1)) {
        ReturnValue(f.ctx, kResultOutOfMutexes);
        return;
    }
    auto mutex = kernel.NewMutex(f.ctx.r[1] != 0);
    mutex->resource_limit = limit;
    ReturnHandle(f.ctx, kernel, std::move(mutex));
}

void SvcTable::Impl::ReleaseMutex(SvcFrame& f) {
    const auto mutex = kernel.GetObject<Mutex>(f.ctx.r[0]);
    f.ctx.r[0] = mutex ? mutex->Release(&f.thread) : kResultInvalidHandle;
}

void SvcTable::Impl::CreateSemaphore(SvcFrame& f) {
    const auto& limit = kernel.CurrentProcess()->resource_limit;
    if (!limit->Reserve(ResourceLimitType::Semaphore, 1)) {
        ReturnValue(f.ctx, kResultOutOfSemaphores);
        return;
    }
    std::shared_ptr<Semaphore> semaphore;
    const Result result = kernel.NewSemaphore(&semaphore, static_cast<s32>(f.ctx.r[1]),
                                              static_cast<s32>(f.ctx.r[2]));
    if (result != kResultSuccess) {
        ReturnValue(f.ctx, result);
        return;
    }
    semaphore->resource_limit = limit;
    ReturnHandle(f.ctx, kernel, std::move(semaphore));
}

void SvcTable::Impl::ReleaseSemaphore(SvcFrame& f) {
    const auto semaphore = kernel.GetObject<Semaphore>(f.ctx.r[1]);
    if (!semaphore) {
        ReturnValue(f.ctx, kResultInvalidHandle);
        return;
    }
    s32 count = 0;
    const Result result = semaphore->Release(&count, static_cast<s32>(f.ctx.r[2]));
    ReturnValue(f.ctx, result, result == kResultSuccess ? static_cast<u32>(count) : 0);
}

void SvcTable::Impl::CreateEvent(SvcFrame& f) {
    const auto& limit = kernel.CurrentProcess()->resource_limit;
    if (!limit->Reserve(ResourceLimitType::Event, 1)) {
        ReturnValue(f.ctx, kResultOutOfEvents);
        return;
    }
    auto event = kernel.NewEvent(static_cast<ResetType>(f.ctx.r[1]));
    event->resource_limit = limit;
    ReturnHandle(f.ctx, kernel, std::move(event));
}

void SvcTable::Impl::SignalEvent(SvcFrame& f) {
    ApplyToObject(f.ctx, kernel, &Event::Signal);
}

void SvcTable::Impl::ClearEvent(SvcFrame& f) {
    ApplyToObject(f.ctx, kernel, &Event::Clear);
}

void SvcTable::Impl::CreateTimer(SvcFrame& f) {
    const auto& limit = kernel.CurrentProcess()->resource_limit;
    if (!limit->Reserve(ResourceLimitType::Timer, 1)) {
        ReturnValue(f.ctx, kResultOutOfTimers);
        return;
    }
    auto timer = kernel.NewTimer(static_cast<ResetType>(f.ctx.r[1]));
    timer->resource_limit = limit;
    ReturnHandle(f.ctx, kernel, std::move(timer));
}

void SvcTable::Impl::SetTimer(SvcFrame& f) {
    const auto initial = static_cast<s64>(SvcGet64(f.ctx, 2, 3));
    const auto interval = static_cast<s64>(SvcGet64(f.ctx, 1, 4));
    if (initial < 0 || interval < 0) {
        f.ctx.r[0] = kResultOutOfRangeKernel;
        return;
    }
    const auto timer = kernel.GetObject<Timer>(f.ctx.r[0]);
    if (!timer) {
        f.ctx.r[0] = kResultInvalidHandle;
        return;
    }
    timer->Set(initial, interval);
    f.ctx.r[0] = kResultSuccess;
}

void SvcTable::Impl::CancelTimer(SvcFrame& f) {
    ApplyToObject(f.ctx, kernel, &Timer::Cancel);
}

void SvcTable::Impl::ClearTimer(SvcFrame& f) {
    ApplyToObject(f.ctx, kernel, &Timer::Clear);
}

void SvcTable::Impl::CreateAddressArbiter(SvcFrame& f) {
    const auto& limit = kernel.CurrentProcess()->resource_limit;
    if (!limit->Reserve(ResourceLimitType::AddressArbiter, 1)) {
        ReturnValue(f.ctx, kResultOutOfAddressArbiters);
        return;
    }
    auto arbiter = kernel.NewAddressArbiter();
    arbiter->resource_limit = limit;
    ReturnHandle(f.ctx, kernel, std::move(arbiter));
}

void SvcTable::Impl::ArbitrateAddress(SvcFrame& f) {
    Result result;
    {
        const auto arbiter = kernel.GetObject<AddressArbiter>(f.ctx.r[0]);
        if (!arbiter) {
            f.ctx.r[0] = kResultInvalidHandle;
            return;
        }
        result = arbiter->ArbitrateAddress(sched.CurrentThreadPtr(),
                                          static_cast<ArbitrationType>(f.ctx.r[2]), f.ctx.r[1],
                                          static_cast<s32>(f.ctx.r[3]),
                                          static_cast<s64>(SvcGet64(f.ctx, 4, 5)));
    }
    if (f.thread.status == ThreadStatus::WaitArb) {
        result = sched.Park(f.thread).result.value_or(result);
    }
    f.ctx.r[0] = result;
}

void SvcTable::Impl::CloseHandle(SvcFrame& f) {
    f.ctx.r[0] = kernel.CurrentProcess()->handle_table.Close(f.ctx.r[0]);
}

void SvcTable::Impl::WaitSynchronization1(SvcFrame& f) {
    auto object = kernel.GetObject<WaitObject>(f.ctx.r[0]);
    if (!object) {
        f.ctx.r[0] = kResultInvalidHandle;
        return;
    }
    const auto timeout = static_cast<s64>(SvcGet64(f.ctx, 2, 3));
    if (!object->ShouldWait(&f.thread)) {
        object->Acquire(&f.thread);
        f.ctx.r[0] = kResultSuccess;
        return;
    }
    if (timeout == 0) {
        f.ctx.r[0] = kResultTimeout;
        return;
    }
    std::vector<std::shared_ptr<WaitObject>> objects;
    objects.push_back(std::move(object));
    const auto outcome = sched.Block(f.thread, std::move(objects), false, timeout);
    f.ctx.r[0] = outcome.result.value_or(kResultSuccess);
}

void SvcTable::Impl::WaitSynchronizationN(SvcFrame& f) {
    const auto timeout = static_cast<s64>(SvcGet64(f.ctx, 0, 4));
    const u32 address = f.ctx.r[1];
    const u32 count = f.ctx.r[2];
    const bool wait_all = f.ctx.r[3] != 0;
    if (!memory.IsMapped(address, 1)) {
        ReturnValue(f.ctx, kResultInvalidPointer);
        return;
    }
    if (static_cast<s32>(count) < 0) {
        ReturnValue(f.ctx, kResultOutOfRange);
        return;
    }
    if (count > (kAddressSpaceSize - address) / sizeof(Handle)) {
        ReturnValue(f.ctx, kResultInvalidPointer);
        return;
    }
    std::vector<std::shared_ptr<WaitObject>> objects;
    for (u32 i = 0; i < count; ++i) {
        const auto handle = memory.Read<Handle>(address + i * static_cast<u32>(sizeof(Handle)));
        if (!handle) {
            ReturnValue(f.ctx, kResultInvalidPointer);
            return;
        }
        auto object = kernel.GetObject<WaitObject>(*handle);
        if (!object) {
            ReturnValue(f.ctx, kResultInvalidHandle);
            return;
        }
        objects.push_back(std::move(object));
    }
    const auto outcome = sched.Block(f.thread, std::move(objects), wait_all, timeout);
    const u32 index = outcome.blocked || outcome.index >= 0 ? static_cast<u32>(outcome.index) : 0;
    ReturnValue(f.ctx, outcome.result.value_or(kResultSuccess), index);
}

void SvcTable::Impl::DuplicateHandle(SvcFrame& f) {
    Handle handle = 0;
    const Result result = kernel.CurrentProcess()->handle_table.Duplicate(&handle, f.ctx.r[1]);
    ReturnValue(f.ctx, result, handle);
}

void SvcTable::Impl::GetSystemTick(SvcFrame& f) {
    SvcPut64(f.ctx, 0, 1, sched.NowTicks());
}

void SvcTable::Impl::GetHandleInfo(SvcFrame& f) {
    const auto object = kernel.GetObject<Object>(f.ctx.r[1]);
    if (!object) {
        ReturnInfo(f.ctx, kResultInvalidHandle);
        return;
    }
    switch (f.ctx.r[2]) {
    case 0:
        ReturnInfo(f.ctx, kResultSuccess, object->GetHandleType() == HandleType::Process
                       ? static_cast<const Process&>(*object).creation_time_ticks : 0);
        break;
    case 1:
        ReturnInfo(f.ctx, kResultSuccess, static_cast<u64>(object.use_count() - 1));
        break;
    case 2:
    case 0x32107:
        ReturnInfo(f.ctx, kResultSuccess);
        break;
    default:
        ReturnInfo(f.ctx, kResultInvalidEnumValue);
        break;
    }
}

void SvcTable::Impl::GetSystemInfo(SvcFrame& f) {
    const auto& regions = kernel.Regions();
    const bool new_3ds = regions.TotalSize() == kFcramSizeNew3ds;
    const u32 param = f.ctx.r[2];
    u64 value = 0;
    switch (f.ctx.r[1]) {
    case 0:
        if (param == 0) {
            value = regions.Used(FcramRegion::Application) + regions.Used(FcramRegion::System) +
                    regions.Used(FcramRegion::Base);
        } else if (param <= 3) {
            value = regions.Used(static_cast<FcramRegion>(param - 1));
        }
        break;
    case 26:
        value = 5;  // The process count observed by the compatibility tests.
        break;
    case 0x10000:
        if (param == 0x80) {
            value = regions.Size(FcramRegion::Application);
        } else if (param == 0x201) {
            value = new_3ds;
        }
        break;
    case 0x10001:
        if (!new_3ds) {
            ReturnInfo(f.ctx, kResultInvalidEnumValue);
            return;
        }
        break;
    }
    ReturnInfo(f.ctx, kResultSuccess, value);
}

void SvcTable::Impl::GetProcessInfo(SvcFrame& f) {
    const auto process = kernel.GetObject<Process>(f.ctx.r[1]);
    if (!process) {
        ReturnInfo(f.ctx, kResultInvalidHandle);
        return;
    }
    switch (f.ctx.r[2]) {
    case 0:
    case 2:
        if ((process->memory_used & kPageMask) != 0) {
            ReturnInfo(f.ctx, kResultMisalignedSize);
        } else {
            ReturnInfo(f.ctx, kResultSuccess, process->memory_used);
        }
        break;
    case 1:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
        ReturnInfo(f.ctx, kResultSuccess);
        break;
    case 20:
        ReturnInfo(f.ctx, kResultSuccess, kFcramPaddr -
                   LinearHeapAreaForKernelVersion(process->kernel_version));
        break;
    case 21:
    case 22:
    case 23:
        ReturnInfo(f.ctx, kResultNotImplemented);
        break;
    default:
        ReturnInfo(f.ctx, kResultInvalidEnumValue);
        break;
    }
}

void SvcTable::Impl::GetThreadInfo(SvcFrame& f) {
    const auto thread = kernel.GetObject<Thread>(f.ctx.r[1]);
    if (!thread) {
        ReturnInfo(f.ctx, kResultInvalidHandle);
    } else if (f.ctx.r[2] != 0x10000) {
        ReturnInfo(f.ctx, kResultInvalidEnumValue);
    } else {
        ReturnInfo(f.ctx, kResultSuccess, thread->tls_address);
    }
}

void SvcTable::Impl::ConnectToPort(SvcFrame& f) {
    const u32 address = f.ctx.r[1];
    if (!connect_to_port || !memory.IsMapped(address, 1)) {
        ReturnValue(f.ctx, kResultNotFound);
        return;
    }
    std::string name;
    constexpr u32 kMaxPortName = 11;
    for (u32 i = 0; i <= kMaxPortName; ++i) {
        const char c = memory.Read<char>(address + i).value_or('\0');
        if (c == '\0') {
            break;
        }
        name.push_back(c);
    }
    if (name.size() > kMaxPortName) {
        ReturnValue(f.ctx, kResultPortNameTooLong);
        return;
    }
    std::shared_ptr<Object> session;
    const Result result = connect_to_port(name, &session);
    if (result != kResultSuccess || !session) {
        ReturnValue(f.ctx, result == kResultSuccess ? kResultNotFound : result);
        return;
    }
    ReturnHandle(f.ctx, kernel, std::move(session));
}

void SvcTable::Impl::SendSyncRequest(SvcFrame& f) {
    Result result = send_sync_request ? send_sync_request(f.thread, f.ctx.r[0]) : kResultInvalidHandle;
    if (f.thread.status != ThreadStatus::Running) {
        result = sched.Park(f.thread).result.value_or(result);
    }
    f.ctx.r[0] = result;
}

void SvcTable::Impl::GetProcessId(SvcFrame& f) {
    const auto process = kernel.GetObject<Process>(f.ctx.r[1]);
    if (!process) {
        ReturnValue(f.ctx, kResultInvalidHandle);
        return;
    }
    ReturnValue(f.ctx, kResultSuccess, process->process_id);
}

void SvcTable::Impl::GetThreadId(SvcFrame& f) {
    const Handle handle = f.ctx.r[1];
    if (handle == kHandleCurrentThread) {
        ReturnValue(f.ctx, kResultSuccess, f.thread.thread_id);
        return;
    }
    const auto& process = kernel.CurrentProcess();
    const Object* object = process ? process->handle_table.PeekGeneric(handle) : nullptr;
    if (object && object->GetHandleType() == HandleType::Thread) {
        ReturnValue(f.ctx, kResultSuccess, static_cast<const Thread&>(*object).thread_id);
    } else {
        ReturnValue(f.ctx, kResultInvalidHandle);
    }
}

void SvcTable::Impl::GetResourceLimit(SvcFrame& f) {
    const auto process = kernel.GetObject<Process>(f.ctx.r[1]);
    if (!process) {
        ReturnValue(f.ctx, kResultInvalidHandle);
        return;
    }
    ReturnHandle(f.ctx, kernel, process->resource_limit);
}

void SvcTable::Impl::GetResourceLimitValues(SvcFrame& f, bool current) {
    const auto limit = kernel.GetObject<ResourceLimit>(f.ctx.r[1]);
    if (!limit) {
        f.ctx.r[0] = kResultInvalidHandle;
        return;
    }
    const auto count = static_cast<s32>(f.ctx.r[3]);
    u64 names = f.ctx.r[2];
    u64 values = f.ctx.r[0];
    for (s32 i = 0; i < count; ++i, names += sizeof(u32), values += sizeof(s64)) {
        if (names + sizeof(u32) > kAddressSpaceSize || values + sizeof(s64) > kAddressSpaceSize) {
            f.ctx.r[0] = kResultInvalidPointer;
            return;
        }
        const auto name = memory.Read<u32>(static_cast<u32>(names));
        if (!name) {
            f.ctx.r[0] = kResultInvalidPointer;
            return;
        }
        const auto type = static_cast<ResourceLimitType>(*name);
        if (type >= ResourceLimitType::Max) {
            f.ctx.r[0] = kResultInvalidEnumValue;
            return;
        }
        const s64 value = current ? limit->GetCurrentValue(type) : limit->GetLimitValue(type);
        if (!memory.Write(static_cast<u32>(values), value)) {
            f.ctx.r[0] = kResultInvalidPointer;
            return;
        }
    }
    f.ctx.r[0] = kResultSuccess;
}

void SvcTable::Impl::Break(SvcFrame& f) {
    EndRun(f);
}

void SvcTable::Impl::OutputDebugString(SvcFrame& f) {
    const u32 address = f.ctx.r[0];
    const u32 length = f.ctx.r[1];
    constexpr u32 kMaxDebugString = 0x10000;
    if (length == 0 || length > kMaxDebugString || !memory.IsMapped(address, 1)) {
        return;
    }
    std::string message(length, '\0');
    if (!memory.ReadBlock(message.data(), address, length)) {
        for (u32 i = 0; i < length && static_cast<u64>(address) + i < kAddressSpaceSize; ++i) {
            message[i] = memory.Read<char>(address + i).value_or('\0');
        }
    }
    MH4N_LOG(Debug, kDebugLog, "{}", message);
    if (hooks.debug_string) {
        hooks.debug_string(message);
    }
}

void SvcTable::Impl::ControlProcess(SvcFrame& f) {
    if (!kernel.GetObject<Process>(f.ctx.r[0])) {
        f.ctx.r[0] = kResultInvalidHandle;
        return;
    }
    if (f.ctx.r[1] != 7) {
        f.ctx.r[0] = kResultNotImplemented;
        return;
    }
    threads.SetNoThreadRestrictions(f.ctx.r[2] == 1);
    f.ctx.r[0] = kResultSuccess;
}

void SvcTable::Impl::FlushProcessDataCache(SvcFrame& f) {
    f.ctx.r[0] = kernel.GetObject<Process>(f.ctx.r[0]) ? kResultSuccess : kResultInvalidHandle;
}

}  // namespace mh4n
