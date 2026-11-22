// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: applet_manager.h, WP31-apt, boot-village.ipc.trace and apt_test.cpp black-box cases.

#include "applet_manager.h"

#include <cstring>

#include "applets/erreula.h"
#include "applets/extrapad.h"
#include "applets/mii_selector.h"
#include "applets/mint.h"
#include "applets/swkbd.h"
#include "mh4n/log.h"

namespace mh4n::apt {
namespace {

constexpr const char* kLog = "Service.APT";

// The periodic "button update" of the console (the HOME and power buttons are polled once per frame of the applet clock). The port
// has no such buttons to report, but the event is part of the deterministic timeline (determinism_test), so it keeps running.
void ScheduleButtonUpdate(Kernel& kernel, u64 due_tick) {
    kernel.ScheduleAt(due_tick, [&kernel, due_tick] { ScheduleButtonUpdate(kernel, due_tick + kAppletUpdateIntervalTicks); });
}

}  // namespace

AppletManager::AppletManager(ServiceContext& context)
    : context_(context), lock_(context.GetKernel().NewMutex(false, "APT lock")) {
    for (std::size_t i = 0; i < applet_slots_.size(); ++i) {
        applet_slots_[i].slot = static_cast<AppletSlot>(i);
    }
    ScheduleButtonUpdate(context.GetKernel(), context.GetKernel().NowTicks() + kAppletUpdateIntervalTicks);
}

AppletManager::~AppletManager() = default;

AppletManager::AppletSlotData* AppletManager::GetAppletSlot(AppletSlot slot) {
    const auto index = static_cast<std::size_t>(slot);
    return index < applet_slots_.size() ? &applet_slots_[index] : nullptr;
}

AppletId AppletManager::GetAppletSlotId(AppletSlot slot) {
    const auto* data = GetAppletSlot(slot);
    return data ? data->applet_id : AppletId::None;
}

AppletManager::AppletSlot AppletManager::GetAppletSlotFromId(AppletId id) {
    if (id == AppletId::None) {
        return AppletSlot::Error;
    }
    if (id == AppletId::AnyLibraryApplet || id == AppletId::AnySysLibraryApplet) {
        const AppletId library = GetAppletSlotId(AppletSlot::LibraryApplet);
        const auto type = static_cast<u32>(library) & static_cast<u32>(AppletId::TypeMask);
        return type == static_cast<u32>(id) ? AppletSlot::LibraryApplet : AppletSlot::Error;
    }
    if (id == AppletId::AnySystemApplet) {
        return GetAppletSlotId(AppletSlot::SystemApplet) != AppletId::None
                   ? AppletSlot::SystemApplet : AppletSlot::Error;
    }
    for (const auto& slot : applet_slots_) {
        if (slot.applet_id == id) {
            return slot.slot;
        }
    }
    return AppletSlot::Error;
}

AppletManager::AppletSlot AppletManager::GetAppletSlotFromPos(AppletPos pos) {
    switch (pos) {
    case AppletPos::Application:
        return AppletSlot::Application;
    case AppletPos::System:
        return AppletSlot::SystemApplet;
    case AppletPos::Library:
    case AppletPos::SysLibrary:
    case AppletPos::AutoLibrary:
        return AppletSlot::LibraryApplet;
    default:
        return AppletSlot::Error;
    }
}

AppletManager::AppletSlot AppletManager::GetAppletSlotFromAttributes(AppletAttributes attributes) {
    return attributes.IsHomeMenu() ? AppletSlot::HomeMenu : GetAppletSlotFromPos(attributes.Pos());
}

ResultVal<GetLockHandleResult> AppletManager::GetLockHandle(AppletAttributes attributes) {
    if (attributes.Pos() == AppletPos::AutoLibrary) {
        attributes.SetPos(last_library_launcher_slot_ == AppletSlot::Application
                              ? AppletPos::Library : AppletPos::SysLibrary);
    }
    return {kResultSuccess, {attributes, 0, lock_}};
}

ResultVal<InitializeResult> AppletManager::Initialize(AppletId app_id, AppletAttributes attributes) {
    auto* slot = GetAppletSlot(GetAppletSlotFromAttributes(attributes));
    if (!slot) {
        return {kResultInvalidAppletSlot};
    }
    if (slot->applet_id != AppletId::None) {
        return {kResultAlreadyExists};
    }
    auto& kernel = context_.GetKernel();
    slot->applet_id = app_id;
    slot->attributes = attributes;
    slot->notification_event = kernel.NewEvent(ResetType::OneShot, "APT notification");
    slot->parameter_event = kernel.NewEvent(ResetType::OneShot, "APT parameter");
    if (slot->slot == AppletSlot::Application) {
        slot->title_id = program_id_;
    }
    if (active_slot_ == AppletSlot::Error) {
        slot->registered = true;
        active_slot_ = slot->slot;
        CancelAndSendParameter({AppletId::None, app_id, SignalType::Wakeup, {}, {}});
    }
    return {kResultSuccess, {slot->notification_event, slot->parameter_event}};
}

Result AppletManager::Enable(AppletAttributes attributes) {
    auto* slot = GetAppletSlot(GetAppletSlotFromAttributes(attributes));
    if (!slot || slot->applet_id == AppletId::None) {
        return kResultInvalidAppletSlot;
    }
    slot->attributes = attributes;
    slot->registered = true;
    if (delayed_parameter_ && delayed_parameter_->destination_id == slot->applet_id) {
        MessageParameter parameter = std::move(*delayed_parameter_);
        delayed_parameter_.reset();
        CancelAndSendParameter(parameter);
    }
    return kResultSuccess;
}

bool AppletManager::IsRegistered(AppletId app_id) {
    const auto* slot = GetAppletSlot(GetAppletSlotFromId(app_id));
    return slot && (slot->registered || slot->loaded);
}

ResultVal<Notification> AppletManager::InquireNotification(AppletId app_id) {
    auto* slot = GetAppletSlot(GetAppletSlotFromId(app_id));
    if (!slot) {
        return {kResultAppletNotFound};
    }
    return {kResultSuccess, std::exchange(slot->notification, Notification::None)};
}

ResultVal<AppletManInfo> AppletManager::GetAppletManInfo(AppletPos requested_applet_pos) {
    const auto* active = GetAppletSlot(active_slot_);
    const auto home = GetAppletSlotId(AppletSlot::HomeMenu);
    return {kResultSuccess, {active ? active->attributes.Pos() : AppletPos::Invalid,
                            GetAppletSlotId(GetAppletSlotFromPos(requested_applet_pos)),
                            home == AppletId::None ? AppletId::HomeMenu : home,
                            active ? active->applet_id : AppletId::None}};
}

Result AppletManager::SendParameter(const MessageParameter& parameter) {
    if (next_parameter_) {
        return kResultParameterPresent;
    }
    CancelAndSendParameter(parameter);
    return kResultSuccess;
}

void AppletManager::CancelAndSendParameter(const MessageParameter& parameter) {
    CancelParameter(false, AppletId::None, false, AppletId::None);
    const auto found = hle_applets_.find(parameter.destination_id);
    if (found != hle_applets_.end()) {
        // Start/Finalize can synchronously close the applet and remove it from the map.
        const auto applet = found->second;
        applet->ReceiveParameter(parameter);
        return;
    }
    next_parameter_ = parameter;
    auto* slot = GetAppletSlot(GetAppletSlotFromId(parameter.destination_id));
    if (slot && slot->parameter_event) {
        slot->parameter_event->Signal();
    }
}

ResultVal<MessageParameter> AppletManager::GlanceParameter(AppletId app_id) {
    if (!next_parameter_) {
        return {kResultNoData};
    }
    const auto destination = next_parameter_->destination_id;
    const auto slot = GetAppletSlotFromId(app_id);
    if (destination != app_id && (slot == AppletSlot::Error || slot != GetAppletSlotFromId(destination))) {
        return {kResultAppletNotFound};
    }
    return {kResultSuccess, *next_parameter_};
}

ResultVal<MessageParameter> AppletManager::ReceiveParameter(AppletId app_id) {
    auto parameter = GlanceParameter(app_id);
    if (parameter.Succeeded()) {
        CancelParameter(false, AppletId::None, false, AppletId::None);
    }
    return parameter;
}

bool AppletManager::CancelParameter(bool check_sender, AppletId sender_appid, bool check_receiver,
                                    AppletId receiver_appid) {
    if (!next_parameter_ || (check_sender && next_parameter_->sender_id != sender_appid) ||
        (check_receiver && next_parameter_->destination_id != receiver_appid)) {
        return false;
    }
    auto* slot = GetAppletSlot(GetAppletSlotFromId(next_parameter_->destination_id));
    if (slot && slot->parameter_event) {
        slot->parameter_event->Clear();
    }
    next_parameter_.reset();
    return true;
}

bool AppletManager::HasHleApplet(AppletId id) const {
    return hle_applets_.contains(id);
}

Result AppletManager::CreateHleApplet(AppletId id, AppletId parent, bool preload) {
    auto* slot = GetAppletSlot(AppletSlot::LibraryApplet);
    if (slot->applet_id != AppletId::None) {
        return kResultAlreadyExists;
    }
    std::shared_ptr<Applet> applet;
    const auto host = weak_from_this();
    switch (id) {
    case AppletId::SoftwareKeyboard1:
    case AppletId::SoftwareKeyboard2:
        applet = std::make_shared<SoftwareKeyboard>(id, parent, host, context_);
        break;
    case AppletId::Ed1:
    case AppletId::Ed2:
        applet = std::make_shared<MiiSelector>(id, parent, host, context_);
        break;
    case AppletId::Error:
    case AppletId::Error2:
        applet = std::make_shared<ErrEula>(id, parent, host, context_);
        break;
    case AppletId::Mint:
    case AppletId::Mint2:
        applet = std::make_shared<Mint>(id, parent, host, context_);
        break;
    case AppletId::Extrapad:
    case AppletId::Extrapad2:
        applet = std::make_shared<Extrapad>(id, parent, host, context_);
        break;
    default:
        MH4N_LOG(Warning, kLog, "Could not create applet {:03X}", static_cast<u32>(id));
        return kResultAppletNotSupported;
    }

    slot->applet_id = id;
    slot->loaded = preload;
    slot->registered = true;
    slot->attributes.SetPos(active_slot_ == AppletSlot::Application ? AppletPos::Library
                                                                   : AppletPos::SysLibrary);
    last_library_launcher_slot_ = active_slot_;
    last_prepared_library_applet_ = id;
    hle_applets_.emplace(id, std::move(applet));
    ScheduleAppletUpdate(id, context_.GetKernel().NowTicks() + kAppletUpdateIntervalTicks);
    return kResultSuccess;
}

void AppletManager::ScheduleAppletUpdate(AppletId id, u64 due_tick) {
    const auto owner = weak_from_this();
    const std::weak_ptr<Applet> scheduled = hle_applets_.at(id);
    context_.GetKernel().ScheduleAt(due_tick, [owner, scheduled, id, due_tick] {
        if (auto manager = owner.lock(); manager && !scheduled.expired()) {
            manager->HleAppletUpdateEvent(id, due_tick);
        }
    });
}

void AppletManager::HleAppletUpdateEvent(AppletId id, u64 due_tick) {
    const auto found = hle_applets_.find(id);
    if (found == hle_applets_.end()) {
        return;
    }
    const auto applet = found->second;
    if (applet->IsActive()) {
        applet->Update();
    }
    if (applet->IsRunning() && HasHleApplet(id)) {
        ScheduleAppletUpdate(id, due_tick + kAppletUpdateIntervalTicks);
    }
}

Result AppletManager::PrepareToStartLibraryApplet(AppletId applet_id) {
    if (next_parameter_) {
        return kResultParameterPresent;
    }
    return CreateHleApplet(applet_id, GetAppletSlotId(active_slot_), false);
}

Result AppletManager::PreloadLibraryApplet(AppletId applet_id) {
    return CreateHleApplet(applet_id, GetAppletSlotId(active_slot_), true);
}

Result AppletManager::FinishPreloadingLibraryApplet(AppletId applet_id) {
    auto* slot = GetAppletSlot(GetAppletSlotFromId(applet_id));
    if (!slot || slot->slot != AppletSlot::LibraryApplet) {
        return kResultInvalidAppletSlot;
    }
    slot->loaded = true;
    return kResultSuccess;
}

Result AppletManager::StartLibraryApplet(AppletId applet_id, std::shared_ptr<Object> object,
                                         const std::vector<u8>& buffer) {
    if (!HasHleApplet(applet_id)) {
        return kResultAppletNotSupported;
    }
    if (next_parameter_) {
        return kResultParameterPresent;
    }
    active_slot_ = AppletSlot::LibraryApplet;
    return SendParameter({GetAppletSlotId(last_library_launcher_slot_), applet_id, SignalType::Wakeup,
                          std::move(object), buffer});
}

Result AppletManager::PrepareToCloseLibraryApplet(bool not_pause, bool exiting, bool jump_home) {
    if (GetAppletSlotId(AppletSlot::LibraryApplet) == AppletId::None) {
        return kResultInvalidAppletSlot;
    }
    library_applet_closing_command_ = jump_home ? SignalType::WakeupToJumpHome
                                      : !not_pause ? SignalType::WakeupByPause
                                      : exiting ? SignalType::WakeupByExit
                                                : SignalType::WakeupByCancel;
    return kResultSuccess;
}

Result AppletManager::CloseLibraryApplet(std::shared_ptr<Object> object, const std::vector<u8>& buffer) {
    auto* slot = GetAppletSlot(AppletSlot::LibraryApplet);
    const auto id = slot->applet_id;
    if (id == AppletId::None) {
        return kResultInvalidAppletSlot;
    }
    slot->Reset();
    slot->loaded = false;
    slot->notification = Notification::None;
    slot->notification_event.reset();
    slot->parameter_event.reset();
    hle_applets_.erase(id);
    last_prepared_library_applet_ = AppletId::None;
    active_slot_ = last_library_launcher_slot_;
    const auto command = std::exchange(library_applet_closing_command_, SignalType::None);
    CancelAndSendParameter({id, GetAppletSlotId(active_slot_), command, std::move(object), buffer});
    return kResultSuccess;
}

Result AppletManager::CancelLibraryApplet(bool app_exiting) {
    if (next_parameter_) {
        return kResultParameterPresent;
    }
    const auto id = GetAppletSlotId(AppletSlot::LibraryApplet);
    if (id == AppletId::None) {
        return kResultInvalidAppletSlot;
    }
    return SendParameter({GetAppletSlotId(last_library_launcher_slot_), id,
                          app_exiting ? SignalType::WakeupByCancelAll : SignalType::WakeupByCancel,
                          {}, {}});
}

Result AppletManager::PrepareToStartSystemApplet(AppletId) {
    if (next_parameter_) {
        return kResultParameterPresent;
    }
    last_system_launcher_slot_ = active_slot_;
    return kResultSuccess;
}

Result AppletManager::StartSystemApplet(AppletId applet_id, std::shared_ptr<Object>,
                                        const std::vector<u8>&) {
    MH4N_LOG(Warning, kLog, "cannot start system applet {:03X}: no such title",
             static_cast<u32>(applet_id));
    return kResultAppletNotSupported;
}

void AppletManager::EnsureHomeMenuLoaded() {
    if (GetAppletSlotId(AppletSlot::HomeMenu) == AppletId::None) {
        MH4N_LOG(Warning, kLog, "The Home Menu failed to launch");
    }
}

Result AppletManager::PrepareToJumpToHomeMenu() {
    if (next_parameter_) {
        return kResultParameterPresent;
    }
    last_jump_to_home_slot_ = active_slot_;
    return kResultSuccess;
}

Result AppletManager::JumpToHomeMenu(std::shared_ptr<Object> object, const std::vector<u8>& buffer) {
    EnsureHomeMenuLoaded();
    return SendParameter({GetAppletSlotId(last_jump_to_home_slot_), AppletId::HomeMenu,
                          SignalType::WakeupByPause, std::move(object), buffer});
}

Result AppletManager::PrepareToCloseApplication(bool return_to_sys) {
    if (active_slot_ != AppletSlot::Application) {
        return kResultInvalidAppletSlot;
    }
    application_close_target_ = return_to_sys ? AppletSlot::HomeMenu : last_system_launcher_slot_;
    return kResultSuccess;
}

Result AppletManager::CloseApplication(std::shared_ptr<Object> object, const std::vector<u8>& buffer) {
    const auto target = GetAppletSlotId(application_close_target_);
    if (target == AppletId::None) {
        context_.RequestExit(0, "the application closed and there is no HOME menu");
    } else {
        CancelAndSendParameter({AppletId::Application, target, SignalType::WakeupByExit,
                                std::move(object), buffer});
    }
    GetAppletSlot(AppletSlot::Application)->Reset();
    active_slot_ = application_close_target_;
    return kResultSuccess;
}

Result AppletManager::PrepareToDoApplicationJump(u64 title_id, ApplicationJumpFlags flags) {
    if (flags == ApplicationJumpFlags::UseStoredParameters) {
        return kResultUnknown;
    }
    app_jump_parameters_ = {flags == ApplicationJumpFlags::UseCurrentParameters ? program_id_ : title_id,
                            flags, program_id_};
    return kResultSuccess;
}

Result AppletManager::DoApplicationJump() {
    context_.RequestExit(0, "DoApplicationJump asks for a system reset");
    return kResultSuccess;
}

void AppletManager::SendApplicationParameterAfterRegistration(const MessageParameter& parameter) {
    if (IsRegistered(parameter.destination_id)) {
        CancelAndSendParameter(parameter);
    } else {
        delayed_parameter_ = parameter;
    }
}

void AppletManager::SendCaptureBufferInfo(const std::vector<u8>& buffer) {
    if (buffer.size() >= sizeof(CaptureBufferInfo)) {
        capture_info_.emplace();
        std::memcpy(&*capture_info_, buffer.data(), sizeof(CaptureBufferInfo));
    }
}

ApplicationRunningMode AppletManager::GetApplicationRunningMode() {
    const auto* app = GetAppletSlot(AppletSlot::Application);
    if (app->applet_id == AppletId::None) {
        return ApplicationRunningMode::NoApplication;
    }
    return app->registered ? ApplicationRunningMode::Old3dsRegistered
                           : ApplicationRunningMode::Old3dsUnregistered;
}

}  // namespace mh4n::apt
