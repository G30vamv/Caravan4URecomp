// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: WP31-apt, applet_manager.h, shared_font.h, apt_test.cpp and boot-village.ipc.trace.

#include "applet_manager.h"
#include "shared_font.h"

#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/threads.h"

namespace mh4n::apt {
namespace {

constexpr Result kInvalidArgument = MakeResult(1006, 51, 7, 27);
constexpr u32 kMaxStaticSize = (1u << 18) - 1;

struct AptState {
    std::shared_ptr<AppletManager> manager;
    std::shared_ptr<SharedMemory> font;
    u32 capture_permission = 0;
};

void ReplyResult(Request& request, Result result = kResultSuccess) {
    request.MakeReply(1, 0).Push(result);
}

std::vector<u8> ReadBuffer(Request& request, u32 size) {
    const auto* buffer = request.PopStaticBuffer();
    if (!buffer) {
        return {};
    }
    return {buffer->data.begin(), buffer->data.begin() + std::min<std::size_t>(size, buffer->data.size())};
}

class AptService final : public Service {
public:
    AptService(ServiceContext& context, const char* name)
        : Service(context, name), state_(context.State<AptState>("APT")) {
        if (!state_->manager) {
            state_->manager = std::make_shared<AppletManager>(context);
        }

        Register(0x0001, "GetLockHandle", &AptService::GetLockHandle);
        Register(0x0002, "Initialize", &AptService::Initialize);
        Register(0x0003, "Enable", &AptService::Enable);
        Register(0x0004, "Finalize");
        Register(0x0005, "GetAppletManInfo", &AptService::GetAppletManInfo);
        Register(0x0006, "GetAppletInfo");
        Register(0x0007, "GetLastSignaledAppletId");
        Register(0x0008, "CountRegisteredApplet");
        Register(0x0009, "IsRegistered", &AptService::IsRegistered);
        Register(0x000A, "GetAttribute");
        Register(0x000B, "InquireNotification", &AptService::InquireNotification);
        Register(0x000C, "SendParameter", &AptService::SendParameter);
        Register(0x000D, "ReceiveParameter", &AptService::ReceiveParameter);
        Register(0x000E, "GlanceParameter", &AptService::GlanceParameter);
        Register(0x000F, "CancelParameter", &AptService::CancelParameter);
        Register(0x0010, "DebugFunc");
        Register(0x0011, "MapProgramIdForDebug");
        Register(0x0012, "SetHomeMenuAppletIdForDebug");
        Register(0x0013, "GetPreparationState");
        Register(0x0014, "SetPreparationState");
        Register(0x0015, "PrepareToStartApplication");
        Register(0x0016, "PreloadLibraryApplet", &AptService::PreloadLibraryApplet);
        Register(0x0017, "FinishPreloadingLibraryApplet", &AptService::FinishPreloadingLibraryApplet);
        Register(0x0018, "PrepareToStartLibraryApplet", &AptService::PrepareToStartLibraryApplet);
        Register(0x0019, "PrepareToStartSystemApplet", &AptService::PrepareToStartSystemApplet);
        Register(0x001A, "PrepareToStartNewestHomeMenu");
        Register(0x001B, "StartApplication");
        Register(0x001C, "WakeupApplication");
        Register(0x001D, "CancelApplication");
        Register(0x001E, "StartLibraryApplet", &AptService::StartLibraryApplet);
        Register(0x001F, "StartSystemApplet", &AptService::StartSystemApplet);
        Register(0x0020, "StartNewestHomeMenu");
        Register(0x0021, "OrderToCloseApplication");
        Register(0x0022, "PrepareToCloseApplication", &AptService::PrepareToCloseApplication);
        Register(0x0023, "PrepareToJumpToApplication");
        Register(0x0024, "JumpToApplication");
        Register(0x0025, "PrepareToCloseLibraryApplet", &AptService::PrepareToCloseLibraryApplet);
        Register(0x0026, "PrepareToCloseSystemApplet");
        Register(0x0027, "CloseApplication", &AptService::CloseApplication);
        Register(0x0028, "CloseLibraryApplet", &AptService::CloseLibraryApplet);
        Register(0x0029, "CloseSystemApplet");
        Register(0x002A, "OrderToCloseSystemApplet");
        Register(0x002B, "PrepareToJumpToHomeMenu", &AptService::PrepareToJumpToHomeMenu);
        Register(0x002C, "JumpToHomeMenu", &AptService::JumpToHomeMenu);
        Register(0x002D, "PrepareToLeaveHomeMenu");
        Register(0x002E, "LeaveHomeMenu");
        Register(0x002F, "PrepareToLeaveResidentApplet");
        Register(0x0030, "LeaveResidentApplet");
        Register(0x0031, "PrepareToDoApplicationJump", &AptService::PrepareToDoApplicationJump);
        Register(0x0032, "DoApplicationJump", &AptService::DoApplicationJump);
        Register(0x0033, "GetProgramIdOnApplicationJump");
        Register(0x0034, "SendDeliverArg");
        Register(0x0035, "ReceiveDeliverArg");
        Register(0x0036, "LoadSysMenuArg");
        Register(0x0037, "StoreSysMenuArg");
        Register(0x0038, "PreloadResidentApplet");
        Register(0x0039, "PrepareToStartResidentApplet");
        Register(0x003A, "StartResidentApplet");
        Register(0x003B, "CancelLibraryApplet", &AptService::CancelLibraryApplet);
        Register(0x003C, "SendDspSleep");
        Register(0x003D, "SendDspWakeUp");
        Register(0x003E, "ReplySleepQuery", &AptService::Succeed);
        Register(0x003F, "ReplySleepNotificationComplete", &AptService::Succeed);
        Register(0x0040, "SendCaptureBufferInfo", &AptService::SendCaptureBufferInfo);
        Register(0x0041, "ReceiveCaptureBufferInfo");
        Register(0x0042, "SleepSystem");
        Register(0x0043, "NotifyToWait", &AptService::Succeed);
        Register(0x0044, "GetSharedFont");
        Register(0x0045, "GetWirelessRebootInfo");
        Register(0x0046, "Wrap");
        Register(0x0047, "Unwrap");
        Register(0x0048, "GetProgramInfo");
        Register(0x0049, "Reboot");
        Register(0x004A, "GetCaptureInfo");
        Register(0x004B, "AppletUtility", &AptService::AppletUtility);
        Register(0x004C, "SetFatalErrDispMode");
        Register(0x004D, "GetAppletProgramInfo");
        Register(0x004E, "HardwareResetAsync");
        Register(0x004F, "SetApplicationCpuTimeLimit", &AptService::SetApplicationCpuTimeLimit);
        Register(0x0050, "GetApplicationCpuTimeLimit", &AptService::GetApplicationCpuTimeLimit);
        Register(0x0051, "GetStartupArgument");
        Register(0x0052, "Wrap1");
        Register(0x0053, "Unwrap1");
        Register(0x0055, "SetScreenCapturePostPermission", &AptService::SetScreenCapturePostPermission);
        Register(0x0056, "GetScreenCapturePostPermission", &AptService::GetScreenCapturePostPermission);
        Register(0x0057, "WakeupApplication2");
        Register(0x0058, "GetProgramID");
        Register(0x0101, "GetTargetPlatform", &AptService::GetTargetPlatform);
        Register(0x0102, "CheckNew3DS");
        Register(0x0103, "GetApplicationRunningMode", &AptService::GetApplicationRunningMode);
        Register(0x0104, "IsStandardMemoryLayout");
        Register(0x0105, "IsTitleAllowed");
    }

private:
    AppletManager& Manager() { return *state_->manager; }

    void GetLockHandle(Request& request) {
        const auto info = Manager().GetLockHandle(AppletAttributes{request.Pop<u32>()});
        auto& reply = request.MakeReply(3, 2);
        reply.Push(info.code, info.value.corrected_attributes.raw, info.value.state);
        reply.PushCopyObjects(info.value.lock);
    }

    void Initialize(Request& request) {
        const auto id = request.Pop<AppletId>();
        const auto result = Manager().Initialize(id, AppletAttributes{request.Pop<u32>()});
        if (result.Failed()) {
            ReplyResult(request, result.code);
            return;
        }
        auto& reply = request.MakeReply(1, 3);
        reply.Push(result.code);
        reply.PushCopyObjects(result.value.notification_event, result.value.parameter_event);
    }

    void Enable(Request& request) {
        ReplyResult(request, Manager().Enable(AppletAttributes{request.Pop<u32>()}));
    }

    void GetAppletManInfo(Request& request) {
        const auto info = Manager().GetAppletManInfo(request.Pop<AppletPos>());
        request.MakeReply(5, 0).Push(info.code, info.value.active_applet_pos,
                                    info.value.requested_applet_id, info.value.home_menu_applet_id,
                                    info.value.active_applet_id);
    }

    void IsRegistered(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, Manager().IsRegistered(request.Pop<AppletId>()));
    }

    void InquireNotification(Request& request) {
        const auto result = Manager().InquireNotification(request.Pop<AppletId>());
        if (result.Failed()) {
            ReplyResult(request, result.code);
            return;
        }
        request.MakeReply(2, 0).Push(result.code, result.value);
    }

    void SendParameter(Request& request) {
        MessageParameter parameter;
        parameter.sender_id = request.Pop<AppletId>();
        parameter.destination_id = request.Pop<AppletId>();
        parameter.signal = request.Pop<SignalType>();
        const u32 size = request.Pop<u32>();
        parameter.object = request.PopGenericObject();
        parameter.buffer = ReadBuffer(request, size);
        ReplyResult(request, Manager().SendParameter(parameter));
    }

    void ReceiveParameter(Request& request) { ReadParameter(request, true); }
    void GlanceParameter(Request& request) { ReadParameter(request, false); }

    void ReadParameter(Request& request, bool receive) {
        const auto id = request.Pop<AppletId>();
        const u32 capacity = request.Pop<u32>();
        if (capacity > kMaxStaticSize) {
            ReplyResult(request, kInvalidArgument);
            return;
        }

        const auto result = receive ? Manager().ReceiveParameter(id) : Manager().GlanceParameter(id);
        if (result.Failed()) {
            ReplyResult(request, result.code);
            return;
        }
        const auto& parameter = result.value;
        const auto size = static_cast<u32>(std::min<std::size_t>(parameter.buffer.size(), capacity));
        std::vector<u8> output(capacity);
        std::copy_n(parameter.buffer.begin(), size, output.begin());
        auto& reply = request.MakeReply(4, 4);
        reply.Push(result.code, parameter.sender_id, parameter.signal, size);
        reply.PushMoveObjects(parameter.object);
        reply.PushStaticBuffer(std::move(output), 0);
    }

    void CancelParameter(Request& request) {
        const bool check_sender = request.Pop<bool>();
        const auto sender = request.Pop<AppletId>();
        const bool check_receiver = request.Pop<bool>();
        const auto receiver = request.Pop<AppletId>();
        request.MakeReply(2, 0).Push(kResultSuccess,
            Manager().CancelParameter(check_sender, sender, check_receiver, receiver));
    }

    void PreloadLibraryApplet(Request& request) {
        ReplyResult(request, Manager().PreloadLibraryApplet(request.Pop<AppletId>()));
    }

    void FinishPreloadingLibraryApplet(Request& request) {
        ReplyResult(request, Manager().FinishPreloadingLibraryApplet(request.Pop<AppletId>()));
    }

    void PrepareToStartLibraryApplet(Request& request) {
        ReplyResult(request, Manager().PrepareToStartLibraryApplet(request.Pop<AppletId>()));
    }

    void PrepareToStartSystemApplet(Request& request) {
        ReplyResult(request, Manager().PrepareToStartSystemApplet(request.Pop<AppletId>()));
    }

    void StartLibraryApplet(Request& request) {
        const auto id = request.Pop<AppletId>();
        const u32 size = request.Pop<u32>();
        auto object = request.PopGenericObject();
        ReplyResult(request, Manager().StartLibraryApplet(id, std::move(object), ReadBuffer(request, size)));
    }

    void StartSystemApplet(Request& request) {
        const auto id = request.Pop<AppletId>();
        const u32 size = request.Pop<u32>();
        auto object = request.PopGenericObject();
        ReplyResult(request, Manager().StartSystemApplet(id, std::move(object), ReadBuffer(request, size)));
    }

    void PrepareToCloseApplication(Request& request) {
        ReplyResult(request, Manager().PrepareToCloseApplication(request.Pop<bool>()));
    }

    void PrepareToCloseLibraryApplet(Request& request) {
        const bool not_pause = request.Pop<bool>();
        const bool exiting = request.Pop<bool>();
        const bool jump_home = request.Pop<bool>();
        ReplyResult(request, Manager().PrepareToCloseLibraryApplet(not_pause, exiting, jump_home));
    }

    void CloseApplication(Request& request) {
        const u32 size = request.Pop<u32>();
        auto object = request.PopGenericObject();
        ReplyResult(request, Manager().CloseApplication(std::move(object), ReadBuffer(request, size)));
    }

    void CloseLibraryApplet(Request& request) {
        const u32 size = request.Pop<u32>();
        auto object = request.PopGenericObject();
        ReplyResult(request, Manager().CloseLibraryApplet(std::move(object), ReadBuffer(request, size)));
    }

    void PrepareToJumpToHomeMenu(Request& request) {
        ReplyResult(request, Manager().PrepareToJumpToHomeMenu());
    }

    void JumpToHomeMenu(Request& request) {
        const u32 size = request.Pop<u32>();
        auto object = request.PopGenericObject();
        ReplyResult(request, Manager().JumpToHomeMenu(std::move(object), ReadBuffer(request, size)));
    }

    void PrepareToDoApplicationJump(Request& request) {
        const auto flags = request.Pop<ApplicationJumpFlags>();
        const auto title_id = request.Pop<u64>();
        ReplyResult(request, Manager().PrepareToDoApplicationJump(title_id, flags));
    }

    void DoApplicationJump(Request& request) { ReplyResult(request, Manager().DoApplicationJump()); }

    void CancelLibraryApplet(Request& request) {
        ReplyResult(request, Manager().CancelLibraryApplet(request.Pop<bool>()));
    }

    void Succeed(Request& request) { ReplyResult(request); }

    void SendCaptureBufferInfo(Request& request) {
        const u32 size = request.Pop<u32>();
        Manager().SendCaptureBufferInfo(ReadBuffer(request, size));
        ReplyResult(request);
    }

    void AppletUtility(Request& request) {
        const u32 utility = request.Pop<u32>();
        request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        if (size > kMaxStaticSize) {
            ReplyResult(request, kInvalidArgument);
            return;
        }
        std::vector<u8> output(size);
        if (utility == 6 && !output.empty()) {
            output.front() = 1;
        }
        auto& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess, kResultSuccess);
        reply.PushStaticBuffer(std::move(output), 0);
    }

    void SetApplicationCpuTimeLimit(Request& request) {
        const u32 selector = request.Pop<u32>();
        const u32 percent = request.Pop<u32>();
        ReplyResult(request, selector == 1 ? Context().Threads().SetAppCpuTimeLimit(percent)
                                           : kInvalidArgument);
    }

    void GetApplicationCpuTimeLimit(Request& request) {
        if (request.Pop<u32>() != 1) {
            ReplyResult(request, kInvalidArgument);
            return;
        }
        request.MakeReply(2, 0).Push(kResultSuccess, Context().Threads().AppCpuTimeLimit());
    }

    void SetScreenCapturePostPermission(Request& request) {
        state_->capture_permission = request.Pop<u32>();
        ReplyResult(request);
    }

    void GetScreenCapturePostPermission(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, state_->capture_permission);
    }

    void GetTargetPlatform(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, Manager().GetTargetPlatform());
    }

    void GetApplicationRunningMode(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, Manager().GetApplicationRunningMode());
    }

    std::shared_ptr<AptState> state_;
};

void StartApt(Runtime& runtime) {
    auto* services = FindServiceManager(runtime);
    if (!services) {
        return;
    }
    auto& context = services->Context();
    const auto state = context.State<AptState>("APT");
    state->manager = std::make_shared<AppletManager>(context);
    if (const auto* image = runtime.Image()) {
        state->manager->SetProgramId(image->program_id);
    }
    AllocateSharedFontBlock(context.GetKernel(), &state->font);
}

MH4N_REGISTER(BootStep, "services.ipc.apt", BootStep{Phase::Services, StartApt});
MH4N_REGISTER(ServiceFactory, "APT:A", ServiceFactory{
    [](ServiceContext& context) { return std::make_unique<AptService>(context, "APT:A"); }, 2});
MH4N_REGISTER(ServiceFactory, "APT:S", ServiceFactory{
    [](ServiceContext& context) { return std::make_unique<AptService>(context, "APT:S"); }, 2});
MH4N_REGISTER(ServiceFactory, "APT:U", ServiceFactory{
    [](ServiceContext& context) { return std::make_unique<AptService>(context, "APT:U"); }, 2});

}  // namespace

Result AllocateSharedFontBlock(Kernel& kernel, std::shared_ptr<SharedMemory>* out) {
    out->reset();
    return kernel.NewSharedMemory(out, nullptr, kSharedFontSize, MemoryPermission::ReadWrite,
                                  MemoryPermission::Read, 0, FcramRegion::System, "APT:SharedFont");
}

std::shared_ptr<SharedMemory> SharedFontBlock(ServiceContext& context) {
    return context.State<AptState>("APT")->font;
}

}  // namespace mh4n::apt
