// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew NIM Services and golden boot-village.ipc.trace.

#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

class NimAoc final : public Service {
public:
    explicit NimAoc(ServiceContext& context) : Service(context, "nim:aoc") {
        Register(0x0003, "SetApplicationId");
        Register(0x0004, "SetTin");
        Register(0x0009, "ListContentSetsEx");
        Register(0x0018, "GetBalance");
        Register(0x001D, "GetCustomerSupportCode");
        Register(0x0021, "Initialize", &NimAoc::Initialize);
        Register(0x0024, "CalculateContentsRequiredSize");
        Register(0x0025, "RefreshServerTime");
    }

private:
    // Golden seq=82.
    void Initialize(Request& request) {
        request.MakeReply(1, 0).Push(u32{0});
    }
};

MH4N_REGISTER(ServiceFactory, "new/nim:aoc", [](ServiceContext& context) {
    return std::make_unique<NimAoc>(context);
});

} // namespace
} // namespace mh4n
