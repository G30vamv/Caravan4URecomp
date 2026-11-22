// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Friend Services and golden boot-village.ipc.trace.

#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

class FrdUser final : public Service {
public:
    explicit FrdUser(ServiceContext& context) : Service(context, "frd:u") {
        Register(0x0001, "HasLoggedIn");
        Register(0x0002, "IsOnline");
        Register(0x0003, "Login");
        Register(0x0004, "Logout");
        Register(0x0005, "GetMyFriendKey");
        Register(0x0006, "GetMyPreference");
        Register(0x0007, "GetMyProfile");
        Register(0x0008, "GetMyPresence", &FrdUser::GetMyPresence);
        Register(0x0009, "GetMyScreenName");
        Register(0x000A, "GetMyMii");
        Register(0x000B, "GetMyLocalAccountId");
        Register(0x000C, "GetMyPlayingGame");
        Register(0x000D, "GetMyFavoriteGame");
        Register(0x000E, "GetMyNcPrincipalId");
        Register(0x000F, "GetMyComment");
        Register(0x0010, "GetMyPassword");
        Register(0x0011, "GetFriendKeyList");
        Register(0x0012, "GetFriendPresence");
        Register(0x0013, "GetFriendScreenName");
        Register(0x0014, "GetFriendMii");
        Register(0x0015, "GetFriendProfile");
        Register(0x0016, "GetFriendRelationship");
        Register(0x0017, "GetFriendAttributeFlags");
        Register(0x0018, "GetFriendPlayingGame");
        Register(0x0019, "GetFriendFavoriteGame");
        Register(0x001A, "GetFriendInfo");
        Register(0x001B, "IsIncludedInFriendList");
        Register(0x001C, "UnscrambleLocalFriendCode");
        Register(0x001D, "UpdateGameModeDescription");
        Register(0x001E, "UpdateMyPresence");
        Register(0x001F, "SendInvitation");
        Register(0x0020, "AttachToEventNotification");
        Register(0x0021, "SetNotificationMask");
        Register(0x0022, "GetEventNotification");
        Register(0x0023, "GetLastResponseResult");
        Register(0x0024, "PrincipalIdToFriendCode");
        Register(0x0025, "FriendCodeToPrincipalId");
        Register(0x0026, "IsValidFriendCode");
        Register(0x0027, "ResultToErrorCode");
        Register(0x0028, "RequestGameAuthentication");
        Register(0x0029, "GetGameAuthenticationData");
        Register(0x002A, "RequestServiceLocator");
        Register(0x002B, "GetServiceLocatorData");
        Register(0x002C, "DetectNatProperties");
        Register(0x002D, "GetNatProperties");
        Register(0x002E, "GetServerTimeDifference");
        Register(0x002F, "AllowHalfAwake");
        Register(0x0030, "GetServerTypes");
        Register(0x0031, "GetFriendComment");
        Register(0x0032, "SetClientSdkVersion", &FrdUser::SetClientSdkVersion);
        Register(0x0033, "GetMyApproachContext");
        Register(0x0034, "AddFriendWithApproach");
        Register(0x0035, "DecryptApproachContext");
        Register(0x0036, "GetExtendedNatProperties");
    }

private:
    // Golden seq=256: 300 zero bytes (CRC32 0xb5348fd2) in static receive buffer 0.
    void GetMyPresence(Request& request) {
        constexpr u32 kPresenceSize = 300;
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(u32{0});
        reply.PushStaticBuffer(std::vector<u8>(kPresenceSize), 0);
    }

    // Golden seq=255.
    void SetClientSdkVersion(Request& request) {
        request.MakeReply(1, 0).Push(u32{0});
    }
};

MH4N_REGISTER(ServiceFactory, "new/frd:u", [](ServiceContext& context) {
    return std::make_unique<FrdUser>(context);
});

} // namespace
} // namespace mh4n
