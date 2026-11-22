// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: applet contracts, apt_test.cpp and applet_frontend_test.cpp black-box fixtures.

#include "mii_selector.h"

#include <algorithm>

#include "mh4n/log.h"

namespace mh4n::apt {

u16 MiiCrc16(const u8* data, std::size_t size) {
    u32 crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= u32{data[i]} << 8;
        for (int bit = 0; bit < 8; ++bit) {
            const u32 polynomial = (crc & 0x8000) != 0 ? 0x1021u : 0u;
            crc = (crc << 1) ^ polynomial;
        }
        crc &= 0xFFFF;
    }
    return static_cast<u16>(crc);
}

std::array<u8, kMiiResultSize> StandardMiiResult() {
    // Serialized default Mii from the black-box fixture, up to the CRC.
    constexpr std::array<u8, kChecksummedMiiSize - 2> data{
        0x03, 0x00, 0x10, 0x30, 0xD2, 0x85, 0xB6, 0xB3, 0x00, 0xC8, 0x85, 0x0A, 0x98, 0x39, 0x1E, 0xE4,
        0x40, 0xF4, 0x07, 0xB7, 0x37, 0x10, 0x00, 0x00, 0xA6, 0x00, 0x43, 0x00, 0x69, 0x00, 0x74, 0x00,
        0x72, 0x00, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x40,
        0x00, 0x00, 0x21, 0x01, 0x02, 0x68, 0x44, 0x18, 0x26, 0x34, 0x46, 0x14, 0x81, 0x12, 0x17, 0x68,
        0x0D, 0x00, 0x00, 0x29, 0x00, 0x52, 0x48, 0x50, 0x66, 0x00, 0x6C, 0x00, 0x54, 0x00, 0x6F, 0x00,
        0x62, 0x00, 0x69, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    std::array<u8, kMiiResultSize> result{};
    std::fill_n(result.begin() + miiresult::kSelectedGuestMiiIndex, 4, u8{0xFF});
    auto output = result.begin() + miiresult::kSelectedMiiData;
    std::copy(data.begin(), data.end(), output);
    const u16 crc = MiiCrc16(data.data(), data.size());
    output[data.size()] = static_cast<u8>(crc >> 8);
    output[data.size() + 1] = static_cast<u8>(crc);
    return result;
}

Result MiiSelector::ReceiveParameterImpl(const MessageParameter& parameter) {
    return AnswerFramebufferRequest(context_, parameter, framebuffer_memory_, "Mii Selector");
}

Result MiiSelector::Start(const MessageParameter& parameter) {
    if (parameter.buffer.size() != kMiiConfigSize) {
        MH4N_LOG(Warning, "Service.APT",
                 "The size of the parameter (MiiConfig) is wrong: {} bytes, expected {}; the applet never closes",
                 parameter.buffer.size(), kMiiConfigSize);
        return kResultUnknown;
    }
    result_ = StandardMiiResult();
    is_active_ = true;
    return kResultSuccess;
}

Result MiiSelector::Finalize() {
    CloseApplet(nullptr, {result_.begin(), result_.end()});
    return kResultSuccess;
}

void MiiSelector::Update() {
    if (is_active_) {
        Finalize();
    }
}

}  // namespace mh4n::apt
