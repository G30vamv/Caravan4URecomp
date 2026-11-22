// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew NDM Services and golden boot-village.ipc.trace.

#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

class NdmUser final : public Service {
public:
    explicit NdmUser(ServiceContext& context) : Service(context, "ndm:u") {
        Register(0x0001, "EnterExclusiveState");
        Register(0x0002, "LeaveExclusiveState");
        Register(0x0003, "QueryExclusiveMode");
        Register(0x0004, "LockState");
        Register(0x0005, "UnlockState");
        // Golden seq=17 and seq=38621.
        Register(0x0006, "SuspendDaemons", &NdmUser::Acknowledge);
        // Golden seq=38627.
        Register(0x0007, "ResumeDaemons", &NdmUser::Acknowledge);
        Register(0x0008, "SuspendScheduler");
        Register(0x0009, "ResumeScheduler");
        Register(0x000A, "GetCurrentState");
        Register(0x000B, "GetTargetState");
        Register(0x000D, "QueryStatus");
        Register(0x000E, "GetDaemonDisableCount");
        Register(0x000F, "GetSchedulerDisableCount");
        Register(0x0010, "SetScanInterval");
        Register(0x0011, "GetScanInterval");
        Register(0x0012, "SetRetryInterval");
        Register(0x0013, "GetRetryInterval");
        // Golden seq=16.
        Register(0x0014, "OverrideDefaultDaemons", &NdmUser::Acknowledge);
        Register(0x0015, "ResetDefaultDaemons");
        Register(0x0016, "GetDefaultDaemons");
        Register(0x0017, "ClearHalfAwakeMacFilter");
    }

private:
    void Acknowledge(Request& request) {
        request.MakeReply(1, 0).Push(u32{0});
    }
};

MH4N_REGISTER(ServiceFactory, "new/ndm:u", [](ServiceContext& context) {
    return std::make_unique<NdmUser>(context);
});

} // namespace
} // namespace mh4n
