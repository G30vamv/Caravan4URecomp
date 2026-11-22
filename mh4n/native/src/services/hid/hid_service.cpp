// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: Service and HID contract headers, hid_test.cpp, boot-village.ipc.trace
// and 3dbrew HID Services and HID:GetIPCHandles.

#include "hid_state.h"

#include "mh4n/registry.h"
#include "mh4n/runtime.h"

namespace mh4n::hid {
namespace {

class HidService final : public Service {
public:
    HidService(ServiceContext& context, const char* name)
        : Service(context, name), hid_(GetShared(context)) {
        Register(0x01, "CalibrateTouchScreen");
        Register(0x02, "UpdateTouchConfig");
        Register(0x0A, "GetIPCHandles", &HidService::GetIPCHandles);
        Register(0x0B, "StartAnalogStickCalibration");
        Register(0x0C, "StopAnalogStickCalibration");
        Register(0x0D, "SetAnalogStickCalibrateParam");
        Register(0x0E, "GetAnalogStickCalibrateParam");
        Register(0x11, "EnableAccelerometer", &HidService::SetSensor<&Shared::EnableAccelerometer>);
        Register(0x12, "DisableAccelerometer", &HidService::SetSensor<&Shared::DisableAccelerometer>);
        Register(0x13, "EnableGyroscopeLow", &HidService::SetSensor<&Shared::EnableGyroscope>);
        Register(0x14, "DisableGyroscopeLow", &HidService::SetSensor<&Shared::DisableGyroscope>);
        Register(0x15, "GetGyroscopeLowRawToDpsCoefficient", &HidService::GetGyroscopeCoefficient);
        Register(0x16, "GetGyroscopeLowCalibrateParam", &HidService::GetGyroscopeCalibration);
        Register(0x17, "GetSoundVolume", &HidService::GetSoundVolume);
    }

private:
    void GetIPCHandles(Request& request) {
        if (!hid_->Initialised()) {
            request.MakeReply(1, 0).Push(kResultOutOfMemory);
            return;
        }
        auto& reply = request.MakeReply(1, 7);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(hid_->Memory(), hid_->PadOrTouch1(), hid_->PadOrTouch2(),
                              hid_->AccelerometerEvent(), hid_->GyroscopeEvent(), hid_->DebugPadEvent());
    }

    template <void (Shared::*Set)()>
    void SetSensor(Request& request) {
        (hid_.get()->*Set)();
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void GetGyroscopeCoefficient(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, kGyroscopeCoefficient);
    }

    void GetGyroscopeCalibration(Request& request) {
        auto& reply = request.MakeReply(6, 0);
        reply.Push(kResultSuccess);
        reply.PushRaw(DefaultGyroscopeCalibrateParam());
    }

    void GetSoundVolume(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, u32{0x3F});
    }

    std::shared_ptr<Shared> hid_;
};

MH4N_REGISTER(ServiceFactory, "hid:USER", ServiceFactory{[](ServiceContext& context) {
    return std::make_unique<HidService>(context, "hid:USER");
}, 6});
MH4N_REGISTER(ServiceFactory, "hid:SPVR", ServiceFactory{[](ServiceContext& context) {
    return std::make_unique<HidService>(context, "hid:SPVR");
}, 6});

MH4N_REGISTER(BootStep, "services.ipc.hid", BootStep{Phase::Services, [](Runtime& runtime) {
    if (auto* manager = FindServiceManager(runtime)) {
        GetShared(manager->Context());
    }
}});

}  // namespace
}  // namespace mh4n::hid
