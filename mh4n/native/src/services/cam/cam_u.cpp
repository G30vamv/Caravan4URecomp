// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Camera Services, libctru cam.h, other_services_test.cpp, IPC replay observations and the IPC/kernel contracts.

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

constexpr Result kInvalidEnum = MakeResult(1005, 20, 7, 28);
constexpr Result kOutOfRange = MakeResult(1021, 20, 7, 28);
constexpr u32 kBytesPerPixel = 2;

struct ImageSize {
    u16 width;
    u16 height;
};

constexpr std::array<ImageSize, 8> kSizes{{
    {640, 480}, {320, 240}, {160, 120}, {352, 288},
    {176, 144}, {256, 192}, {512, 384}, {400, 240},
}};

struct Trim {
    u16 left = 0;
    u16 top = 0;
    u16 right = 0;
    u16 bottom = 0;
};

struct CaptureFormat {
    ImageSize size = kSizes[0];
    bool rgb = false;
};

struct Camera {
    std::array<CaptureFormat, 2> formats{};
    unsigned current_context = 0;
    unsigned frame_ms = 67;
};

struct Receive {
    u32 address;
    u32 size;
};

struct Port {
    std::shared_ptr<Event> vsync;
    std::shared_ptr<Event> buffer_error;
    std::shared_ptr<Event> completion;
    std::array<s64, 5> times{};
    std::optional<Receive> receive;
    std::optional<unsigned> camera;
    u64 vsync_timer = 0;
    u64 frame_timer = 0;
    u32 transfer_bytes = 256;
    Trim trim;
    bool trimming = false;
    bool busy = false;
};

class Cam final : public Service {
public:
    explicit Cam(ServiceContext& context) : Service(context, "cam:u") {
        Register(0x01, "StartCapture", &Cam::StartCapture);
        Register(0x02, "StopCapture", &Cam::StopCapture);
        Register(0x03, "IsBusy", &Cam::IsBusy);
        Register(0x04, "ClearBuffer", &Cam::ClearBuffer);
        Register(0x05, "GetVsyncInterruptEvent", &Cam::GetInterruptEvent);
        Register(0x06, "GetBufferErrorInterruptEvent", &Cam::GetInterruptEvent);
        Register(0x07, "SetReceiving", &Cam::SetReceiving);
        Register(0x08, "IsFinishedReceiving", &Cam::IsFinishedReceiving);
        Register(0x09, "SetTransferLines", &Cam::SetTransfer);
        Register(0x0A, "GetMaxLines", &Cam::GetMaxTransfer);
        Register(0x0B, "SetTransferBytes", &Cam::SetTransfer);
        Register(0x0C, "GetTransferBytes", &Cam::GetTransferBytes);
        Register(0x0D, "GetMaxBytes", &Cam::GetMaxTransfer);
        Register(0x0E, "SetTrimming", &Cam::SetTrimming);
        Register(0x0F, "IsTrimming", &Cam::IsTrimming);
        Register(0x10, "SetTrimmingParams", &Cam::SetTrimmingParams);
        Register(0x11, "GetTrimmingParams", &Cam::GetTrimmingParams);
        Register(0x12, "SetTrimmingParamsCenter", &Cam::SetTrimmingParams);
        Register(0x13, "Activate", &Cam::Activate);
        Register(0x14, "SwitchContext", &Cam::SwitchContext);
        Register(0x15, "SetExposure");
        Register(0x16, "SetWhiteBalance");
        Register(0x17, "SetWhiteBalanceWithoutBaseUp");
        Register(0x18, "SetSharpness");
        Register(0x19, "SetAutoExposure");
        Register(0x1A, "IsAutoExposure");
        Register(0x1B, "SetAutoWhiteBalance");
        Register(0x1C, "IsAutoWhiteBalance");
        Register(0x1D, "FlipImage", &Cam::CheckFormatSelection);
        Register(0x1E, "SetDetailSize", &Cam::SetSize);
        Register(0x1F, "SetSize", &Cam::SetSize);
        Register(0x20, "SetFrameRate", &Cam::SetFrameRate);
        Register(0x21, "SetPhotoMode");
        Register(0x22, "SetEffect", &Cam::CheckFormatSelection);
        Register(0x23, "SetContrast");
        Register(0x24, "SetLensCorrection");
        Register(0x25, "SetOutputFormat", &Cam::SetOutputFormat);
        Register(0x26, "SetAutoExposureWindow");
        Register(0x27, "SetAutoWhiteBalanceWindow");
        Register(0x28, "SetNoiseFilter");
        Register(0x29, "SynchronizeVsyncTiming", &Cam::Acknowledge);
        Register(0x2A, "GetLatestVsyncTiming", &Cam::GetLatestVsyncTiming);
        Register(0x2B, "GetStereoCameraCalibrationData", &Cam::GetCalibration);
        Register(0x2C, "SetStereoCameraCalibrationData");
        Register(0x2D, "WriteRegisterI2c");
        Register(0x2E, "WriteMcuVariableI2c");
        Register(0x2F, "ReadRegisterI2cExclusive");
        Register(0x30, "ReadMcuVariableI2cExclusive");
        Register(0x31, "SetImageQualityCalibrationData");
        Register(0x32, "GetImageQualityCalibrationData");
        Register(0x33, "SetPackageParameterWithoutContext", &Cam::Acknowledge);
        Register(0x34, "SetPackageParameterWithContext", &Cam::SetPackageSize);
        Register(0x35, "SetPackageParameterWithContextDetail", &Cam::SetPackageSize);
        Register(0x36, "GetSuitableY2rStandardCoefficient", &Cam::GetStandardCoefficient);
        Register(0x37, "PlayShutterSoundWithWave");
        Register(0x38, "PlayShutterSound", &Cam::Acknowledge);
        Register(0x39, "DriverInitialize", &Cam::Reset);
        Register(0x3A, "DriverFinalize", &Cam::Reset);
        Register(0x3B, "GetActivatedCamera");

        ResetPorts();
    }

    ~Cam() override {
        for (auto& port : ports_) {
            Cancel(port.vsync_timer);
            Cancel(port.frame_timer);
        }
    }

private:
    void Acknowledge(Request& request) {
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void Cancel(u64& timer) {
        if (timer != 0) {
            Context().GetKernel().Cancel(timer);
            timer = 0;
        }
    }

    void ResetPorts() {
        for (auto& port : ports_) {
            Cancel(port.vsync_timer);
            Cancel(port.frame_timer);
            port = {};
            auto& kernel = Context().GetKernel();
            port.vsync = kernel.NewEvent(ResetType::OneShot);
            port.buffer_error = kernel.NewEvent(ResetType::OneShot);
            port.completion = kernel.NewEvent(ResetType::Sticky);
        }
    }

    void Reset(Request& request) {
        ResetPorts();
        cameras_ = {};
        Acknowledge(request);
    }

    static bool Selected(u32 mask, unsigned index) {
        return (mask & (1u << index)) != 0;
    }

    template <class Apply>
    void WithPorts(Request& request, u32 mask, Apply apply) {
        if (mask > 3) {
            request.MakeReply(1, 0).Push(kInvalidEnum);
            return;
        }
        for (unsigned i = 0; i < ports_.size(); ++i) {
            if (Selected(mask, i)) {
                apply(i, ports_[i]);
            }
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    Port* FindPort(u32 mask) {
        return mask == 1 || mask == 2 ? &ports_[mask - 1] : nullptr;
    }

    u64 FramePeriod(const Port& port) const {
        return kTicksPerSecond * cameras_[*port.camera].frame_ms / 1000;
    }

    void ScheduleVsync(unsigned index, u64 tick) {
        Port& port = ports_[index];
        port.vsync_timer = Context().GetKernel().ScheduleAt(tick, [this, index, tick] {
            Port& current = ports_[index];
            std::move_backward(current.times.begin(), current.times.end() - 1, current.times.end());
            current.times.front() = static_cast<s64>(TicksToNs(tick) / 1000);
            ScheduleVsync(index, tick + FramePeriod(current));
            current.vsync->Signal();
        });
    }

    void Activate(Request& request) {
        const u32 mask = request.Pop<u8>();
        // The first port is shared by the inner camera and the first outer camera.
        if (mask > 7 || (mask & 3) == 3) {
            request.MakeReply(1, 0).Push(kInvalidEnum);
            return;
        }
        std::array<std::optional<unsigned>, 2> cameras;
        if (mask & 3) {
            cameras[0] = Selected(mask, 0) ? 0 : 1;
        }
        if (Selected(mask, 2)) {
            cameras[1] = 2;
        }
        const u64 now = Context().GetKernel().NowTicks();
        for (unsigned i = 0; i < ports_.size(); ++i) {
            Port& port = ports_[i];
            if (port.camera == cameras[i]) {
                continue;
            }
            Cancel(port.vsync_timer);
            StopPort(port);
            port.times.fill(0);
            port.vsync->Clear();
            port.camera = cameras[i];
            if (port.camera) {
                ScheduleVsync(i, now + FramePeriod(port));
            }
        }
        Acknowledge(request);
    }

    void StartCapture(Request& request) {
        const u32 mask = request.Pop<u8>();
        WithPorts(request, mask, [this](unsigned index, Port& port) {
            if (port.camera && !port.busy) {
                port.busy = true;
                ScheduleFrame(index);
            }
        });
    }

    void StopPort(Port& port) {
        port.busy = false;
        Cancel(port.frame_timer);
        port.receive.reset();
    }

    void StopCapture(Request& request) {
        const u32 mask = request.Pop<u8>();
        WithPorts(request, mask, [this](unsigned, Port& port) { StopPort(port); });
    }

    void ClearBuffer(Request& request) {
        const u32 mask = request.Pop<u8>();
        if (mask > 3) {
            Acknowledge(request);
            return;
        }
        WithPorts(request, mask, [this](unsigned, Port& port) {
            Cancel(port.frame_timer);
            port.receive.reset();
            port.buffer_error->Clear();
        });
    }

    void IsBusy(Request& request) {
        const u32 mask = request.Pop<u8>();
        const bool busy = mask <= 3 && (!Selected(mask, 0) || ports_[0].busy) &&
                          (!Selected(mask, 1) || ports_[1].busy);
        request.MakeReply(2, 0).Push(mask <= 3 ? kResultSuccess : kInvalidEnum, busy);
    }

    void GetInterruptEvent(Request& request) {
        const Port* port = FindPort(request.Pop<u8>());
        std::shared_ptr<Event> event;
        if (port) {
            event = request.CommandId() == 0x05 ? port->vsync : port->buffer_error;
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(port ? kResultSuccess : kInvalidEnum);
        reply.PushCopyObjects(std::move(event));
    }

    void SetReceiving(Request& request) {
        const u32 address = request.Pop<u32>();
        Port* port = FindPort(request.Pop<u8>());
        const u32 size = request.Pop<u32>();
        if (port) {
            Cancel(port->frame_timer);
            port->completion->Clear();
            port->receive = Receive{address, size};
            ScheduleFrame(static_cast<unsigned>(port - ports_.data()));
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(port ? kResultSuccess : kInvalidEnum);
        reply.PushCopyObjects(port ? port->completion : std::shared_ptr<Event>{});
    }

    void IsFinishedReceiving(Request& request) {
        const Port* port = FindPort(request.Pop<u8>());
        request.MakeReply(2, 0).Push(port ? kResultSuccess : kInvalidEnum, port && !port->receive);
    }

    void ScheduleFrame(unsigned index) {
        Port& port = ports_[index];
        if (!port.camera || !port.busy || !port.receive) {
            return;
        }
        const u64 tick = Context().GetKernel().NowTicks() + FramePeriod(port);
        port.frame_timer = Context().GetKernel().ScheduleAt(tick, [this, index] {
            Port& current = ports_[index];
            current.frame_timer = 0;
            const Receive receive = *current.receive;
            current.receive.reset();
            WriteFrame(current, receive);
            current.completion->Signal();
        });
    }

    void WriteFrame(Port& port, Receive receive) {
        const Camera& camera = cameras_[*port.camera];
        const CaptureFormat& format = camera.formats[camera.current_context];
        u32 pixels = static_cast<u32>(format.size.width) * format.size.height;
        if (port.trimming) {
            const Trim& trim = port.trim;
            if (trim.left >= trim.right || trim.top >= trim.bottom ||
                trim.right > format.size.width || trim.bottom > format.size.height) {
                port.buffer_error->Signal();
                return;
            }
            pixels = static_cast<u32>(trim.right - trim.left) * (trim.bottom - trim.top);
        }
        const auto size = static_cast<u32>(
            std::min<u64>(static_cast<u64>(pixels) * kBytesPerPixel, receive.size));
        auto& memory = Context().Memory();
        if (!memory.IsMapped(receive.address, size)) {
            port.buffer_error->Signal();
            return;
        }
        constexpr u32 block_size = 4096;
        std::array<u8, block_size> block{};
        if (!format.rgb) {
            for (u32 i = 1; i < block_size; i += 2) {
                block[i] = 0x80;
            }
        }
        for (u32 offset = 0; offset < size;) {
            const u32 count = std::min(size - offset, block_size);
            if (!memory.WriteBlock(receive.address + offset, block.data(), count)) {
                port.buffer_error->Signal();
                return;
            }
            offset += count;
        }
    }

    void SetTransfer(Request& request) {
        const u32 mask = request.Pop<u8>();
        const u32 unit = request.Pop<u32>();
        const u32 width = request.Pop<u16>();
        request.Pop<u16>();
        const bool lines = request.CommandId() == 0x09;
        const u32 bytes = lines ? (unit & 0xFFFF) * width * kBytesPerPixel : unit;
        WithPorts(request, mask, [bytes](unsigned, Port& port) { port.transfer_bytes = bytes; });
    }

    void GetTransferBytes(Request& request) {
        const Port* port = FindPort(request.Pop<u8>());
        request.MakeReply(2, 0).Push(port ? kResultSuccess : kInvalidEnum,
                                    port ? port->transfer_bytes : 0u);
    }

    void GetMaxTransfer(Request& request) {
        const u32 width = request.Pop<u16>();
        const u32 height_word = request.Pop<u32>();
        const u32 height = static_cast<u16>(height_word);
        const u32 row_bytes = width * kBytesPerPixel;
        const u64 total = static_cast<u64>(row_bytes) * height;
        if (total == 0 || total % 256 != 0) {
            request.MakeReply(2, 0).Push(kOutOfRange, height_word);
            return;
        }
        if (request.CommandId() == 0x0D) {
            u32 bytes = 2560;
            while (total % bytes != 0) {
                bytes -= 256;
            }
            request.MakeReply(2, 0).Push(kResultSuccess, bytes);
        } else {
            u32 rows = std::min(height, 5120u / row_bytes);
            while (rows > 0 && (height % rows != 0 || (rows * row_bytes) % 256 != 0)) {
                --rows;
            }
            if (rows == 0) {
                request.MakeReply(2, 0).Push(kOutOfRange, 0u);
            } else {
                request.MakeReply(2, 0).Push(kResultSuccess, static_cast<u16>(rows));
            }
        }
    }

    void SetTrimming(Request& request) {
        const u32 mask = request.Pop<u8>();
        const bool enabled = request.Pop<bool>();
        WithPorts(request, mask, [enabled](unsigned, Port& port) { port.trimming = enabled; });
    }

    void IsTrimming(Request& request) {
        const Port* port = FindPort(request.Pop<u8>());
        request.MakeReply(2, 0).Push(port ? kResultSuccess : kInvalidEnum, port && port->trimming);
    }

    void SetTrimmingParams(Request& request) {
        const u32 mask = request.Pop<u8>();
        const u16 a = request.Pop<u16>();
        const u16 b = request.Pop<u16>();
        const u16 c = request.Pop<u16>();
        const u16 d = request.Pop<u16>();
        Trim trim{a, b, c, d};
        if (request.CommandId() == 0x12) {
            trim.left = static_cast<u16>((c - a) / 2);
            trim.top = static_cast<u16>((d - b) / 2);
            trim.right = static_cast<u16>(trim.left + a);
            trim.bottom = static_cast<u16>(trim.top + b);
        }
        WithPorts(request, mask, [trim](unsigned, Port& port) { port.trim = trim; });
    }

    void GetTrimmingParams(Request& request) {
        const Port* port = FindPort(request.Pop<u8>());
        const Trim trim = port ? port->trim : Trim{};
        request.MakeReply(5, 0).Push(port ? kResultSuccess : kInvalidEnum,
                                    trim.left, trim.top, trim.right, trim.bottom);
    }

    void SwitchContext(Request& request) {
        const u32 mask = request.Pop<u8>();
        const u32 context = request.Pop<u8>();
        if (mask > 7 || (context != 1 && context != 2)) {
            request.MakeReply(1, 0).Push(kInvalidEnum);
            return;
        }
        for (unsigned i = 0; i < cameras_.size(); ++i) {
            if (Selected(mask, i)) {
                cameras_[i].current_context = context - 1;
            }
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    template <class Apply>
    void WithFormats(Request& request, u32 cameras, u32 contexts, Apply apply) {
        if (cameras > 7 || contexts > 3) {
            request.MakeReply(1, 0).Push(kInvalidEnum);
            return;
        }
        for (unsigned i = 0; i < cameras_.size(); ++i) {
            for (unsigned j = 0; Selected(cameras, i) && j < cameras_[i].formats.size(); ++j) {
                if (Selected(contexts, j)) {
                    apply(cameras_[i].formats[j]);
                }
            }
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void SetSize(Request& request) {
        const u32 cameras = request.Pop<u8>();
        ImageSize size{};
        if (request.CommandId() == 0x1E) {
            size.width = request.Pop<u16>();
            size.height = request.Pop<u16>();
            for (unsigned i = 0; i < 4; ++i) {
                request.Pop<u16>();  // Cropping a blank image cannot change its pixels.
            }
        } else {
            const u32 preset = request.Pop<u8>();
            if (preset >= kSizes.size()) {
                request.MakeReply(1, 0).Push(kInvalidEnum);
                return;
            }
            size = kSizes[preset];
        }
        const u32 contexts = request.Pop<u8>();
        WithFormats(request, cameras, contexts, [size](CaptureFormat& format) {
            format.size = size;
        });
    }

    void SetOutputFormat(Request& request) {
        const u32 cameras = request.Pop<u8>();
        const u32 output = request.Pop<u8>();
        const u32 contexts = request.Pop<u8>();
        WithFormats(request, cameras, contexts, [output](CaptureFormat& format) {
            format.rgb = output == 1;
        });
    }

    void CheckFormatSelection(Request& request) {
        const u32 cameras = request.Pop<u8>();
        request.Pop<u8>();
        const u32 contexts = request.Pop<u8>();
        request.MakeReply(1, 0).Push(cameras <= 7 && contexts <= 3 ? kResultSuccess : kInvalidEnum);
    }

    void SetPackageSize(Request& request) {
        const u32 selection = request.Pop<u32>();
        const u32 cameras = selection & 0xFF;
        const u32 contexts = (selection >> 8) & 0xFF;
        ImageSize size{};
        if (request.CommandId() == 0x34) {
            const u32 preset = request.Pop<u8>();
            if (preset >= kSizes.size()) {
                request.MakeReply(1, 0).Push(kInvalidEnum);
                return;
            }
            size = kSizes[preset];
        } else {
            const u32 dimensions = request.Pop<u32>();
            size = {static_cast<u16>(dimensions), static_cast<u16>(dimensions >> 16)};
        }
        WithFormats(request, cameras, contexts, [size](CaptureFormat& format) {
            format.size = size;
        });
    }

    void GetStandardCoefficient(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, 0u);
    }

    void SetFrameRate(Request& request) {
        constexpr std::array periods{67u, 67u, 67u, 100u, 118u, 200u, 50u, 50u, 33u, 33u,
                                     67u, 50u, 33u};
        const u32 mask = request.Pop<u8>();
        const u32 rate = request.Pop<u8>();
        if (mask > 7) {
            request.MakeReply(1, 0).Push(kInvalidEnum);
            return;
        }
        for (unsigned i = 0; i < cameras_.size(); ++i) {
            if (Selected(mask, i) && rate < periods.size()) {
                cameras_[i].frame_ms = periods[rate];
            }
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void GetLatestVsyncTiming(Request& request) {
        const Port* port = FindPort(request.Pop<u8>());
        const u32 count = request.Pop<u32>();
        const bool valid = port && count <= port->times.size();
        std::vector<u8> data(valid ? count * sizeof(s64) : 0);
        if (!data.empty()) {
            std::memcpy(data.data(), port->times.data(), data.size());
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(valid ? kResultSuccess : kOutOfRange);
        reply.PushStaticBuffer(std::move(data), 0);
    }

    void GetCalibration(Request& request) {
        constexpr std::array<u32, 16> calibration{
            0x00000000, 0x3F803A32, 0x3C085CCD, 0xC2AF68E1,
            0xC0F482E2, 0x00000000, 0x00000000, 0x42815666,
            0x42818577, 0x437A0000, 0x420C0000, 0x01E00280,
            0x00000000, 0x00000000, 0x00000000, 0x00000000,
        };
        Reply& reply = request.MakeReply(17, 0);
        reply.Push(kResultSuccess);
        reply.PushRaw(calibration);
    }

    std::array<Camera, 3> cameras_{};
    std::array<Port, 2> ports_{};
};

MH4N_REGISTER(ServiceFactory, "cam:u", ServiceFactory([](ServiceContext& context) {
    return std::make_unique<Cam>(context);
}, 1));

}  // namespace
}  // namespace mh4n
