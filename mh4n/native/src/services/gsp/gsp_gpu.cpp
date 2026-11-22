// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew GSP Services, gsp.h, gsp_test and golden/ipc/boot-village.ipc.trace.

#include "gsp.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "mh4n/registry.h"

namespace mh4n::gsp {
namespace {

constexpr u32 kLedForceOffAddress = 0x1FF81084;
constexpr u32 kBlackFillEnabled = 1u << 24;

void Respond(Request& request, Result result = kResultSuccess) {
    request.MakeReply(1, 0).Push(result);
}

Result CheckRegisterRange(u32 offset, u32 size) {
    if (offset >= kRegsLimit || offset % sizeof(u32) != 0) {
        return kResultRegsOutOfRangeOrMisaligned;
    }
    if (size > kMaxRegBytes) {
        return kResultRegsInvalidSize;
    }
    if (size % sizeof(u32) != 0) {
        return kResultRegsMisaligned;
    }
    return kResultSuccess;
}

u32 BufferWord(const StaticBuffer& buffer, u32 offset) {
    u32 word;
    std::memcpy(&word, buffer.data.data() + offset, sizeof(word));
    return word;
}

class GpuSession final : public Service {
public:
    explicit GpuSession(ServiceContext& context)
        : Service(context, "gsp::Gpu"), state_(GspState::Get(context)),
          thread_(state_->ClaimThread()) {
        Register(0x01, "WriteHWRegs", &GpuSession::WriteRegisters);
        Register(0x02, "WriteHWRegsWithMask", &GpuSession::WriteMaskedRegisters);
        Register(0x03, "WriteHWRegRepeat");
        Register(0x04, "ReadHWRegs", &GpuSession::ReadRegisters);
        Register(0x05, "SetBufferSwap", &GpuSession::SetBufferSwap);
        Register(0x06, "SetCommandList");
        Register(0x07, "RequestDma");
        Register(0x08, "FlushDataCache", &GpuSession::FlushCache);
        Register(0x09, "InvalidateDataCache", &GpuSession::InvalidateCache);
        Register(0x0A, "RegisterInterruptEvents");
        Register(0x0B, "SetLcdForceBlack", &GpuSession::SetLcdForceBlack);
        Register(0x0C, "TriggerCmdReqQueue", &GpuSession::TriggerQueue);
        Register(0x0D, "SetDisplayTransfer");
        Register(0x0E, "SetTextureCopy");
        Register(0x0F, "SetMemoryFill");
        Register(0x10, "SetAxiConfigQoSMode", &GpuSession::Acknowledge);
        Register(0x11, "SetPerfLogMode", &GpuSession::SetPerfLogMode);
        Register(0x12, "GetPerfLog", &GpuSession::GetPerfLog);
        Register(0x13, "RegisterInterruptRelayQueue", &GpuSession::RegisterRelay);
        Register(0x14, "UnregisterInterruptRelayQueue", &GpuSession::UnregisterRelay);
        Register(0x15, "TryAcquireRight", &GpuSession::AcquireRight);
        Register(0x16, "AcquireRight", &GpuSession::AcquireRight);
        Register(0x17, "ReleaseRight", &GpuSession::ReleaseRight);
        Register(0x18, "ImportDisplayCaptureInfo", &GpuSession::ImportDisplayCaptureInfo);
        Register(0x19, "SaveVramSysArea", &GpuSession::SaveVram);
        Register(0x1A, "RestoreVramSysArea", &GpuSession::RestoreVram);
        Register(0x1B, "ResetGpuCore");
        Register(0x1C, "SetLedForceOff", &GpuSession::SetLedForceOff);
        Register(0x1D, "SetTestCommand");
        Register(0x1E, "SetInternalPriorities", &GpuSession::Acknowledge);
        Register(0x1F, "StoreDataCache", &GpuSession::FlushCache);
    }

    ~GpuSession() override {
        if (thread_ < kMaxThreads) {
            state_->ReleaseThread(thread_);
        }
    }

private:
    void Acknowledge(Request& request) { Respond(request); }

    void WriteRegisters(Request& request) { WriteRegistersImpl(request, false); }

    void WriteMaskedRegisters(Request& request) { WriteRegistersImpl(request, true); }

    void WriteRegistersImpl(Request& request, bool masked) {
        const u32 offset = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const Result validation = CheckRegisterRange(offset, size);
        if (validation != kResultSuccess) {
            Respond(request, validation);
            return;
        }

        const StaticBuffer* values = request.PopStaticBuffer();
        const StaticBuffer* masks = masked ? request.PopStaticBuffer() : nullptr;
        if (!values || values->data.size() < size ||
            (masked && (!masks || masks->data.size() < size))) {
            Respond(request, kResultRegsInvalidSize);
            return;
        }
        if (auto* gpu = state_->GetGpu()) {
            for (u32 byte = 0; byte < size; byte += sizeof(u32)) {
                const u32 address = kRegsBegin + offset + byte;
                u32 value = BufferWord(*values, byte);
                if (masks) {
                    const u32 mask = BufferWord(*masks, byte);
                    value = (gpu->ReadReg(address) & ~mask) | (value & mask);
                }
                gpu->WriteReg(address, value);
            }
        }
        Respond(request);
    }

    void ReadRegisters(Request& request) {
        const u32 offset = request.Pop<u32>();
        const u32 size = std::min(request.Pop<u32>(), kMaxRegBytes);
        const Result validation = CheckRegisterRange(offset, size);
        if (validation != kResultSuccess) {
            Respond(request, validation);
            return;
        }

        std::vector<u8> values(size);
        if (auto* gpu = state_->GetGpu()) {
            for (u32 byte = 0; byte < size; byte += sizeof(u32)) {
                const u32 value = gpu->ReadReg(kRegsBegin + offset + byte);
                std::memcpy(values.data() + byte, &value, sizeof(value));
            }
        }
        auto& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer(std::move(values), 0);
    }

    void SetBufferSwap(Request& request) {
        const u32 screen = request.Pop<u32>();
        const auto info = request.PopRaw<gpu::FrameBufferInfo>();
        if (auto* gpu = state_->GetGpu()) {
            gpu->SetBufferSwap(screen, info);
        }
        Respond(request);
    }

    void FlushCache(Request& request) {
        const u32 address = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        if (auto* gpu = state_->GetGpu()) {
            gpu->FlushRegion(address, size);
        }
        Respond(request);
    }

    void InvalidateCache(Request& request) {
        const u32 address = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        if (auto* gpu = state_->GetGpu()) {
            gpu->InvalidateRegion(address, size);
        }
        Respond(request);
    }

    void SetLcdForceBlack(Request& request) {
        const bool enabled = request.Pop<bool>();
        if (auto* gpu = state_->GetGpu()) {
            gpu->SetColorFill(enabled ? kBlackFillEnabled : 0);
        }
        Respond(request);
    }

    void TriggerQueue(Request& request) {
        state_->ProcessCommandQueue();
        Respond(request);
    }

    void SetPerfLogMode(Request& request) {
        state_->SetPerfLogEnabled(request.Pop<bool>());
        Respond(request);
    }

    void GetPerfLog(Request& request) {
        auto& reply = request.MakeReply(15, 0);
        reply.PushRaw(state_->PerfLog());
        reply.Push(u32{0});
    }

    void RegisterRelay(Request& request) {
        const u32 flags = request.Pop<u32>();
        auto event = request.PopObject<Event>();
        if (!event || thread_ >= kMaxThreads) {
            Respond(request, kResultInvalidHandle);
            return;
        }
        auto& slot = state_->Slot(thread_);
        slot.flags = flags;
        slot.interrupt_event = std::move(event);
        slot.registered = true;
        const Result result = state_->TakeFirstInitialization() ? kResultFirstInitialization
                                                               : kResultSuccess;
        auto& reply = request.MakeReply(2, 2);
        reply.Push(result, thread_);
        reply.PushCopyObjects(state_->SharedMem());
    }

    void UnregisterRelay(Request& request) {
        if (thread_ < kMaxThreads) {
            auto& slot = state_->Slot(thread_);
            slot.registered = false;
            slot.flags = 0;
            slot.interrupt_event.reset();
        }
        Respond(request);
    }

    void AcquireRight(Request& request) {
        const u32 owner = state_->ThreadWithRights();
        if (thread_ >= kMaxThreads) {
            Respond(request, kResultBusy);
        } else if (owner == thread_) {
            Respond(request, kResultAlreadyDone);
        } else if (owner != kNoThread) {
            Respond(request, kResultBusy);
        } else {
            state_->SetThreadWithRights(thread_);
            Respond(request);
        }
    }

    void ReleaseRight(Request& request) {
        if (thread_ < kMaxThreads) {
            state_->ReleaseRight(thread_);
        }
        Respond(request);
    }

    void ImportDisplayCaptureInfo(Request& request) {
        auto& reply = request.MakeReply(9, 0);
        reply.Push(kResultSuccess);
        for (u32 screen = 0; screen < 2; ++screen) {
            const auto [dirty, info] = state_->GetFrameBufferInfo(state_->ThreadWithRights(), screen);
            if (dirty) {
                state_->ForceBufferSwap(screen);
            }
            reply.Push(info->address_left, info->address_right, info->format, info->stride);
        }
    }

    Handled SaveVram(Request& request) { return QueueVramOperation(request, true); }

    Handled RestoreVram(Request& request) { return QueueVramOperation(request, false); }

    Handled QueueVramOperation(Request& request, bool save) {
        if (thread_ >= kMaxThreads || (state_->Slot(thread_).flags & kAllowSaveVramSysArea) == 0) {
            Respond(request, kResultNoSaveVramSysAreaPerm);
            return Handled::Done;
        }
        if (save) {
            state_->SaveVramSysArea();
        } else {
            state_->RestoreVramSysArea();
        }
        state_->Slot(thread_).vblank_waiters.push_back({request.Share(), request.CommandId()});
        return Handled::Deferred;
    }

    void SetLedForceOff(Request& request) {
        request.Memory().Write(kLedForceOffAddress, static_cast<u8>(request.Pop<bool>()));
        Respond(request);
    }

    std::shared_ptr<GspState> state_;
    u32 thread_;
};

MH4N_REGISTER(ServiceFactory, "gsp::Gpu", ServiceFactory{
    [](ServiceContext& context) { return std::make_unique<GpuSession>(context); }, kMaxThreads});

}  // namespace
}  // namespace mh4n::gsp
