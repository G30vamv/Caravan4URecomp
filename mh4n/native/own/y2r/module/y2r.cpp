// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Camera Services/DMA, libctru y2r.h and boot-village IPC.

#include "y2r_conversion.h"

#include <limits>
#include <optional>
#include <vector>

#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

using namespace y2r;

// libctru: image_size counts payload bytes, and the (signed) gap is applied
// after each DMA unit. Trace seq=13259-13262 also shows padding above each s16.
struct Transfer {
    u32 address = 0;
    u32 size = 0;
    s16 unit = 0;
    s16 gap = 0;

    template <typename Operation>
    bool ForEachChunk(u32 bytes, Operation operation) const {
        const s32 stride = s32{unit} + gap;
        if (bytes > size || unit <= 0 || stride <= 0 || stride >= 0x8000) {
            return false;
        }
        const u32 chunk_size = gap == 0 ? bytes : static_cast<u32>(unit);
        for (u32 offset = 0; offset < bytes; offset += chunk_size) {
            const u64 guest = address + u64{offset / chunk_size} * static_cast<u32>(stride);
            const u32 count = std::min(chunk_size, bytes - offset);
            if (guest + count > u64{std::numeric_limits<u32>::max()} + 1 ||
                !operation(static_cast<u32>(guest), offset, count)) {
                return false;
            }
        }
        return true;
    }
};

enum Buffer : u8 { Y, U, V, Yuyv, RgbOutput, BufferCount };

struct Y2rState {
    Settings settings;
    std::array<Transfer, BufferCount> buffers;
    std::array<bool, BufferCount> finished{};
    std::shared_ptr<Event> completion;
    std::optional<u64> pending;
    bool initialized = false;
    bool interrupt = true;
};

bool ValidWidth(u16 width) {
    return width != 0 && width <= 1024 && width % 8 == 0;
}

bool ValidLines(u16 lines) {
    return lines != 0 && lines <= 1024;
}

bool ConvertFrame(GuestMemory& memory, const Settings& settings,
                  const std::array<Transfer, BufferCount>& buffers) {
    const bool packed = settings.input == 4;
    const auto sizes = InputSizes(settings);
    const u32 output_size = u32{settings.width} * settings.lines *
                            kBytesPerPixel[settings.output];
    const auto& destination = buffers[RgbOutput];

    // Validate the complete destination before writing, so an unmapped DMA chunk
    // cannot leave a partially converted frame in guest memory.
    if (!destination.ForEachChunk(output_size, [&](u32 address, u32, u32 size) {
            return memory.IsMapped(address, size);
        })) {
        return false;
    }

    std::array<std::vector<u8>, 3> planes;
    for (u32 plane = 0; plane < (packed ? 1u : 3u); ++plane) {
        auto& data = planes[plane];
        const auto& source = buffers[packed ? static_cast<u32>(Yuyv) : plane];
        data.resize(sizes[plane]);
        if (!source.ForEachChunk(sizes[plane], [&](u32 address, u32 offset, u32 size) {
                return memory.ReadBlock(data.data() + offset, address, size);
            })) {
            return false;
        }
    }

    std::vector<u8> output(output_size);
    Convert(settings, planes[0], planes[1], planes[2], output);
    return destination.ForEachChunk(output_size, [&](u32 address, u32 offset, u32 size) {
        return memory.WriteBlock(address, output.data() + offset, size);
    });
}

class Y2rUser final : public Service {
public:
    explicit Y2rUser(ServiceContext& context)
        : Service(context, "y2r:u"), state_(context.State<Y2rState>("new/y2r:u")) {
        // Camera Services IPC table. Game configurations appear at seq=13250-13263.
        Register(0x0001, "SetInputFormat", &Y2rUser::SetField<&Settings::input, 4>);
        Register(0x0002, "GetInputFormat", &Y2rUser::GetField<&Settings::input>);
        Register(0x0003, "SetOutputFormat", &Y2rUser::SetField<&Settings::output, 3>);
        Register(0x0004, "GetOutputFormat", &Y2rUser::GetField<&Settings::output>);
        Register(0x0005, "SetRotation", &Y2rUser::SetField<&Settings::rotation, 3>);
        Register(0x0006, "GetRotation", &Y2rUser::GetField<&Settings::rotation>);
        Register(0x0007, "SetBlockAlignment", &Y2rUser::SetField<&Settings::block_alignment, 1>);
        Register(0x0008, "GetBlockAlignment", &Y2rUser::GetField<&Settings::block_alignment>);
        Register(0x0009, "SetSpacialDithering");
        Register(0x000a, "GetSpacialDithering");
        Register(0x000b, "SetTemporalDithering");
        Register(0x000c, "GetTemporalDithering");
        Register(0x000d, "SetTransferEndInterrupt", &Y2rUser::SetInterrupt);
        Register(0x000e, "GetTransferEndInterrupt", &Y2rUser::GetInterrupt);
        Register(0x000f, "GetTransferEndEvent", &Y2rUser::GetEvent);
        Register(0x0010, "SetSendingY", &Y2rUser::SetBuffer<Y>);
        Register(0x0011, "SetSendingU", &Y2rUser::SetBuffer<U>);
        Register(0x0012, "SetSendingV", &Y2rUser::SetBuffer<V>);
        Register(0x0013, "SetSendingYuv", &Y2rUser::SetBuffer<Yuyv>);
        Register(0x0014, "IsFinishedSendingYuv", &Y2rUser::IsFinished<Yuyv>);
        Register(0x0015, "IsFinishedSendingY", &Y2rUser::IsFinished<Y>);
        Register(0x0016, "IsFinishedSendingU", &Y2rUser::IsFinished<U>);
        Register(0x0017, "IsFinishedSendingV", &Y2rUser::IsFinished<V>);
        Register(0x0018, "SetReceiving", &Y2rUser::SetBuffer<RgbOutput>);
        Register(0x0019, "IsFinishedReceiving", &Y2rUser::IsFinished<RgbOutput>);
        Register(0x001a, "SetInputLineWidth", &Y2rUser::SetDimensions<&Settings::width, ValidWidth>);
        Register(0x001b, "GetInputLineWidth", &Y2rUser::GetField<&Settings::width>);
        Register(0x001c, "SetInputLines", &Y2rUser::SetDimensions<&Settings::lines, ValidLines>);
        Register(0x001d, "GetInputLines", &Y2rUser::GetField<&Settings::lines>);
        Register(0x001e, "SetCoefficientParams", &Y2rUser::SetCoefficients);
        Register(0x001f, "GetCoefficientParams", &Y2rUser::GetCoefficients);
        Register(0x0020, "SetStandardCoefficient", &Y2rUser::SetStandard);
        Register(0x0021, "GetStandardCoefficientParams", &Y2rUser::GetStandard);
        Register(0x0022, "SetAlpha", &Y2rUser::SetField<&Settings::alpha, 0xffff>);
        Register(0x0023, "GetAlpha", &Y2rUser::GetField<&Settings::alpha>);
        Register(0x0024, "SetDitheringWeightParams");
        Register(0x0025, "GetDitheringWeightParams");
        Register(0x0026, "StartConversion", &Y2rUser::Start);
        Register(0x0027, "StopConversion", &Y2rUser::Stop);
        Register(0x0028, "IsBusyConversion", &Y2rUser::IsBusy);
        Register(0x0029, "SetPackageParameter");
        Register(0x002a, "PingProcess", &Y2rUser::Ping);
        Register(0x002b, "DriverInitialize", &Y2rUser::Initialize);
        Register(0x002c, "DriverFinalize", &Y2rUser::Finalize);
        Register(0x002d, "GetPackageParameter");
    }

private:
    static void ResultOnly(Request& request, Result result = kResultSuccess) {
        request.MakeReply(1, 0).Push(result);
    }

    template <auto Field, auto Max>
    void SetField(Request& request) {
        using Type = std::remove_reference_t<decltype(state_->settings.*Field)>;
        const auto value = request.Pop<Type>();
        if (value > Max) {
            ResultOnly(request, kResultOutOfRangeKernel);
            return;
        }
        state_->settings.*Field = value;
        ResultOnly(request);
    }

    template <auto Field, bool (*Valid)(u16)>
    void SetDimensions(Request& request) {
        const u16 value = request.Pop<u16>();
        if (!Valid(value)) {
            ResultOnly(request, kResultOutOfRangeKernel);
            return;
        }
        state_->settings.*Field = value;
        ResultOnly(request);
    }

    template <auto Field>
    void GetField(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, state_->settings.*Field);
    }

    // Four little-endian words contain eight coefficients (seq=13257-13258).
    void SetCoefficients(Request& request) {
        state_->settings.coefficients = request.PopRaw<Coefficients>();
        ResultOnly(request);
    }

    static void ReplyCoefficients(Request& request, const Coefficients& coefficients) {
        auto& reply = request.MakeReply(5, 0);
        reply.Push(kResultSuccess);
        reply.PushRaw(coefficients);
    }

    void GetCoefficients(Request& request) {
        ReplyCoefficients(request, state_->settings.coefficients);
    }

    void SetStandard(Request& request) {
        const auto index = request.Pop<u8>();
        if (index >= kPresets.size()) {
            ResultOnly(request, kResultOutOfRangeKernel);
            return;
        }
        state_->settings.coefficients = kPresets[index];
        ResultOnly(request);
    }

    void GetStandard(Request& request) {
        const auto index = request.Pop<u8>();
        if (index >= kPresets.size()) {
            ResultOnly(request, kResultOutOfRangeKernel);
            return;
        }
        ReplyCoefficients(request, kPresets[index]);
    }

    void SetInterrupt(Request& request) {
        state_->interrupt = request.Pop<bool>();
        ResultOnly(request);
    }

    void GetInterrupt(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, state_->interrupt);
    }

    std::shared_ptr<Event> CompletionEvent() {
        if (!state_->completion) {
            state_->completion = Context().GetKernel().NewEvent(ResetType::OneShot, "Y2R");
        }
        return state_->completion;
    }

    // seq=12949 returns a copied handle to the single transfer-end event.
    void GetEvent(Request& request) {
        auto& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(CompletionEvent());
    }

    template <Buffer Id>
    void SetBuffer(Request& request) {
        const auto address = request.Pop<u32>();
        const auto size = request.Pop<u32>();
        const auto unit = request.Pop<s16>();
        const auto gap = request.Pop<s16>();
        // The game supplies the current-process handle (seq=13259-13262).
        request.PopGenericObject();
        state_->buffers[Id] = {address, size, unit, gap};
        state_->finished[Id] = false;
        ResultOnly(request);
    }

    template <Buffer Id>
    void IsFinished(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, state_->finished[Id]);
    }

    // Start acknowledges the command (seq=13264). Run the DMA at the next
    // scheduler safe point and publish the result before signalling the event.
    void Start(Request& request) {
        const Settings settings = state_->settings;
        if (!state_->pending && ValidWidth(settings.width) && ValidLines(settings.lines) &&
            (settings.block_alignment == 0 || settings.lines % 8 == 0)) {
            const auto buffers = state_->buffers;
            CompletionEvent()->Clear();
            state_->finished.fill(false);
            auto& kernel = Context().GetKernel();
            state_->pending = kernel.ScheduleAt(
                kernel.NowTicks(), [state = std::weak_ptr{state_}, memory = &Context().Memory(),
                                    settings, buffers] {
                    if (auto active = state.lock()) {
                        const bool converted = ConvertFrame(*memory, settings, buffers);
                        active->pending.reset();
                        if (converted) {
                            const bool packed = settings.input == 4;
                            active->finished = {!packed, !packed, !packed, packed, true};
                            if (active->interrupt) {
                                active->completion->Signal();
                            }
                        }
                    }
                });
        }
        ResultOnly(request);
    }

    void Cancel() {
        if (state_->pending) {
            Context().GetKernel().Cancel(*state_->pending);
            state_->pending.reset();
        }
    }

    // seq=18619-18621: stop, idle reply and finalization.
    void Stop(Request& request) {
        Cancel();
        ResultOnly(request);
    }

    void IsBusy(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, state_->pending.has_value());
    }

    void Ping(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, state_->initialized);
    }

    // seq=12947 initializes the driver before configuring and copying the event.
    void Initialize(Request& request) {
        Cancel();
        state_->settings = {};
        state_->buffers = {};
        state_->finished.fill(false);
        state_->interrupt = true;
        state_->initialized = true;
        CompletionEvent()->Clear();
        ResultOnly(request);
    }

    void Finalize(Request& request) {
        Cancel();
        state_->initialized = false;
        state_->buffers = {};
        state_->finished.fill(false);
        if (state_->completion) {
            state_->completion->Clear();
        }
        ResultOnly(request);
    }

    std::shared_ptr<Y2rState> state_;
};

MH4N_REGISTER(ServiceFactory, "new/y2r:u", [](ServiceContext& context) {
    return std::make_unique<Y2rUser>(context);
});

} // namespace
} // namespace mh4n
