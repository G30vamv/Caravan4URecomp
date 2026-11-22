// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: applet contracts, apt_test.cpp and applet_frontend_test.cpp black-box fixtures.

#include "erreula.h"

#include <algorithm>

#include "frontend_hub.h"
#include "mh4n/log.h"

namespace mh4n::apt {

Result ErrEula::ReceiveParameterImpl(const MessageParameter& parameter) {
    return AnswerFramebufferRequest(context_, parameter, framebuffer_memory_, "Error/EULA");
}

Result ErrEula::Start(const MessageParameter& parameter) {
    param_.fill(0);
    std::copy_n(parameter.buffer.begin(), std::min(param_.size(), parameter.buffer.size()),
                param_.begin());
    const auto type = static_cast<ErrEulaDisplayType>(param_[errlayout::kType]);
    if (type == ErrEulaDisplayType::Agree) {
        std::fill_n(param_.begin() + errlayout::kResult, 4, u8{0});
        param_[errlayout::kResult] = 1;
    } else if (type == ErrEulaDisplayType::ErrorCode || type == ErrEulaDisplayType::Error) {
        const auto hub = FindFrontendHub(context_);
        if (auto frontend = hub ? hub->Error() : nullptr) {
            u32 code = 0;
            for (unsigned i = 0; i < 4; ++i) {
                code |= u32{param_[errlayout::kErrorCode + i]} << (8 * i);
            }
            std::u16string message;
            for (std::size_t i = 0; i < kErrEulaMessageSize; ++i) {
                const auto offset = errlayout::kMessage + i * 2;
                const auto unit = static_cast<char16_t>(param_[offset] | (u32{param_[offset + 1]} << 8));
                if (!unit) {
                    break;
                }
                message.push_back(unit);
            }
            frontend->Show(code, message);
        }
    }
    MH4N_LOG(Info, "Service.APT", "the error applet is closed at once");
    return Finalize();
}

Result ErrEula::Finalize() {
    CloseApplet(nullptr, {param_.begin(), param_.end()});
    return kResultSuccess;
}

}  // namespace mh4n::apt
