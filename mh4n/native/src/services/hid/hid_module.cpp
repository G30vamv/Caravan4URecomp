// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: HID contract headers, hid_test.cpp and 3dbrew HID Shared Memory.

#include "hid_module.h"

#include <cmath>
#include <cstring>
#include <iterator>

namespace mh4n::hid {
namespace {

template <typename Block, typename Entry>
void Append(Block& block, u32& index, const Entry& entry, u64 tick) {
    block.entries[index] = entry;
    if (index == 0) {
        block.index_reset_ticks_previous = block.index_reset_ticks;
        block.index_reset_ticks = static_cast<s64>(tick);
    }
    block.index = index;
    index = (index + 1) % static_cast<u32>(std::size(block.entries));
}

template <typename Block>
Block ReadBlock(const u8* memory) {
    Block block;
    std::memcpy(&block, memory, sizeof block);
    return block;
}

s16 AverageAxis(s16 sample, std::array<s16, kCirclePadAveraging - 1>& history) {
    const int average = (sample + history[0] + history[1]) / kCirclePadAveraging;
    history[1] = history[0];
    history[0] = sample;
    return static_cast<s16>(average);
}

}  // namespace

GyroscopeCalibrateParam DefaultGyroscopeCalibrateParam() {
    return {{{0, 6700, -6700}, {0, 6700, -6700}, {0, 6700, -6700}}};
}

DirectionState GetStickDirectionState(s16 circle_pad_x, s16 circle_pad_y) {
    const s64 x = circle_pad_x;
    const s64 y = circle_pad_y;
    const s64 x_squared = x * x;
    const s64 y_squared = y * y;
    if (x_squared + y_squared <= 40 * 40) {
        return {};
    }
    // The squared tangent at 30 degrees is 1/3. At 60 degrees it is 3.
    const bool vertical = 3 * y_squared > x_squared;
    const bool horizontal = 3 * x_squared > y_squared;
    return {vertical && y > 0, vertical && y < 0, horizontal && x < 0, horizontal && x > 0};
}

s16 CirclePadRaw(float stick) {
    return static_cast<s16>(std::lround(stick * kMaxCirclePadPosition));
}

void Module::UpdatePad(u8* memory, const InputState& input, u64 tick) {
    if (!memory) {
        return;
    }

    auto pad = ReadBlock<decltype(SharedMem::pad)>(memory + kPadOffset);
    const s16 x = AverageAxis(input.circle_x, circle_old_x_);
    const s16 y = AverageAxis(input.circle_y, circle_old_y_);
    const auto directions = GetStickDirectionState(x, y);
    const u32 buttons = (input.buttons & kPadButtonMask) |
                        (directions.up ? kPadCircleUp : 0) |
                        (directions.down ? kPadCircleDown : 0) |
                        (directions.left ? kPadCircleLeft : 0) |
                        (directions.right ? kPadCircleRight : 0);
    Append(pad, next_pad_index_, PadDataEntry{buttons, buttons & ~state_, state_ & ~buttons, x, y}, tick);
    state_ = buttons;
    pad.current_state = buttons;
    pad.sliderstate_3d = 0.0f;
    std::memcpy(memory + kPadOffset, &pad, sizeof pad);

    auto touch = ReadBlock<decltype(SharedMem::touch)>(memory + kTouchOffset);
    auto entry = touch.entries[next_touch_index_];
    entry.x = input.touch_down ? input.touch_x : 0;
    entry.y = input.touch_down ? input.touch_y : 0;
    entry.valid = (entry.valid & ~0x7Fu) | static_cast<u32>(input.touch_down);
    Append(touch, next_touch_index_, entry, tick);
    std::memcpy(memory + kTouchOffset, &touch, sizeof touch);
}

void Module::UpdateAccelerometer(u8* memory, const InputState& input, u64 tick) {
    if (!memory) {
        return;
    }

    auto block = ReadBlock<decltype(SharedMem::accelerometer)>(memory + kAccelerometerOffset);
    const auto& [x, y, z] = input.accel;
    block.raw_entry = {static_cast<s16>(-2 * x), static_cast<s16>(-2 * z), static_cast<s16>(2 * y)};
    Append(block, next_accelerometer_index_, AccelerometerDataEntry{x, y, z}, tick);
    std::memcpy(memory + kAccelerometerOffset, &block, sizeof block);
}

void Module::UpdateGyroscope(u8* memory, const InputState& input, u64 tick) {
    if (!memory) {
        return;
    }

    auto block = ReadBlock<decltype(SharedMem::gyroscope)>(memory + kGyroscopeOffset);
    const auto& [x, y, z] = input.gyro;
    block.raw_entry = {x, z, static_cast<s16>(-y)};
    Append(block, next_gyroscope_index_, GyroscopeDataEntry{x, y, z}, tick);
    std::memcpy(memory + kGyroscopeOffset, &block, sizeof block);
}

}  // namespace mh4n::hid
