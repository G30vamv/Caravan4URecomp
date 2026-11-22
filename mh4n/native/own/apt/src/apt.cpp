// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew NS and APT Services, libctru APT IPC definitions, and boot-village.ipc.trace.

#include "apt_state.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#include "mh4n/loader.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"
#include "mh4n/threads.h"

namespace mh4n {
namespace {

constexpr Result kAppletNotFound = 0xC880CFFA;  // 3dbrew APT:GetAppletInfo.
constexpr Result kInvalidState = MakeResult(1000, 51, 5, 25);
constexpr Result kInvalidArgument = MakeResult(1006, 51, 7, 27);
constexpr Result kNoParameter = MakeResult(1007, 51, 5, 25);
constexpr Result kNoTitleMetadata = MakeResult(1007, 51, 4, 25);
constexpr u32 kGameCardMedia = 2;

struct AptState {
    apt::Application application;
    std::shared_ptr<Mutex> lock;
    std::shared_ptr<Event> notification;
    std::shared_ptr<Event> resume;
    u32 capture_permission = 0;
};

void ReplyResult(Request& request, Result result = kResultSuccess) {
    request.MakeReply(1, 0).Push(result);
}

class AptService final : public Service {
public:
    explicit AptService(ServiceContext& context)
        : Service(context, "APT:A"), state_(context.State<AptState>("new/APT:A")) {
        Register(0x0001, "GetLockHandle", &AptService::GetLockHandle);
        Register(0x0002, "Initialize", &AptService::Initialize);
        Register(0x0003, "Enable", &AptService::Enable);
        Register(0x0004, "Finalize", &AptService::Finalize);
        Register(0x0005, "GetAppletManInfo", &AptService::GetAppletManInfo);
        Register(0x0006, "GetAppletInfo", &AptService::GetAppletInfo);
        Register(0x0009, "IsRegistered", &AptService::IsRegistered);
        Register(0x000B, "InquireNotification", &AptService::InquireNotification);
        Register(0x000C, "SendParameter", &AptService::SendParameter);
        Register(0x000D, "ReceiveParameter", &AptService::ReceiveParameter);
        Register(0x000E, "GlanceParameter", &AptService::GlanceParameter);
        Register(0x000F, "CancelParameter", &AptService::CancelParameter);
        Register(0x001C, "WakeupApplication", &AptService::WakeupApplication);
        Register(0x0022, "PrepareToCloseApplication", &AptService::PrepareToCloseApplication);
        Register(0x0027, "CloseApplication", &AptService::CloseApplication);
        Register(0x0043, "NotifyToWait", &AptService::NotifyToWait);
        Register(0x004B, "AppletUtility", &AptService::AppletUtility);
        Register(0x004F, "SetApplicationCpuTimeLimit", &AptService::SetApplicationCpuTimeLimit);
        Register(0x0050, "GetApplicationCpuTimeLimit", &AptService::GetApplicationCpuTimeLimit);
        Register(0x0055, "SetScreenCapturePostPermission", &AptService::SetScreenCapturePostPermission);
        Register(0x0056, "GetScreenCapturePostPermission", &AptService::GetScreenCapturePostPermission);
        Register(0x0101, "GetTargetPlatform", &AptService::GetTargetPlatform);

        // The named commands with no handler are from the 3dbrew NS and APT Services table.
        Register(0x0007, "GetLastSignaledAppletId");
        Register(0x0008, "CountRegisteredApplet");
        Register(0x000A, "GetAttribute");
        Register(0x0010, "DebugFunc");
        Register(0x0011, "MapProgramIdForDebug");
        Register(0x0012, "SetHomeMenuAppletIdForDebug");
        Register(0x0013, "GetPreparationState");
        Register(0x0014, "SetPreparationState");
        Register(0x0015, "PrepareToStartApplication");
        Register(0x0016, "PreloadLibraryApplet");
        Register(0x0017, "FinishPreloadingLibraryApplet");
        Register(0x0018, "PrepareToStartLibraryApplet");
        Register(0x0019, "PrepareToStartSystemApplet");
        Register(0x001A, "PrepareToStartNewestHomeMenu");
        Register(0x001B, "StartApplication");
        Register(0x001D, "CancelApplication");
        Register(0x001E, "StartLibraryApplet");
        Register(0x001F, "StartSystemApplet");
        Register(0x0020, "StartNewestHomeMenu");
        Register(0x0021, "OrderToCloseApplication");
        Register(0x0023, "PrepareToJumpToApplication");
        Register(0x0024, "JumpToApplication");
        Register(0x0025, "PrepareToCloseLibraryApplet");
        Register(0x0026, "PrepareToCloseSystemApplet");
        Register(0x0028, "CloseLibraryApplet");
        Register(0x0029, "CloseSystemApplet");
        Register(0x002A, "OrderToCloseSystemApplet");
        Register(0x002B, "PrepareToJumpToHomeMenu");
        Register(0x002C, "JumpToHomeMenu");
        Register(0x002D, "PrepareToLeaveHomeMenu");
        Register(0x002E, "LeaveHomeMenu");
        Register(0x002F, "PrepareToLeaveResidentApplet");
        Register(0x0030, "LeaveResidentApplet");
        Register(0x0031, "PrepareToDoApplicationJump");
        Register(0x0032, "DoApplicationJump");
        Register(0x0033, "GetProgramIdOnApplicationJump");
        Register(0x0034, "SendDeliverArg");
        Register(0x0035, "ReceiveDeliverArg");
        Register(0x0036, "LoadSysMenuArg");
        Register(0x0037, "StoreSysMenuArg");
        Register(0x0038, "PreloadResidentApplet");
        Register(0x0039, "PrepareToStartResidentApplet");
        Register(0x003A, "StartResidentApplet");
        Register(0x003B, "CancelLibraryApplet");
        Register(0x003C, "SendDspSleep");
        Register(0x003D, "SendDspWakeUp");
        Register(0x003E, "ReplySleepQuery");
        Register(0x003F, "ReplySleepNotificationComplete");
        Register(0x0040, "SendCaptureBufferInfo");
        Register(0x0041, "ReceiveCaptureBufferInfo");
        Register(0x0042, "SleepSystem");
        Register(0x0044, "GetSharedFont");
        Register(0x0045, "GetWirelessRebootInfo");
        Register(0x0046, "Wrap");
        Register(0x0047, "Unwrap");
        Register(0x0048, "GetProgramInfo");
        Register(0x0049, "Reboot");
        Register(0x004A, "GetCaptureInfo");
        Register(0x004C, "SetFatalErrDispMode");
        Register(0x004D, "GetAppletProgramInfo");
        Register(0x004E, "HardwareResetAsync");
        Register(0x0051, "GetStartupArgument");
        Register(0x0052, "Wrap1");
        Register(0x0053, "Unwrap1");
        Register(0x0057, "WakeupApplication2");
        Register(0x0058, "GetProgramID");
        Register(0x0102, "CheckNew3DS");
        Register(0x0103, "GetApplicationRunningMode");
        Register(0x0104, "IsStandardMemoryLayout");
        Register(0x0105, "IsTitleAllowed");
    }

private:
    // boot-village seq=8: an unlocked mutex shared by all sessions.
    void GetLockHandle(Request& request) {
        if (!state_->lock) {
            state_->lock = Context().GetKernel().NewMutex(false, "APT lock");
        }
        auto& reply = request.MakeReply(3, 2);
        reply.Push(kResultSuccess, u32{0}, u32{0});
        reply.PushCopyObjects(state_->lock);
    }

    // 3dbrew APT:Initialize; boot-village seq=10 and the first wait on event #1.
    void Initialize(Request& request) {
        const u32 id = request.Pop<u32>();
        const u32 attributes = request.Pop<u32>();
        if (!state_->application.Initialize(id, attributes)) {
            ReplyResult(request, kInvalidState);
            return;
        }
        auto& kernel = Context().GetKernel();
        state_->notification = kernel.NewEvent(ResetType::OneShot, "APT notification");
        state_->resume = kernel.NewEvent(ResetType::OneShot, "APT resume");
        state_->resume->Signal();
        auto& reply = request.MakeReply(1, 3);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(state_->notification, state_->resume);
    }

    // 3dbrew APT:Enable and boot-village seq=67.
    void Enable(Request& request) {
        ReplyResult(request, state_->application.Enable(request.Pop<u32>()) ? kResultSuccess : kInvalidState);
    }

    // 3dbrew APT:Finalize and the public APT client IPC definition.
    void Finalize(Request& request) {
        if (!state_->application.Finalize(request.Pop<u32>())) {
            ReplyResult(request, kAppletNotFound);
            return;
        }
        ClearEvents();
        ReplyResult(request);
    }

    // 3dbrew APT:GetAppletManInfo and the application-slot position table.
    void GetAppletManInfo(Request& request) {
        const u32 position = request.Pop<u32>();
        const bool registered = state_->application.Registered(apt::kApplication);
        const bool active = state_->application.Active();
        request.MakeReply(5, 0).Push(kResultSuccess,
                                     active ? apt::kApplicationPosition : apt::kNoPosition,
                                     registered && position == apt::kApplicationPosition ? apt::kApplication : 0,
                                     u32{0}, active ? apt::kApplication : 0);
    }

    // 3dbrew APT:GetAppletInfo; the game folder's exheader supplies the title ID.
    void GetAppletInfo(Request& request) {
        const u32 id = request.Pop<u32>();
        if (!state_->application.Loaded(id)) {
            ReplyResult(request, kAppletNotFound);
            return;
        }

        const auto path = std::filesystem::path(Context().GameFolder()) / "exheader.bin";
        std::ifstream file(path, std::ios::binary);
        std::array<u8, kExHeaderMinSize> bytes{};
        ExHeader header;
        std::string error;
        if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
            !ParseExHeader(bytes.data(), bytes.size(), &header, &error)) {
            ReplyResult(request, kNoTitleMetadata);
            return;
        }

        request.MakeReply(7, 0).Push(kResultSuccess, header.program_id, kGameCardMedia,
                                    u32{state_->application.Registered(id)}, u32{1},
                                    state_->application.Attributes());
    }

    // 3dbrew NS and APT Services: IsRegistered.
    void IsRegistered(Request& request) {
        const u32 id = request.Pop<u32>();
        request.MakeReply(2, 0).Push(kResultSuccess, u32{state_->application.Registered(id)});
    }

    // 3dbrew APT:Initialize and the Notification enum. With no host event source, report NONE.
    void InquireNotification(Request& request) {
        if (!state_->application.Loaded(request.Pop<u32>())) {
            ReplyResult(request, kAppletNotFound);
            return;
        }
        request.MakeReply(2, 0).Push(kResultSuccess, u32{0});
    }

    // 3dbrew APT:SendParameter, including its 0x1000 byte limit.
    void SendParameter(Request& request) {
        apt::Parameter parameter{request.Pop<u32>(), request.Pop<u32>(), request.Pop<u32>(), {}, {}};
        const u32 size = request.Pop<u32>();
        parameter.object = request.PopGenericObject();
        const auto* buffer = request.PopStaticBuffer();
        if (size > apt::kMaxParameterSize || !buffer || buffer->data.size() < size) {
            ReplyResult(request, kInvalidArgument);
            return;
        }
        if (!state_->application.Loaded(parameter.receiver)) {
            ReplyResult(request, kAppletNotFound);
            return;
        }
        parameter.bytes.assign(buffer->data.begin(), buffer->data.begin() + size);
        if (!state_->application.Send(std::move(parameter))) {
            ReplyResult(request, kInvalidState);
            return;
        }
        state_->resume->Signal();
        ReplyResult(request);
    }

    // 3dbrew APT:ReceiveParameter and boot-village seq=71.
    void ReceiveParameter(Request& request) { ReadParameter(request, true); }

    // 3dbrew APT:GlanceParameter and boot-village seq=12 (moves a null handle).
    void GlanceParameter(Request& request) { ReadParameter(request, false); }

    void ReadParameter(Request& request, bool receive) {
        const u32 receiver = request.Pop<u32>();
        const u32 capacity = request.Pop<u32>();
        const auto* parameter = state_->application.Pending(receiver);
        if (capacity > apt::kMaxParameterSize || !parameter || parameter->bytes.size() > capacity) {
            ReplyResult(request, !parameter ? kNoParameter : kInvalidArgument);
            return;
        }

        const u32 command = parameter->command;
        const bool consume = receive || command == 8 || command == 9;
        std::vector<u8> output(capacity);
        std::copy(parameter->bytes.begin(), parameter->bytes.end(), output.begin());
        auto& reply = request.MakeReply(4, 4);
        reply.Push(kResultSuccess, parameter->sender, command, static_cast<u32>(parameter->bytes.size()));
        if (consume || !parameter->object) {
            reply.PushMoveObjects(parameter->object);
        } else {
            reply.PushCopyObjects(parameter->object);
        }
        reply.PushStaticBuffer(std::move(output), 0);
        if (consume) {
            state_->application.Consume();
            state_->resume->Clear();
        }
    }

    // 3dbrew APT:CancelParameter: each enabled filter must match.
    void CancelParameter(Request& request) {
        const auto check_sender = request.Pop<u8>();
        const auto sender = request.Pop<u32>();
        const auto check_receiver = request.Pop<u8>();
        const auto receiver = request.Pop<u32>();
        const bool canceled = state_->application.Cancel(check_sender ? std::optional{sender} : std::nullopt,
                                                          check_receiver ? std::optional{receiver} : std::nullopt);
        if (canceled) {
            state_->resume->Clear();
        }
        request.MakeReply(2, 0).Push(kResultSuccess, static_cast<u8>(canceled));
    }

    // 3dbrew NS and APT Services: WakeupApplication and COMMAND_WAKEUP.
    void WakeupApplication(Request& request) {
        if (!state_->application.Wakeup()) {
            ReplyResult(request, kInvalidState);
            return;
        }
        state_->resume->Signal();
        ReplyResult(request);
    }

    // 3dbrew APT:PrepareToCloseApplication.
    void PrepareToCloseApplication(Request& request) {
        ReplyResult(request, state_->application.PrepareToClose() ? kResultSuccess : kInvalidState);
    }

    // 3dbrew NS and APT Services: CloseApplication; the runtime owns process exit.
    void CloseApplication(Request& request) {
        const u32 size = request.Pop<u32>();
        request.PopGenericObject();
        const auto* buffer = request.PopStaticBuffer();
        if (size > apt::kMaxParameterSize || (size != 0 && (!buffer || buffer->data.size() < size))) {
            ReplyResult(request, kInvalidArgument);
            return;
        }
        if (!state_->application.Close()) {
            ReplyResult(request, kInvalidState);
            return;
        }
        ClearEvents();
        ReplyResult(request);
        Context().RequestExit(0, "APT: CloseApplication");
    }

    // boot-village seq=69 and the libctru NotifyToWait protocol before receiving a wakeup.
    void NotifyToWait(Request& request) {
        ReplyResult(request, state_->application.NotifyToWait(request.Pop<u32>()) ? kResultSuccess : kInvalidState);
    }

    // boot-village seq=73,75,77,875,35535,36194,38666; libctru SleepIfShellClosed and UnlockTransition.
    void AppletUtility(Request& request) {
        const u32 utility = request.Pop<u32>();
        const u32 input_size = request.Pop<u32>();
        const u32 output_size = request.Pop<u32>();
        const auto* input = request.PopStaticBuffer();
        const u32 expected_size = utility == 4 ? 1 : 4;
        if ((utility != 4 && utility != 7) || input_size != expected_size || output_size != 1 ||
            !input || input->data.size() != input_size) {
            ReplyResult(request, kInvalidArgument);
            return;
        }

        // The host exposes no lid; the shell is open. No transition locks are held.
        auto& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess, kResultSuccess);
        reply.PushStaticBuffer(std::vector<u8>(output_size), 0);
    }

    // 3dbrew APT:SetApplicationCpuTimeLimit and boot-village seq=56.
    void SetApplicationCpuTimeLimit(Request& request) {
        const u32 selector = request.Pop<u32>();
        const u32 percent = request.Pop<u32>();
        ReplyResult(request, selector == 1 ? Context().Threads().SetAppCpuTimeLimit(percent) : kInvalidArgument);
    }

    // 3dbrew APT:GetApplicationCpuTimeLimit.
    void GetApplicationCpuTimeLimit(Request& request) {
        if (request.Pop<u32>() != 1) {
            ReplyResult(request, kInvalidArgument);
            return;
        }
        request.MakeReply(2, 0).Push(kResultSuccess, Context().Threads().AppCpuTimeLimit());
    }

    // 3dbrew NS and APT Services: SetScreenCapturePostPermission; boot-village seq=14.
    void SetScreenCapturePostPermission(Request& request) {
        state_->capture_permission = request.Pop<u32>();
        ReplyResult(request);
    }

    // 3dbrew NS and APT Services: GetScreenCapturePostPermission.
    void GetScreenCapturePostPermission(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, state_->capture_permission);
    }

    // 3dbrew APT:GetTargetPlatform and boot-village seq=3 (default launch mode).
    void GetTargetPlatform(Request& request) { request.MakeReply(2, 0).Push(kResultSuccess, u32{0}); }

    void ClearEvents() {
        state_->notification->Clear();
        state_->resume->Clear();
        state_->notification.reset();
        state_->resume.reset();
    }

    std::shared_ptr<AptState> state_;
};

MH4N_REGISTER(ServiceFactory, "new/APT:A",
              [](ServiceContext& context) { return std::make_unique<AptService>(context); });

}  // namespace
}  // namespace mh4n
