// Derived from the Mint applet of this tree (mint.cpp), itself derived from Azahar src/core/hle/applets/mint.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it says "GPLv2 or any later version").
// Copyright 2016 Citra Emulator Project (the Mint applet's flow)
#include "applets/extrapad.h"

#include "mh4n/log.h"

namespace mh4n::apt {

namespace {
constexpr const char* kLog = "Service.APT";
}

Result Extrapad::ReceiveParameterImpl(const MessageParameter& parameter) {
    return AnswerFramebufferRequest(context_, parameter, framebuffer_memory_, "Extrapad");
}

Result Extrapad::Start(const MessageParameter& parameter) {
    startup_param_ = parameter.buffer;
    MH4N_LOG(Info, kLog, "the Extrapad applet {:03X} (Circle Pad Pro setup) is closed at once", static_cast<u32>(id_));
    // Let the application know that we're closing
    Finalize();
    return kResultSuccess;
}

Result Extrapad::Finalize() {
    CloseApplet(nullptr, std::vector<u8>(startup_param_.size(), 0));
    return kResultSuccess;
}

}  // namespace mh4n::apt
