// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/loader.h, mh4n/memory.h and loader_test.cpp.

#include "image_fields.h"
#include "mh4n/loader.h"

#include <charconv>
#include <cstring>

namespace mh4n {

void FillConfigMemory(u8* page, const FcramRegions& regions) {
    using namespace config_mem;
    using loader_detail::WriteLe;
    std::memset(page, 0, kConfigMemorySize);
    page[kKernelVersionMin] = page[kFirmVersionMin] = 0x3A;
    page[kKernelVersionMaj] = page[kFirmVersionMaj] = 2;
    page[kUnitInfo] = page[kPrevFirm] = 1;
    WriteLe(page + kNsTid, 8, 0x0004013000008002);
    WriteLe(page + kSysCoreVer, 4, 2);
    WriteLe(page + kFirmSysCoreVer, 4, 2);
    WriteLe(page + kCtrSdkVer, 4, 0xF450);
    WriteLe(page + kFirmCtrSdkVer, 4, 0xF450);
    WriteLe(page + kAppMemType, 4, regions.MemoryMode());
    WriteLe(page + kAppMemAlloc, 4, regions.Size(FcramRegion::Application));
    WriteLe(page + kSysMemAlloc, 4, regions.Size(FcramRegion::System));
    WriteLe(page + kBaseMemAlloc, 4, regions.Size(FcramRegion::Base));
}

void FillSharedPage(u8* page) {
    using namespace shared_page;
    std::memset(page, 0, kSharedPageSize);
    page[kRunningHw] = 1;
    page[kWifiLinkLevel] = 3;
    page[kWifiState] = 2;
    page[kBatteryState] = 0x17;
    page[kUnknownValue] = 1;
}

InitTimeParse ParseInitTime(std::string_view text) {
    const auto split = text.find('.');
    const auto whole = text.substr(0, split);
    const auto fraction = split == text.npos ? std::string_view{} : text.substr(split + 1);
    const auto parse = [](std::string_view part, u64& value) {
        const auto end = part.data() + part.size();
        const auto result = std::from_chars(part.data(), end, value);
        return result.ec == std::errc{} && result.ptr == end;
    };
    u64 seconds = 0;
    u64 micros = 0;
    if (whole.empty() || whole.size() > 10 || !parse(whole, seconds) ||
        (split != text.npos && (fraction.empty() || fraction.size() > 6 || !parse(fraction, micros)))) {
        return {false, 0, "expected seconds with an optional fraction of up to six digits"};
    }
    for (auto digits = fraction.size(); digits < 6; ++digits) {
        micros *= 10;
    }
    micros += seconds * 1000000;
    if (micros < kMinInitTimeMicros || micros > kMaxInitTimeMicros) {
        return {false, 0, "date is outside the 3DS clock range"};
    }
    return {true, micros, {}};
}

Result SystemPages::Map(GuestMemory& memory, const FcramRegions& regions) {
    Result result = memory.Map(kConfigMemoryVaddr, kConfigMemorySize, MemPerm::Read, MemState::Shared);
    if (!IsSuccess(result)) {
        return result;
    }
    // The two pages have independent backing and must stay distinct in Query.
    result = memory.Map(kSharedPageVaddr, kSharedPageSize, MemPerm::Read, MemState::Shared, kSharedPageVaddr);
    if (!IsSuccess(result)) {
        memory.Unmap(kConfigMemoryVaddr, kConfigMemorySize);
        return result;
    }
    FillConfigMemory(memory.Base() + kConfigMemoryVaddr, regions);
    FillSharedPage(memory.Base() + kSharedPageVaddr);
    memory_ = &memory;
    UpdateSharedPage(0);
    return result;
}

u64 SystemPages::SystemTimeSince2000(u64 ticks) const {
    constexpr u64 epoch_us = kUnixTimeOf2000 * 1000000;
    const u64 elapsed_us = (ticks / kBaseClockRateArm11) * 1000000 +
                           ((ticks % kBaseClockRateArm11) * 1000000) / kBaseClockRateArm11;
    if (init_time_us_ < epoch_us) {
        const u64 before_epoch = epoch_us - init_time_us_;
        return elapsed_us < before_epoch ? 0 : (elapsed_us - before_epoch) / 1000;
    }
    const u64 initial_us = init_time_us_ - epoch_us;
    return initial_us / 1000 + (elapsed_us + initial_us % 1000) / 1000;
}

u64 SystemPages::SystemTimeSince1900(u64 ticks) const {
    return kMillisecondsFrom1900To2000 + SystemTimeSince2000(ticks);
}

void SystemPages::UpdateSharedPage(u64 ticks) {
    using namespace shared_page;
    using loader_detail::WriteLe;
    if (!memory_) {
        return;
    }
    u8* page = memory_->Base() + kSharedPageVaddr;
    const auto counter = static_cast<u32>(loader_detail::ReadLe(page + kDateTimeCounter, 4));
    u8* date = page + (counter & 1 ? kDateTime0 : kDateTime1);
    WriteLe(date + kDtDateTime, 8, SystemTimeSince1900(ticks));
    WriteLe(date + kDtUpdateTick, 8, ticks);
    WriteLe(date + kDtTickToSecond, 8, kBaseClockRateArm11);
    WriteLe(date + kDtTickOffset, 8, 0);
    WriteLe(page + kDateTimeCounter, 4, counter + 1);
}

void SystemPages::Set3DSlider(float value) {
    if (memory_) {
        std::memcpy(memory_->Base() + kSharedPageVaddr + shared_page::kSliderState3d, &value, sizeof value);
    }
}

void SystemPages::SetMacAddress(const std::array<u8, 6>& mac) {
    if (memory_) {
        std::memcpy(memory_->Base() + kSharedPageVaddr + shared_page::kWifiMacAddress, mac.data(), mac.size());
    }
}

std::array<u8, 6> SystemPages::GetMacAddress() const {
    std::array<u8, 6> mac{};
    if (memory_) {
        std::memcpy(mac.data(), memory_->Base() + kSharedPageVaddr + shared_page::kWifiMacAddress, mac.size());
    }
    return mac;
}

}  // namespace mh4n
