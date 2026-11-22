// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew GSP Services and gsp_test.

#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n::gsp {
namespace {

class LcdSession final : public Service {
public:
    explicit LcdSession(ServiceContext& context) : Service(context, "gsp::Lcd") {
        Register(0x01, "EnableABL");
        Register(0x02, "DisableABL");
        Register(0x03, "SetRSLut");
        Register(0x04, "SetRSParams");
        Register(0x05, "SetABLArea");
        Register(0x07, "SetInertia");
        Register(0x08, "SetDitherMode");
        Register(0x09, "SetDitherParams");
        Register(0x0A, "SetBrightnessRaw");
        Register(0x0B, "SetBrightness");
        Register(0x0C, "ReloadConfig");
        Register(0x0D, "RestoreConfig");
        Register(0x0E, "GetPowerState");
        Register(0x0F, "PowerOnAllBacklights");
        Register(0x10, "PowerOffAllBacklights");
        Register(0x11, "PowerOnBacklight");
        Register(0x12, "PowerOffBacklight");
        Register(0x13, "SetLedForceOff");
        Register(0x14, "GetVendor");
        Register(0x15, "GetBrightness");
    }
};

MH4N_REGISTER(ServiceFactory, "gsp::Lcd",
              [](ServiceContext& context) { return std::make_unique<LcdSession>(context); });

}  // namespace
}  // namespace mh4n::gsp
