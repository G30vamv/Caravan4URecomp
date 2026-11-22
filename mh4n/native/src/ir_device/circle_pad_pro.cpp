// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: circle_pad_pro.h, ir_device_test.cpp hardware vectors, ir_cpp_test.cpp and 3dbrew Circle Pad Pro.

#include "circle_pad_pro.h"

#include <algorithm>
#include <utility>

#include "mh4n/clock.h"
#include "mh4n/log.h"

namespace mh4n::ir_device {
namespace {

constexpr auto kLog = "IR";

u32 EncodeAxis(s16 raw) {
    const float position = std::clamp(static_cast<float>(raw) / kCStickRadiusRaw, -1.0f, 1.0f);
    return static_cast<u32>(2048.0f + position * 2047.0f);
}

}  // namespace

const std::array<u8, kCalibrationSize> kCalibrationData = [] {
    constexpr std::array<u8, 16> block{
        0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F,
        0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
    };
    std::array<u8, kCalibrationSize> data{};
    for (std::size_t offset = 0; offset < data.size(); offset += block.size()) {
        std::copy(block.begin(), block.end(), data.begin() + offset);
    }
    data[0] = 0;
    data[15] = 0xF5;
    return data;
}();

std::array<u8, kStatusSize> BuildStatus(const InputState& input) {
    const u32 axes = EncodeAxis(input.cstick_x) | (EncodeAxis(input.cstick_y) << 12);
    u8 buttons = 0x80 | kBatteryLevel;
    if (!input.zl) {
        buttons |= 0x20;
    }
    if (!input.zr) {
        buttons |= 0x40;
    }
    return {static_cast<u8>(ResponseId::PollHid), static_cast<u8>(axes),
            static_cast<u8>(axes >> 8), static_cast<u8>(axes >> 16), buttons, 0};
}

CirclePadPro::CirclePadPro(SendFunc send, Timer& timer, InputFunc input)
    : send_(std::move(send)), timer_(timer), input_(std::move(input)) {}

CirclePadPro::~CirclePadPro() {
    OnDisconnect();
}

void CirclePadPro::OnConnect() {}

void CirclePadPro::OnDisconnect() {
    ++generation_;
    polling_ = false;
    timer_.Cancel();
}

void CirclePadPro::OnReceive(std::span<const u8> data) {
    if (data.empty()) {
        MH4N_LOG(Warning, kLog, "Empty Circle Pad Pro request");
        return;
    }

    const u8 id = data.front();
    const auto command = static_cast<RequestId>(id);
    std::size_t expected;
    switch (command) {
    case RequestId::ConfigureHidPolling: expected = 3; break;
    case RequestId::ReadCalibrationData: expected = 6; break;
    default:
        if (!std::exchange(logged_unknown_[id], true)) {
            MH4N_LOG(Warning, kLog, "Unknown Circle Pad Pro request {:#x}", id);
        }
        return;
    }

    if (data.size() != expected) {
        if (!std::exchange(logged_size_[id], true)) {
            MH4N_LOG(Warning, kLog, "Circle Pad Pro request {:#x} has size {}, expected {}",
                     id, data.size(), expected);
        }
        return;
    }
    if (command == RequestId::ConfigureHidPolling) {
        HandleConfigureHidPolling(data);
    } else {
        HandleReadCalibrationData(data);
    }
}

void CirclePadPro::HandleConfigureHidPolling(std::span<const u8> request) {
    OnDisconnect();
    period_ms_ = request[1];
    polling_ = period_ms_ != 0;
    if (polling_) {
        Schedule();
    }
}

void CirclePadPro::HandleReadCalibrationData(std::span<const u8> request) {
    const std::size_t offset = (static_cast<u16>(request[3]) << 8 | request[2]) & 0xFFF0u;
    const std::size_t size = (static_cast<u16>(request[5]) << 8 | request[4]) & 0xFFF0u;
    if (offset + size > kCalibrationSize) {
        return;
    }

    std::array<u8, 5 + kCalibrationSize> reply{
        static_cast<u8>(ResponseId::ReadCalibrationData), request[2], request[3], request[4], request[5],
    };
    std::copy_n(kCalibrationData.data() + offset, size, reply.data() + 5);
    send_(std::span(reply).first(5 + size));
}

void CirclePadPro::SendHidStatus() {
    send_(BuildStatus(input_ ? input_() : InputState{}));
}

void CirclePadPro::Schedule() {
    timer_.Schedule(kTicksPerSecond * period_ms_ / 1000, [this, generation = generation_] {
        if (generation == generation_ && polling_) {
            SendHidStatus();
            if (generation == generation_ && polling_) {
                Schedule();
            }
        }
    });
}

}  // namespace mh4n::ir_device
