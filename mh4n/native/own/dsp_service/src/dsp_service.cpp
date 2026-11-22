// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew DSP Services, libctru DSP IPC, dsp_usage.md, dsp_regions.tsv,
// golden boot-village IPC/SVC/MDSP, and the WP40 NativeDsp contract.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "audio_out/audio_out.h"
#include "dsp_native.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

constexpr auto kLog = "DSP";
constexpr char kServiceName[] = "dsp::DSP";
constexpr char kNewServiceName[] = "new/dsp::DSP";
constexpr u32 kDspDataBase = 0x1ff40000;
constexpr std::array<u32, 2> kBanks{0x1ff50000, 0x1ff70000};
constexpr u32 kComponentSize = 0xc25c;
constexpr u32 kComponentHeaderOffset = 0x100;
constexpr u32 kPipeInterruptType = 2;
constexpr u16 kPipeDoorbell = 0x4000;
constexpr u16 kFrameSemaphoreMask = 0x2000;
static_assert(NativeDsp::kFrameSamples == audio_out::kDspFrameSamples);

class DspDevice final : public std::enable_shared_from_this<DspDevice> {
public:
    explicit DspDevice(ServiceContext& context)
        : context_(context),
          dsp_([&context](u32 address, void* dst, u32 size) {
              const auto* samples = context.Memory().PhysicalPointer(address, size);
              if (!samples) {
                  return false;
              }
              std::memcpy(dst, samples, size);
              return true;
          }, [](std::string_view message) { MH4N_LOG(Warning, kLog, "{}", message); }) {}

    void Load() { loaded_ = true; }

    void Unload() {
        StopFrames();
        dsp_.Shutdown();
        loaded_ = false;
    }

    bool WritePipe(u32 pipe, std::span<const u8> bytes) {
        if (!loaded_ || pipe != NativeDsp::kAudioPipe ||
            !dsp_.WritePipe(NativeDsp::kAudioPipe, bytes)) {
            return false;
        }
        StopFrames();
        // States 0 and 2 publish the startup table. States 1 and 3 acknowledge in register 0.
        if (dsp_.PipeReadable(NativeDsp::kAudioPipe) != 0) {
            run_state_ = RunState::WaitingForDoorbell;
            input_bank_ = 0;
        }
        return true;
    }

    std::vector<u8> ReadPipe(u32 pipe, u32 peer, u16 size) {
        if (pipe != NativeDsp::kAudioPipe || peer != 0 ||
            size > dsp_.PipeReadable(NativeDsp::kAudioPipe)) {
            return {};
        }
        std::vector<u8> bytes(size);
        bytes.resize(dsp_.ReadPipe(NativeDsp::kAudioPipe, bytes));
        return bytes;
    }

    bool ReceiveDataReady() const { return dsp_.ReceiveDataReady(); }
    u16 ReceiveData() { return dsp_.ReceiveData(); }

    void SetInterrupt(std::shared_ptr<Event> event) { pipe_interrupt_ = std::move(event); }

    void SetSemaphore(u16 bits) {
        if ((bits & kPipeDoorbell) != 0 && run_state_ == RunState::WaitingForDoorbell) {
            run_state_ = RunState::WaitingForArm;
            SignalPipeInterrupt();
        }
    }

    void SetSemaphoreMask(u16 mask) { semaphore_mask_ = mask; }

    std::shared_ptr<Event> SemaphoreEvent() {
        if (!semaphore_event_) {
            semaphore_event_ = context_.GetKernel().NewEvent(ResetType::OneShot, "DSP semaphore");
            semaphore_event_->SetHleNotifier([weak = weak_from_this()] {
                if (const auto device = weak.lock()) {
                    device->ArmPublished();
                }
            });
        }
        return semaphore_event_;
    }

private:
    enum class RunState { Stopped, WaitingForDoorbell, WaitingForArm, Running };

    void SignalPipeInterrupt() {
        if (pipe_interrupt_) {
            pipe_interrupt_->Signal();
        }
    }

    void ArmPublished() {
        semaphore_event_->Clear();
        if ((semaphore_mask_ & kFrameSemaphoreMask) == 0) {
            return;
        }
        if (run_state_ == RunState::WaitingForArm) {
            run_state_ = RunState::Running;
            ScheduleFrame(context_.GetKernel().NowTicks() + audio_out::kDspFrameTicks);
        }
        if (run_state_ == RunState::Running) {
            frame_pending_ = true;
        }
    }

    void ScheduleFrame(u64 due) {
        timer_ = context_.GetKernel().ScheduleAt(due, [weak = weak_from_this(), due] {
            if (const auto device = weak.lock()) {
                device->ProcessFrame(due);
            }
        });
    }

    void StopFrames() {
        if (timer_ != 0) {
            context_.GetKernel().Cancel(std::exchange(timer_, 0));
        }
        run_state_ = RunState::Stopped;
        frame_pending_ = false;
    }

    void ProcessFrame(u64 due) {
        timer_ = 0;
        if (run_state_ != RunState::Running) {
            return;
        }
        {   // MODIFIED (diagnostic, 2026-11-22): periods run, with the ARM's bank, and periods skipped because the scheduler was late
            static auto since = std::chrono::steady_clock::now();
            static u64 run = 0, with_bank = 0, skipped = 0;
            static double worst_ms = 0;
            ++run;
            if (frame_pending_) ++with_bank;
            const u64 now_t = context_.GetKernel().NowTicks();
            if (now_t > due) {
                skipped += (now_t - due) / audio_out::kDspFrameTicks;
                worst_ms = std::max(worst_ms, static_cast<double>(now_t - due) / 268111.856);
            }
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
            if (secs >= 10.0) {
                MH4N_LOG(Info, kLog, "DSP clock: {:.1f} s: {} periods run ({:.1f} % of real time), {} with the ARM's bank, {} skipped as late, latest up to {:.1f} ms late",
                         secs, run, run * 160.0 / 32728.0 / secs * 100.0, with_bank, skipped, worst_ms);
                since = std::chrono::steady_clock::now();
                run = with_bank = skipped = 0;
                worst_ms = 0;
            }
        }

        NativeDsp::Frame frame{};
        if (std::exchange(frame_pending_, false)) {
            // The ARM starts with counter 4 in bank 0, then publishes once per pipe interrupt.
            const auto output_bank = input_bank_ ^ 1u;
            constexpr auto size = NativeDsp::kBankBytes;
            auto* input = context_.Memory().PhysicalPointer(kBanks[input_bank_], size);
            auto* output = context_.Memory().PhysicalPointer(kBanks[output_bank], size);
            if (input && output) {
                const auto tick_start = std::chrono::steady_clock::now();
                frame = dsp_.Tick(NativeDsp::Bank{input, size}, NativeDsp::Bank{output, size});
                // (2026-11-20: a DSP frame is due every ~4.1 ms on the game thread; say when mixing takes a good part of it)
                const double tick_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tick_start).count();
                tick_total_ms_ += tick_ms;
                tick_max_ms_ = std::max(tick_max_ms_, tick_ms);
                if (tick_ms >= 2.0 && slow_ticks_logged_ < 50) {
                    ++slow_ticks_logged_;
                    MH4N_LOG(Info, kLog, "a DSP frame took {:.2f} ms to mix (it is due every 4.1 ms)", tick_ms);
                }
                if (++ticks_ % 3000 == 0) {
                    MH4N_LOG(Info, kLog, "DSP mixing: {:.2f} ms a frame on average, {:.2f} ms the worst of the last 3000", tick_total_ms_ / 3000, tick_max_ms_);
                    tick_total_ms_ = tick_max_ms_ = 0;
                }
                last_input_ = input;   // MODIFIED: kept for the catch-up below
                last_output_ = output;
                input_bank_ = output_bank;
                SignalPipeInterrupt();
            }
        }
        if (auto* sink = context_.Interfaces().audio) {
            std::array<s16, NativeDsp::kFrameSamples * 2> interleaved;
            static_assert(sizeof(frame) == sizeof(interleaved));
            std::memcpy(interleaved.data(), frame.data(), sizeof(interleaved));
            sink->Queue(interleaved.data(), frame.size());
        }
        // Keep the deadline grid when the scheduler is late. A missed period has no new ARM bank.
        const u64 now = context_.GetKernel().NowTicks();
        const u64 period = audio_out::kDspFrameTicks;
        const u64 missed = now > due ? (now - due) / period : 0;
        // MODIFIED (2026-11-22): the periods the scheduler was too late for are played, not dropped. The real DSP never waits for the
        // ARM: the voices go on with the configuration they have. Ticking the last bank again does that (the voice flags are cleared
        // when read, so nothing is applied twice). Before this the music lost 2-4 % of its frames in the village (measured 96-98 % of
        // real time: slow, wavering tempo and an empty audio ring). Limited to 24 periods (about 100 ms).
        if (last_input_ && missed > 0) {
            if (auto* sink = context_.Interfaces().audio) {
                for (u64 k = 0; k < std::min<u64>(missed, 24); ++k) {
                    const NativeDsp::Frame extra = dsp_.Tick(NativeDsp::Bank{last_input_, NativeDsp::kBankBytes}, NativeDsp::Bank{last_output_, NativeDsp::kBankBytes});
                    std::array<s16, NativeDsp::kFrameSamples * 2> interleaved;
                    std::memcpy(interleaved.data(), extra.data(), sizeof(interleaved));
                    sink->Queue(interleaved.data(), extra.size());
                }
            }
        }
        ScheduleFrame(due + (missed + 1) * period);
    }

    ServiceContext& context_;
    NativeDsp dsp_;
    std::shared_ptr<Event> pipe_interrupt_;
    std::shared_ptr<Event> semaphore_event_;
    u64 timer_{};
    std::size_t input_bank_{};
    u16 semaphore_mask_{};
    RunState run_state_{RunState::Stopped};
    bool frame_pending_{};
    bool loaded_{};
    double tick_total_ms_{}, tick_max_ms_{};
    u64 ticks_{};
    u8* last_input_{};
    u8* last_output_{};
    u32 slow_ticks_logged_{};
};

class DspService final : public Service {
public:
    DspService(ServiceContext& context, std::shared_ptr<DspDevice> device)
        : Service(context, kServiceName), device_(std::move(device)) {
        // Command IDs and names: 3dbrew DSP Services.
        Register(0x0001, "RecvData", &DspService::RecvData);  // dsp_usage.md §1, libctru
        Register(0x0002, "RecvDataIsReady", &DspService::RecvDataIsReady);  // dsp_usage.md §1, libctru
        Register(0x0003, "SendData");
        Register(0x0004, "SendDataIsEmpty");
        Register(0x0005, "SendFifoEx");
        Register(0x0006, "RecvFifoEx");
        Register(0x0007, "SetSemaphore", &DspService::SetSemaphore);  // IPC 185, 218
        Register(0x0008, "GetSemaphore");
        Register(0x0009, "ClearSemaphore");
        Register(0x000a, "MaskSemaphore");
        Register(0x000b, "CheckSemaphoreRequest");
        Register(0x000c, "ConvertProcessAddressFromDspDram", &DspService::ConvertAddress);  // IPC 188–217
        Register(0x000d, "WriteProcessPipe", &DspService::WriteProcessPipe);  // IPC 184, dsp_usage.md §2
        Register(0x000e, "ReadPipe");
        Register(0x000f, "GetPipeReadableSize");
        Register(0x0010, "ReadPipeIfPossible", &DspService::ReadPipeIfPossible);  // IPC 186–187
        Register(0x0011, "LoadComponent", &DspService::LoadComponent);  // IPC 180
        Register(0x0012, "UnloadComponent", &DspService::UnloadComponent);  // dsp_usage.md §1, libctru
        Register(0x0013, "FlushDataCache", &DspService::FlushDataCache);  // IPC 2292 and later
        Register(0x0014, "InvalidateDCache");
        Register(0x0015, "RegisterInterruptEvents", &DspService::RegisterInterruptEvents);  // IPC 181
        Register(0x0016, "GetSemaphoreEventHandle", &DspService::GetSemaphoreEventHandle);  // IPC 182
        Register(0x0017, "SetSemaphoreMask", &DspService::SetSemaphoreMask);  // IPC 183
        Register(0x0018, "GetPhysicalAddress");
        Register(0x0019, "GetVirtualAddress");
        Register(0x001a, "SetIirFilterI2S1");
        Register(0x001b, "SetIirFilterI2S2");
        Register(0x001c, "SetIirFilterEQ");
        Register(0x001d, "ReadMultiEx_SPI2");
        Register(0x001e, "WriteMultiEx_SPI2");
        Register(0x001f, "GetHeadphoneStatus", &DspService::GetHeadphoneStatus);  // libctru DSP IPC
        Register(0x0020, "ForceHeadphoneOut");
        Register(0x0021, "GetIsDspOccupied");
    }

private:
    void RecvData(Request& request) {
        const u16 reg = request.Pop<u16>();
        request.MakeReply(2, 0).Push(kResultSuccess, reg == 0 ? device_->ReceiveData() : u16{0});
    }

    void RecvDataIsReady(Request& request) {
        const u16 reg = request.Pop<u16>();
        request.MakeReply(2, 0).Push(kResultSuccess, reg == 0 && device_->ReceiveDataReady());
    }

    void SetSemaphore(Request& request) {
        device_->SetSemaphore(request.Pop<u16>());
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void ConvertAddress(Request& request) {
        const u32 address = request.Pop<u32>();
        constexpr u32 word_count = (kDspRamVaddr + kDspRamSize - kDspDataBase) / 2;
        if (address >= word_count) {
            request.MakeReply(2, 0).Push(kResultInvalidAddress, u32{0});
            return;
        }
        request.MakeReply(2, 0).Push(kResultSuccess, kDspDataBase + 2 * address);
    }

    void WriteProcessPipe(Request& request) {
        const u32 pipe = request.Pop<u32>();
        const u32 count = request.Pop<u32>();
        const auto* buffer = request.PopStaticBuffer();
        const bool written = buffer && buffer->data.size() == count &&
                             device_->WritePipe(pipe, buffer->data);
        request.MakeReply(1, 0).Push(written ? kResultSuccess : kResultInvalidBufferDescriptor);
    }

    void ReadPipeIfPossible(Request& request) {
        const u32 pipe = request.Pop<u32>();
        const u32 peer = request.Pop<u32>();
        auto bytes = device_->ReadPipe(pipe, peer, request.Pop<u16>());
        auto& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess, static_cast<u16>(bytes.size()));
        reply.PushStaticBuffer(std::move(bytes), 0);
    }

    void LoadComponent(Request& request) {
        const u32 size = request.Pop<u32>();
        request.Pop<u16>();  // Program bank mask.
        request.Pop<u16>();  // Data bank mask.
        const auto* buffer = request.PopMappedBuffer();
        if (!buffer) {
            request.MakeReply(2, 0).Push(kResultInvalidBufferDescriptor, u32{0});
            return;
        }

        // NativeDsp implements the ABI of this DSP1 component in C++.
        std::array<char, 4> magic{};
        u32 image_size = 0;
        const bool loaded = size == kComponentSize && buffer->Size() == size &&
                            buffer->Read(magic.data(), kComponentHeaderOffset, sizeof(magic)) &&
                            magic == std::array{'D', 'S', 'P', '1'} &&
                            buffer->Read(&image_size, kComponentHeaderOffset + sizeof(magic),
                                         sizeof(image_size)) &&
                            image_size == size;
        if (loaded) {
            device_->Load();
        }
        auto& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess, static_cast<u32>(loaded));
        reply.PushMappedBuffer(*buffer);
    }

    void UnloadComponent(Request& request) {
        device_->Unload();
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void FlushDataCache(Request& request) {
        // The ARM and the DSP sample reader see the same physical backing on the scheduler thread.
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void RegisterInterruptEvents(Request& request) {
        const u32 type = request.Pop<u32>();
        const u32 pipe = request.Pop<u32>();
        auto object = request.PopGenericObject();
        auto event = ObjectCast<Event>(object);
        if (object && !event) {
            request.MakeReply(1, 0).Push(kResultInvalidHandleInCommand);
            return;
        }
        if (type == kPipeInterruptType && pipe == NativeDsp::kAudioPipe) {
            device_->SetInterrupt(std::move(event));
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void GetSemaphoreEventHandle(Request& request) {
        auto& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(device_->SemaphoreEvent());
    }

    void SetSemaphoreMask(Request& request) {
        device_->SetSemaphoreMask(request.Pop<u16>());
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void GetHeadphoneStatus(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, false);
    }

    std::shared_ptr<DspDevice> device_;
};

MH4N_REGISTER(ServiceFactory, kNewServiceName, [](ServiceContext& context) {
    const auto slot = context.State<std::shared_ptr<DspDevice>>(kNewServiceName);
    if (!*slot) {
        *slot = std::make_shared<DspDevice>(context);
    }
    return std::make_unique<DspService>(context, *slot);
});

}  // namespace
}  // namespace mh4n
