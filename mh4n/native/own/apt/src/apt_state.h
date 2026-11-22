// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew NS and APT Services, and boot-village.ipc.trace seq=10,12,67,69,71.
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "mh4n/types.h"

namespace mh4n {
class Object;

namespace apt {

inline constexpr u32 kApplication = 0x300;
inline constexpr u32 kApplicationPosition = 0;
inline constexpr u32 kNoPosition = 0xFFFFFFFF;
inline constexpr u32 kWakeup = 1;
inline constexpr std::size_t kMaxParameterSize = 0x1000;

struct Parameter {
    u32 sender;
    u32 receiver;
    u32 command;
    std::shared_ptr<Object> object;
    std::vector<u8> bytes;
};

// One application slot and one pending parameter in the single-title runtime.
class Application {
public:
    bool Initialize(u32 id, u32 attributes) {
        if (phase_ != Phase::Uninitialized || id != kApplication || (attributes & 7) != kApplicationPosition) {
            return false;
        }
        attributes_ = attributes;
        parameter_ = Parameter{0, kApplication, kWakeup, {}, {}};
        phase_ = Phase::Initialized;
        return true;
    }

    bool Enable(u32 attributes) {
        if (phase_ != Phase::Initialized || (attributes & 7) != kApplicationPosition) {
            return false;
        }
        attributes_ = attributes;
        phase_ = Phase::Active;
        return true;
    }

    bool Loaded(u32 id) const {
        return id == kApplication && phase_ != Phase::Uninitialized && phase_ != Phase::Closed;
    }

    bool Registered(u32 id) const { return Loaded(id) && phase_ != Phase::Initialized; }
    bool Active() const { return phase_ == Phase::Active; }
    u32 Attributes() const { return attributes_; }

    bool NotifyToWait(u32 id) {
        if (!Registered(id) || (phase_ != Phase::Active && phase_ != Phase::Waiting)) {
            return false;
        }
        phase_ = Phase::Waiting;
        return true;
    }

    bool Send(Parameter parameter) {
        if (!Loaded(parameter.receiver) || parameter_ || parameter.bytes.size() > kMaxParameterSize) {
            return false;
        }
        parameter_ = std::move(parameter);
        return true;
    }

    bool Wakeup() {
        return phase_ == Phase::Waiting && Send(Parameter{0, kApplication, kWakeup, {}, {}});
    }

    const Parameter* Pending(u32 receiver) const {
        return parameter_ && parameter_->receiver == receiver ? &*parameter_ : nullptr;
    }

    void Consume() {
        const u32 command = parameter_->command;
        parameter_.reset();
        if (phase_ == Phase::Waiting && (command == kWakeup || (command >= 10 && command <= 15) || command == 17)) {
            phase_ = Phase::Active;
        }
    }

    bool Cancel(std::optional<u32> sender, std::optional<u32> receiver) {
        if (!parameter_ || (sender && *sender != parameter_->sender) ||
            (receiver && *receiver != parameter_->receiver)) {
            return false;
        }
        parameter_.reset();
        return true;
    }

    bool PrepareToClose() {
        if (!Registered(kApplication)) {
            return false;
        }
        phase_ = Phase::Closing;
        return true;
    }

    bool Close() {
        if (phase_ != Phase::Closing) {
            return false;
        }
        parameter_.reset();
        phase_ = Phase::Closed;
        return true;
    }

    bool Finalize(u32 id) {
        if (!Loaded(id)) {
            return false;
        }
        *this = Application{};
        return true;
    }

private:
    enum class Phase { Uninitialized, Initialized, Active, Waiting, Closing, Closed };
    Phase phase_ = Phase::Uninitialized;
    u32 attributes_ = 0;
    std::optional<Parameter> parameter_;
};

}  // namespace apt
}  // namespace mh4n
