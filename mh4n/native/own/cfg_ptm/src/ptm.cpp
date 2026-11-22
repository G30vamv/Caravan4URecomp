// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew PTM Services and its user command descriptions, golden boot-village.ipc.trace seq=48.

#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

class PtmUser final : public Service {
public:
    explicit PtmUser(ServiceContext& context) : Service(context, "ptm:u") {
        // Command IDs and reply types: 3dbrew PTM Services and linked command pages.
        Register(0x0001, "RegisterAlarmClient");
        Register(0x0002, "SetRtcAlarm");
        Register(0x0003, "GetRtcAlarm");
        Register(0x0004, "CancelRtcAlarm");
        Register(0x0005, "GetAdapterState", &PtmUser::ReplyValue<false>);
        Register(0x0006, "GetShellState", &PtmUser::ReplyValue<true>);
        Register(0x0007, "GetBatteryLevel", &PtmUser::ReplyValue<u8{5}>);
        Register(0x0008, "GetBatteryChargeState", &PtmUser::ReplyValue<false>);
        Register(0x0009, "GetPedometerState", &PtmUser::ReplyValue<false>);
        Register(0x000A, "GetStepHistoryEntry");
        Register(0x000B, "GetStepHistory");
        Register(0x000C, "GetTotalStepCount", &PtmUser::ReplyValue<u32{0}>);
        Register(0x000D, "SetPedometerRecordingMode");
        Register(0x000E, "GetPedometerRecordingMode");
        Register(0x000F, "GetStepHistoryAll");
    }

private:
    template <auto value>
    void ReplyValue(Request& request) {
        request.MakeReply(2, 0).Push(Result{0}, value);
    }
};

MH4N_REGISTER(ServiceFactory, "new/ptm:u", [](ServiceContext& context) {
    return std::make_unique<PtmUser>(context);
});

}  // namespace
}  // namespace mh4n
