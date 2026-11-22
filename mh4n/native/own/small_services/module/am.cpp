// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Application Manager Services and golden boot-village.ipc.trace seq=80.

#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

class AmApp final : public Service {
public:
    explicit AmApp(ServiceContext& context) : Service(context, "am:app") {
        Register(0x1001, "GetDLCContentInfoCount");
        Register(0x1002, "FindDLCContentInfos");
        Register(0x1003, "ListDLCContentInfos");
        Register(0x1004, "DeleteDLCContents");
        Register(0x1005, "GetDLCTitleInfos");
        Register(0x1006, "GetDLCOrLicenseNumTickets");
        Register(0x1007, "ListDLCOrLicenseTicketInfos");
        Register(0x1008, "GetDLCOrLicenseItemRights");
        Register(0x1009, "IsDLCTitleInUse");
        Register(0x100A, "ReloadSDTitleDatabase");
        Register(0x100B, "GetNumDLCExistingContentInfos");
        Register(0x100C, "ListDLCExistingContentInfos");
        Register(0x100D, "GetPatchTitleInfos");
    }
};

MH4N_REGISTER(ServiceFactory, "new/am:app", [](ServiceContext& context) {
    return std::make_unique<AmApp>(context);
});

} // namespace
} // namespace mh4n
