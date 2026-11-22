// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/applet_frontend.h, frontend_hub.h and applet_frontend_test.cpp.

#include "frontend_hub.h"

#include <map>

#include "mh4n/runtime.h"
#include "swkbd.h"

namespace mh4n {
namespace {

struct FrontendState {
    std::shared_ptr<apt::FrontendHub> hub;
};

std::mutex runtime_hubs_mutex;
std::map<Runtime*, std::weak_ptr<apt::FrontendHub>> runtime_hubs;

std::shared_ptr<apt::FrontendHub> RuntimeHub(Runtime& runtime, bool create) {
    std::lock_guard lock(runtime_hubs_mutex);
    const auto entry = runtime_hubs.find(&runtime);
    if (entry != runtime_hubs.end()) {
        if (auto hub = entry->second.lock()) {
            return hub;
        }
    }
    if (!create) {
        return {};
    }
    std::erase_if(runtime_hubs, [](const auto& item) { return item.second.expired(); });
    auto hub = std::make_shared<apt::FrontendHub>();
    runtime.Own(hub);
    runtime_hubs[&runtime] = hub;
    return hub;
}

// The runner's scripted frontend posts through the scheduler, yielding back from Execute first.
class ScriptedKeyboard final : public KeyboardFrontend {
public:
    ScriptedKeyboard(Kernel& kernel, std::weak_ptr<apt::FrontendHub> hub,
                     const std::vector<std::string>& answers)
        : kernel_(kernel), hub_(std::move(hub)), answers_(answers) {}

    void Execute(const KeyboardRequest& request) override {
        if (next_ == answers_.size()) {
            return;
        }
        apt::KeyboardAnswer answer{apt::Utf8ToUtf16(answers_[next_++]), request.OkButton()};
        kernel_.Post([hub = hub_, answer = std::move(answer)]() mutable {
            if (auto live = hub.lock()) {
                live->Post(std::move(answer));
            }
        });
    }

private:
    Kernel& kernel_;
    std::weak_ptr<apt::FrontendHub> hub_;
    std::vector<std::string> answers_;
    std::size_t next_ = 0;
};

void AttachFrontends(Runtime& runtime) {
    auto* services = FindServiceManager(runtime);
    if (!services) {
        return;
    }
    auto hub = RuntimeHub(runtime, false);
    const auto answers = runtime.GetOptions().All("--test-keyboard-frontend");
    if (!answers.empty() && (!hub || !hub->Keyboard())) {
        hub = RuntimeHub(runtime, true);
        hub->SetKeyboard(std::make_shared<ScriptedKeyboard>(runtime.GetKernel(), hub, answers));
    }
    services->Context().State<FrontendState>("applet_frontend")->hub = std::move(hub);
}

MH4N_OPTION("runner", "--test-keyboard-frontend", "TEXT", "answer the keyboard with TEXT through its frontend");
MH4N_REGISTER(BootStep, "services.ipc.applets", BootStep{Phase::Services, AttachFrontends});

}  // namespace

KeyboardResult ValidateKeyboard(const KeyboardRequest& request, const std::u16string& text, u8 button) {
    return static_cast<KeyboardResult>(apt::CheckFinalize(apt::ToFrontendConfig(request), text, button));
}

void FinishKeyboard(Runtime& runtime, std::u16string text, u8 button) {
    if (auto hub = RuntimeHub(runtime, false); hub && hub->Keyboard()) {
        hub->Post({std::move(text), button});
    }
}

void SetKeyboardFrontend(Runtime& runtime, std::shared_ptr<KeyboardFrontend> frontend) {
    RuntimeHub(runtime, true)->SetKeyboard(std::move(frontend));
}

void SetErrorFrontend(Runtime& runtime, std::shared_ptr<ErrorFrontend> frontend) {
    RuntimeHub(runtime, true)->SetError(std::move(frontend));
}

namespace apt {

void FrontendHub::SetKeyboard(std::shared_ptr<KeyboardFrontend> frontend) {
    std::lock_guard lock(mutex_);
    keyboard_.swap(frontend);
    answer_.reset();
}

void FrontendHub::SetError(std::shared_ptr<ErrorFrontend> frontend) {
    std::lock_guard lock(mutex_);
    error_.swap(frontend);
}

std::shared_ptr<KeyboardFrontend> FrontendHub::Keyboard() const {
    std::lock_guard lock(mutex_);
    return keyboard_;
}

std::shared_ptr<ErrorFrontend> FrontendHub::Error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void FrontendHub::Post(KeyboardAnswer answer) {
    std::lock_guard lock(mutex_);
    answer_ = std::move(answer);
}

std::optional<KeyboardAnswer> FrontendHub::Take() {
    std::lock_guard lock(mutex_);
    return std::exchange(answer_, std::nullopt);
}

void FrontendHub::Drop() {
    std::lock_guard lock(mutex_);
    answer_.reset();
}

std::shared_ptr<FrontendHub> FindFrontendHub(ServiceContext& context) {
    return context.State<FrontendState>("applet_frontend")->hub;
}

}  // namespace apt
}  // namespace mh4n
