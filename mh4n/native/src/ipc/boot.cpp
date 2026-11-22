// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/runtime.h, mh4n/service.h and ipc/internal.h.
#include "internal.h"

#include "mh4n/runtime.h"
#include "mh4n/service.h"

#include <mutex>

namespace mh4n {
namespace {

struct RuntimeManagers {
    std::mutex mutex;
    std::map<Runtime*, ServiceManager*> managers;
};

RuntimeManagers& Managers() {
    static RuntimeManagers table;
    return table;
}

void InstallServices(Runtime& runtime) {
    auto manager = std::make_shared<ServiceManager>(ServiceEnvironment{
        .game_folder = runtime.GameFolder(),
        .user_folder = runtime.UserFolder(),
        .options = &runtime.GetOptions(),
        .kernel = &runtime.GetKernel(),
        .scheduler = &runtime.Scheduler(),
        .threads = &runtime.Threads(),
        .interfaces = &runtime.Interfaces(),
        .request_exit = [&runtime](int status, const std::string& reason) {
            runtime.RequestExit(status, reason);
        },
    });
    manager->InstallRegistered();
    manager->Install(runtime.Svc());
    detail::RememberServiceManager(&runtime, manager.get());
    runtime.Own(std::move(manager));
}

MH4N_REGISTER(BootStep, "services", BootStep{Phase::Services, InstallServices});

}  // namespace

namespace detail {

void RememberServiceManager(Runtime* runtime, ServiceManager* manager) {
    auto& table = Managers();
    const std::lock_guard lock(table.mutex);
    table.managers.insert_or_assign(runtime, manager);
}

void ForgetServiceManager(ServiceManager* manager) {
    auto& table = Managers();
    const std::lock_guard lock(table.mutex);
    std::erase_if(table.managers, [manager](const auto& entry) { return entry.second == manager; });
}

}  // namespace detail

ServiceManager* FindServiceManager(Runtime& runtime) {
    auto& table = Managers();
    const std::lock_guard lock(table.mutex);
    const auto found = table.managers.find(&runtime);
    return found == table.managers.end() ? nullptr : found->second;
}

}  // namespace mh4n
