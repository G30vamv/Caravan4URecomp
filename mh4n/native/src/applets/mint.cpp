// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: applet contracts, apt_test.cpp and applet_frontend_test.cpp black-box fixtures.

#include "mint.h"

#include "mh4n/log.h"

namespace mh4n::apt {

Result Mint::ReceiveParameterImpl(const MessageParameter& parameter) {
    return AnswerFramebufferRequest(context_, parameter, framebuffer_memory_, "Mint");
}

Result Mint::Start(const MessageParameter& parameter) {
    startup_param_.resize(parameter.buffer.size());
    MH4N_LOG(Info, "Service.APT", "the Mint applet is closed at once");
    return Finalize();
}

Result Mint::Finalize() {
    CloseApplet(nullptr, startup_param_);
    return kResultSuccess;
}

}  // namespace mh4n::apt
