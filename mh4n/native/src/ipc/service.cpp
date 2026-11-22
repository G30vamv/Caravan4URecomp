// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/service.h and ipc_test.cpp.
#include "mh4n/service.h"

namespace mh4n {

ServiceContext::ServiceContext(ServiceEnvironment environment) : env_(std::move(environment)) {}

void ServiceContext::RequestExit(int status, const std::string& reason) const {
    if (env_.request_exit) {
        env_.request_exit(status, reason);
    }
}

std::shared_ptr<void>& ServiceContext::StateSlot(std::string_view key) {
    return state_.try_emplace(std::string(key)).first->second;
}

std::shared_ptr<ClientSession> ServiceContext::CreateSession(std::unique_ptr<Service> service) {
    return Services().CreateSession(std::move(service));
}

Service::Service(ServiceContext& context, std::string name)
    : context_(context), name_(std::move(name)) {}

Service::~Service() = default;

Handled Service::Handle(Request& request) {
    const auto command = commands_.find(request.CommandId());
    if (command != commands_.end() && command->second.handler) {
        return command->second.handler(request);
    }
    ReplyUnimplemented(request);
    return Handled::Done;
}

const char* Service::CommandName(u16 command_id) const {
    const auto command = commands_.find(command_id);
    return command == commands_.end() ? nullptr : command->second.name;
}

void Service::AddCommand(u16 command_id, const char* name, Handler handler) {
    commands_.insert_or_assign(command_id, Command{name, std::move(handler)});
}

void Service::ReplyUnimplemented(Request& request) {
    context_.Services().ReportUnimplemented(name_, request.Header(), CommandName(request.CommandId()));
    request.MakeReply(1, 0).Push(kResultSuccess);
}

}  // namespace mh4n
