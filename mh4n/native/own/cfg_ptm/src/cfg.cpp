// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Config Services, Config Savegame, Cfg:GetSystemModel and golden boot-village.ipc.trace.

#include "cfg_save.h"

#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

constexpr Result kInvalidSize = MakeResult(1017, 64, 5, 27);
constexpr Result kNotFound = MakeResult(1018, 64, 4, 27);

struct CfgState {
    std::optional<wp11::ConfigSave> save;
};

class CfgUser final : public Service {
public:
    explicit CfgUser(ServiceContext& context)
        : Service(context, "cfg:u"), state_(context.State<CfgState>("new/cfg:u")) {
        if (!state_->save) {
            state_->save.emplace(context.UserFolder());
        }
        Register(0x0001, "GetConfigInfoBlk2", &CfgUser::GetConfigInfoBlk2);
        Register(0x0002, "SecureInfoGetRegion", &CfgUser::GetRegion);
        Register(0x0003, "GenHashConsoleUnique", &CfgUser::GetTransferableId);
        Register(0x0004, "GetRegionCanadaUSA", &CfgUser::IsCoppacsSupported);
        Register(0x0005, "GetSystemModel", &CfgUser::GetSystemModel);
        Register(0x0006, "GetModelNintendo2DS");
        Register(0x0007, "SetConfigBlock160000");
        Register(0x0008, "TranslateCountryInfo");
        Register(0x0009, "GetCountryCodeString");
        Register(0x000A, "GetCountryCodeID");
        Register(0x000B, "IsFangateSupported");
    }

private:
    void GetConfigInfoBlk2(Request& request) {
        // Golden seq=51–54, 162, 219, 225 and 38586–38587.
        const u32 count = request.Pop<u32>();
        const u32 id = request.Pop<u32>();
        const auto* buffer = request.PopMappedBuffer();
        const auto block = state_->save->ReadBlock(id, wp11::kUserRead);

        Result result = 0;
        if (!buffer || !buffer->Writable() || count > buffer->Size()) {
            result = kInvalidSize;
        } else if (block.empty()) {
            result = kNotFound;
        } else if (count != block.size() || !buffer->Write(block.data(), 0, count)) {
            result = kInvalidSize;
        }

        auto& reply = request.MakeReply(1, buffer ? 2 : 0);
        reply.Push(result);
        if (buffer) {
            reply.PushMappedBuffer(*buffer);
        }
    }

    void GetRegion(Request& request) {
        // 3dbrew Config Services and SecureInfo_A.
        request.MakeReply(2, 0).Push(Result{0}, state_->save->Region());
    }

    void GetTransferableId(Request& request) {
        // Golden seq=228 and 3dbrew Cfg:GenHashConsoleUnique.
        const u32 salt = request.Pop<u32>();
        const auto id = state_->save->ReadBlock(wp11::kConsoleIdBlock, wp11::kSystemRead);
        auto& reply = request.MakeReply(3, 0);
        if (id.size() != 8) {
            reply.Push(kNotFound, u64{0});
            return;
        }
        reply.Push(Result{0}, wp11::TransferableId(id.first<8>(), salt));
    }

    void IsCoppacsSupported(Request& request) {
        // 3dbrew Cfg:GetRegionCanadaUSA uses the last byte of CountryInfo.
        const auto country = state_->save->ReadBlock(0x000B0000, wp11::kSystemRead);
        if (country.size() != 4) {
            request.MakeReply(2, 0).Push(kNotFound, false);
            return;
        }
        request.MakeReply(2, 0).Push(Result{0}, country.back() == 18 || country.back() == 49);
    }

    void GetSystemModel(Request& request) {
        // 3dbrew Cfg:GetSystemModel.
        const auto block = state_->save->ReadBlock(wp11::kModelBlock, wp11::kSystemRead);
        auto& reply = request.MakeReply(2, 0);
        if (block.size() != 4) {
            reply.Push(kNotFound, u8{0});
            return;
        }
        reply.Push(Result{0}, block.front());
    }

    std::shared_ptr<CfgState> state_;
};

MH4N_REGISTER(ServiceFactory, "new/cfg:u", [](ServiceContext& context) {
    return std::make_unique<CfgUser>(context);
});

}  // namespace
}  // namespace mh4n
