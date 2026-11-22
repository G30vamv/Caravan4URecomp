// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew NCCH/Extended Header, mh4n/loader.h and loader_test.cpp.

#include "image_fields.h"
#include "mh4n/loader.h"

#include <algorithm>

namespace mh4n {
namespace {

void ReadCapabilities(const u8* data, KernelCaps& caps) {
    constexpr unsigned count = 28;
    constexpr u32 page_mask = 0xFFFFF;
    const auto descriptor = [data](unsigned index) {
        return static_cast<u32>(loader_detail::ReadLe(data + index * 4, 4));
    };
    const auto is_range = [](u32 word) { return (word & 0xFFE00000) == 0xFF800000; };

    for (unsigned i = 0; i < count; ++i) {
        const u32 word = descriptor(i);
        if ((word & 0xF8000000) == 0xF0000000) {
            const unsigned first = ((word >> 24) & 7) * 24;
            for (unsigned bit = 0; bit < 24 && first + bit < caps.svc_mask.size(); ++bit) {
                if (word & (1u << bit)) {
                    caps.svc_mask.set(first + bit);
                }
            }
        } else if ((word & 0xFE000000) == 0xFC000000) {
            caps.kernel_version = word & 0xFFFF;
        } else if ((word & 0xFF000000) == 0xFE000000) {
            caps.handle_table_size = word & 0x7FFFF;
        } else if ((word & 0xFF800000) == 0xFF000000) {
            caps.flags = static_cast<u16>(word);
        } else if (is_range(word) && i + 1 < count && is_range(descriptor(i + 1))) {
            const u32 end = descriptor(++i);
            const u32 first_page = word & page_mask;
            const u32 end_page = end & page_mask;
            if (first_page < end_page) {
                caps.address_mappings.push_back({first_page << 12, (end_page - first_page) << 12,
                                                 (word & 0x100000) != 0, (end & 0x100000) != 0});
            }
        } else if ((word & 0xFFF00000) == 0xFFE00000) {
            caps.address_mappings.push_back({(word & page_mask) << 12, 0x1000, false, false});
        }
    }
}

}  // namespace

bool ParseExHeader(const u8* data, std::size_t size, ExHeader* out, std::string* error) {
    if (!data || size < kExHeaderMinSize) {
        return loader_detail::Fail(error, "exheader.bin is too small");
    }
    if (!out) {
        return loader_detail::Fail(error, "no exheader output");
    }

    const auto le32 = [data](std::size_t offset) {
        return static_cast<u32>(loader_detail::ReadLe(data + offset, 4));
    };
    const auto segment = [&](std::size_t offset) {
        return ExHeaderSegment{le32(offset), le32(offset + 4), le32(offset + 8)};
    };

    ExHeader header;
    header.title.assign(data, std::find(data, data + 8, u8{0}));
    header.flags = data[0xD];
    header.remaster_version = static_cast<u16>(loader_detail::ReadLe(data + 0xE, 2));
    header.text = segment(0x10);
    header.ro = segment(0x20);
    header.data = segment(0x30);
    header.stack_size = le32(0x1C);
    header.bss_size = le32(0x3C);
    header.program_id = loader_detail::ReadLe(data + 0x200, 8);
    header.core_version = le32(0x208);
    header.enable_l2_cache = (data[0x20C] & 1) != 0;
    header.enable_804mhz_cpu = (data[0x20C] & 2) != 0;
    header.n3ds_mode = data[0x20D] & 0xF;
    header.ideal_processor = data[0x20E] & 3;
    header.affinity_mask = (data[0x20E] >> 2) & 3;
    header.system_mode = data[0x20E] >> 4;
    header.priority = data[0x20F];
    header.max_cpu = data[0x210] & 0x7F;
    header.schedule_mode = data[0x210] >> 7;
    header.resource_limit_category = data[0x36F];
    ReadCapabilities(data + 0x370, header.kernel_caps);
    *out = std::move(header);
    return true;
}

}  // namespace mh4n
