// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/service.h, mh4n/ipc.h, mh4n/svc.h,
// ipc_test.cpp, srv_test.cpp and independent/AGENTS.md.
#include "internal.h"

#include "mh4n/registry.h"
#include "mh4n/service.h"
#include "mh4n/svc.h"

#include <algorithm>
#include <cstdlib>

namespace mh4n {
namespace {

constexpr auto kLog = "Service";

Result CheckName(std::string_view name, std::size_t max_size) {
    if (name.empty() || name.size() > max_size) {
        return kResultSrvInvalidNameSize;
    }
    if (name.find('\0') != std::string_view::npos) {
        return kResultSrvNameContainsNul;
    }
    return kResultSuccess;
}

std::set<std::string_view> SelectedServices(std::string_view list) {
    std::set<std::string_view> names;
    while (!list.empty()) {
        const auto comma = list.find(',');
        auto name = list.substr(0, comma);
        const auto first = name.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            name.remove_prefix(first);
            name = name.substr(0, name.find_last_not_of(" \t") + 1);
            names.insert(name);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
    return names;
}

}  // namespace

struct ServerSession::Port {
    std::string name;
    u32 open = 0;
};

ServerSession::~ServerSession() {
    Close();
}

void ServerSession::Close() {
    if (port_) {
        --port_->open;
        port_.reset();
    }
    service_.reset();
}

ClientSession::ClientSession(Kernel& kernel, std::shared_ptr<ServerSession> server,
                             std::string port_name)
    : Object(kernel), server_(std::move(server)), port_name_(std::move(port_name)) {
    SetName(port_name_);
}

struct ServiceManager::Observers {
    std::vector<IpcObserver> callbacks;
};

ServiceManager::ServiceManager(ServiceEnvironment environment)
    : context_(std::move(environment)), observers_(std::make_shared<Observers>()) {
    context_.manager_ = this;
}

ServiceManager::~ServiceManager() {
    detail::ForgetServiceManager(this);
    if (svc_) {
        svc_->SetConnectToPortHandler({});
        svc_->SetSendSyncRequestHandler({});
    }
    waiting_.clear();
    for (const auto& session : sessions_) {
        if (auto server = session.lock()) {
            server->Close();
        }
    }
}

Result ServiceManager::AddEntry(std::map<std::string, Entry>& table, const std::string& name,
                                ServiceFactory factory) {
    if (!factory.create) {
        return kResultSrvAccessDenied;
    }
    if (table.contains(name)) {
        return kResultAlreadyRegistered;
    }
    table.emplace(name, Entry{std::move(factory), std::make_shared<ServerSession::Port>(
                                                     ServerSession::Port{name})});
    return kResultSuccess;
}

Result ServiceManager::RegisterService(const std::string& name, ServiceFactory factory) {
    const Result valid = CheckName(name, kMaxServiceNameLength);
    if (valid != kResultSuccess) {
        return valid;
    }
    const Result added = AddEntry(services_, name, std::move(factory));
    if (added != kResultSuccess) {
        return added;
    }
    std::vector<std::function<void()>> callbacks;
    const auto [begin, end] = waiting_.equal_range(name);
    for (auto it = begin; it != end; ++it) {
        callbacks.push_back(std::move(it->second));
    }
    waiting_.erase(begin, end);
    for (const auto& callback : callbacks) {
        callback();
    }
    return kResultSuccess;
}

Result ServiceManager::AddNamedPort(const std::string& name, ServiceFactory factory) {
    const Result valid = CheckName(name, kMaxPortNameLength);
    return valid == kResultSuccess ? AddEntry(ports_, name, std::move(factory)) : valid;
}

void ServiceManager::InstallRegistered() {
    constexpr std::string_view prefix = "new/";
    const char* env = std::getenv("MH4N_NEW_SERVICES");
    const auto selected = SelectedServices(env ? env : "all");  // default: the independent implementations; MH4N_NEW_SERVICES=none uses the older ones
    const auto& registry = Registry<ServiceFactory>::Instance();
    for (const auto& [key, factory] : registry.All()) {
        const std::string_view name = key;
        if (name.starts_with(prefix)) {
            continue;
        }
        const ServiceFactory* replacement = nullptr;
        if (selected.contains("all") || selected.contains(name)) {  // "all" (Claude's addition): every service that has a new/ one
            replacement = registry.Find(fmt::format("{}{}", prefix, name));
            if (replacement != nullptr) {
                MH4N_LOG(Info, kLog, "service '{}': the new implementation", name);
            }
        }
        const auto& chosen = replacement ? *replacement : factory;
        if (factory.named_port) {
            AddNamedPort(key, chosen);
        } else {
            RegisterService(key, chosen);
        }
    }
    // "all": every independent service whose older counterpart is not built into the program is installed under its real name
    if (selected.contains("all")) {
        for (const auto& [key, factory] : registry.All()) {
            const std::string_view name = key;
            if (!name.starts_with(prefix)) {
                continue;
            }
            const std::string real(name.substr(prefix.size()));
            if (registry.Find(real)) {
                continue;  // handled above
            }
            if (factory.named_port) {
                AddNamedPort(real, factory);
            } else {
                RegisterService(real, factory);
            }
        }
    }
    for (const auto name : selected) {
        if (registry.Find(name)) {
            continue;
        }
        if (const auto* factory = registry.Find(fmt::format("{}{}", prefix, name))) {
            if (factory->named_port) {
                AddNamedPort(std::string(name), *factory);
            } else {
                RegisterService(std::string(name), *factory);
            }
        }
    }
}

bool ServiceManager::IsServiceRegistered(const std::string& name) const {
    return services_.contains(name);
}

Result ServiceManager::ConnectToPort(const std::string& name, std::shared_ptr<Object>* session) {
    const auto entry = ports_.find(name);
    if (entry == ports_.end()) {
        return kResultUnknownPort;
    }
    std::shared_ptr<ClientSession> client;
    const Result result = Connect(entry->second, &client);
    if (result == kResultSuccess) {
        *session = std::move(client);
    }
    return result;
}

Result ServiceManager::ConnectToService(const std::string& name,
                                        std::shared_ptr<ClientSession>* session) {
    const Result valid = CheckName(name, kMaxServiceNameLength);
    if (valid != kResultSuccess) {
        return valid;
    }
    const auto entry = services_.find(name);
    return entry == services_.end() ? kResultSrvServiceNotRegistered : Connect(entry->second, session);
}

void ServiceManager::NotifyWhenRegistered(const std::string& name, std::function<void()> fn) {
    waiting_.emplace(name, std::move(fn));
}

Result ServiceManager::Connect(Entry& entry, std::shared_ptr<ClientSession>* session) {
    if (entry.port->open >= entry.factory.max_sessions) {
        return kResultMaxConnectionsReached;
    }
    auto service = entry.factory.create(context_);
    if (!service) {
        return kResultSrvAccessDenied;
    }
    *session = MakeSession(std::move(service), entry.port, entry.port->name);
    return kResultSuccess;
}

void ServiceManager::PruneSessions() {
    std::erase_if(sessions_, [](const auto& session) { return session.expired(); });
}

std::shared_ptr<ClientSession> ServiceManager::MakeSession(std::unique_ptr<Service> service,
                                                         std::shared_ptr<ServerSession::Port> port,
                                                         const std::string& port_name) {
    auto server = std::shared_ptr<ServerSession>(new ServerSession());
    server->name_ = port ? port_name : service->Name();
    server->service_ = std::move(service);
    server->port_ = std::move(port);
    if (server->port_) {
        ++server->port_->open;
    }
    PruneSessions();
    sessions_.push_back(server);
    return std::make_shared<ClientSession>(context_.GetKernel(), std::move(server), port_name);
}

std::shared_ptr<ClientSession> ServiceManager::CreateSession(std::unique_ptr<Service> service) {
    return MakeSession(std::move(service), nullptr, "");
}

void ServiceManager::Install(SvcTable& svc) {
    svc_ = &svc;
    svc.SetConnectToPortHandler([this](const auto& name, auto* session) {
        return ConnectToPort(name, session);
    });
    svc.SetSendSyncRequestHandler([this](Thread& thread, Handle handle) {
        return SendSyncRequest(thread, handle);
    });
}

Result ServiceManager::SendSyncRequest(Thread& thread, Handle handle) {
    const auto process = thread.owner_process.lock();
    auto session = process ? process->handle_table.Get<ClientSession>(handle) : nullptr;
    if (!session) {
        return kResultInvalidHandle;
    }
    auto* service = session->Server()->GetService();
    if (!service) {
        return kResultSessionClosed;
    }

    auto& kernel = context_.GetKernel();
    auto request = std::make_shared<Request>(kernel, ObjectCast<Thread>(thread.shared_from_this()),
                                             session, next_sequence_++);
    std::array<u32, ipc::kCommandAreaLength> area{};
    const bool read = kernel.Memory().ReadBlock(area.data(), thread.tls_address + 0x80, sizeof(area));
    const Result translated = read ? request->Translate(area) : kResultInvalidBufferDescriptor;
    for (const auto& observer : observers_->callbacks) {
        if (observer.sent) {
            observer.sent(*request, translated);
        }
    }
    if (translated != kResultSuccess) {
        request->ReleaseKernelObjects();
        return translated;
    }

    request->SetRepliedHook([weak = std::weak_ptr(observers_)](const Request& answered,
                                                            std::span<const u32> words) {
        if (const auto observers = weak.lock()) {
            for (const auto& observer : observers->callbacks) {
                if (observer.replied) {
                    observer.replied(answered, words);
                }
            }
        }
    });

    Handled handled;
    {
        std::optional<ServiceScope> accounting;
        if (auto* scheduler = context_.env_.scheduler; scheduler && scheduler->CurrentThread() == &thread) {
            accounting.emplace(*scheduler, service->Name());
        }
        handled = service->Handle(*request);
    }
    if (handled == Handled::Deferred) {
        if (thread.status == ThreadStatus::Running || thread.status == ThreadStatus::WaitIPC) {
            thread.status = ThreadStatus::WaitIPC;
            thread.wake_hook = [weak = std::weak_ptr(request)](
                                   ThreadWakeupReason reason, WaitObject*, std::optional<Result>&, s32&) {
                if (reason == ThreadWakeupReason::Terminated) {
                    if (const auto pending = weak.lock()) {
                        pending->Cancel();
                    }
                }
            };
        }
        return kResultSuccess;
    }

    if (!request->HasReply()) {
        MH4N_LOG(Warning, kLog, "{}: the handler returned without a reply", service->Name());
        request->MakeReply(1, 0).Push(kResultSuccess);
    }
    const auto words = request->WriteReply(*request->GetReply());
    request->replied_hook_(*request, words);
    request->ReleaseKernelObjects();
    request->MarkFinished();

    if (reply_delay_ns_ > 0 && thread.status == ThreadStatus::Running) {
        thread.status = ThreadStatus::WaitIPC;
        thread.ArmTimeout(reply_delay_ns_);
    }
    return kResultSuccess;
}

void ServiceManager::AddObserver(IpcObserver observer) {
    observers_->callbacks.push_back(std::move(observer));
}

bool ServiceManager::ReportUnimplemented(const std::string& service, u32 header, const char* name) {
    if (!reported_.emplace(service, header).second) {
        return false;
    }
    MH4N_LOG(Warning, kLog, "unimplemented {} 0x{:08x} {}", service, header, name ? name : "");
    return true;
}

}  // namespace mh4n
