// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: cpuvertex and RHI contract headers, gpu_cpuvertex and gpu_cpuvertex_batch unit tests.

#include "native_programs.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace mh4n::gpu::cpuvertex {
namespace {

bool CacheByWords() {
    const char* value = std::getenv("MH4N_PROGRAMS_BY_WORDS");
    return !value || std::string_view(value) != "0";
}

} // namespace

void NativePrograms::SetTable(const PicaNativeEntry* entries, u32 count) {
    entries_.clear();
    known_.clear();
    groups_.clear();
    seen_.clear();
    seen_next_ = 0;
    if (!entries) {
        return;
    }

    for (u32 i = 0; i < count; ++i) {
        const auto& entry = entries[i];
        if (!entry.function || entry.code_words > pica::kMaxProgramWords ||
            entry.desc_count > pica::kMaxSwizzleWords) {
            continue;
        }
        entries_.push_back(entry);
        const bool geometry = entry.geometry != 0;
        auto group = std::find_if(groups_.begin(), groups_.end(), [&](const Group& g) {
            return g.entry_point == entry.entry_point && g.geometry == geometry;
        });
        if (group == groups_.end()) {
            groups_.push_back({entry.entry_point, geometry, entry.code_words, entry.desc_count});
        } else {
            group->code_words = std::max(group->code_words, entry.code_words);
            group->desc_count = std::max(group->desc_count, entry.desc_count);
        }
    }
}

PicaNativeFunction NativePrograms::Hashed(const pica::ShaderSetup& unit, u32 entry_point,
                                          bool geometry) const {
    for (const auto& entry : entries_) {
        if (entry.entry_point == entry_point && (entry.geometry != 0) == geometry &&
            entry.code_hash == pica_native_hash(unit.program_code.data(), entry.code_words) &&
            entry.desc_hash == pica_native_hash(unit.swizzle_data.data(), entry.desc_count)) {
            return entry.function;
        }
    }
    return nullptr;
}

PicaNativeFunction NativePrograms::Find(const pica::ShaderSetup& unit, u32 entry_point, bool geometry) {
    if (Empty() || !unit.program_serial || !unit.swizzle_serial) {
        return nullptr;
    }
    const auto known = std::find_if(known_.begin(), known_.end(), [&](const Known& k) {
        return k.program_serial == unit.program_serial && k.swizzle_serial == unit.swizzle_serial &&
               k.entry_point == entry_point && k.geometry == geometry;
    });
    if (known != known_.end()) {
        return known->function;
    }

    PicaNativeFunction function = nullptr;
    static const bool by_words = CacheByWords();
    const auto group = std::find_if(groups_.begin(), groups_.end(), [&](const Group& g) {
        return g.entry_point == entry_point && g.geometry == geometry;
    });
    if (group != groups_.end()) {
        if (by_words) {
            const auto seen = std::find_if(seen_.begin(), seen_.end(), [&](const Seen& s) {
                return s.entry_point == entry_point && s.geometry == geometry &&
                       std::equal(s.code.begin(), s.code.end(), unit.program_code.begin()) &&
                       std::equal(s.desc.begin(), s.desc.end(), unit.swizzle_data.begin());
            });
            if (seen != seen_.end()) {
                function = seen->function;
            } else {
                function = Hashed(unit, entry_point, geometry);
                Seen match{entry_point, geometry,
                           {unit.program_code.begin(), unit.program_code.begin() + group->code_words},
                           {unit.swizzle_data.begin(), unit.swizzle_data.begin() + group->desc_count},
                           function};
                constexpr size_t kWordCacheSize = 64;
                if (seen_.size() < kWordCacheSize) {
                    seen_.push_back(std::move(match));
                } else {
                    seen_[seen_next_] = std::move(match);
                    seen_next_ = (seen_next_ + 1) % kWordCacheSize;
                }
            }
        } else {
            function = Hashed(unit, entry_point, geometry);
        }
    }

    constexpr size_t kSerialCacheSize = 64;
    if (known_.size() == kSerialCacheSize) {
        known_.clear();
    }
    known_.push_back({unit.program_serial, unit.swizzle_serial, entry_point, geometry, function});
    return function;
}

} // namespace mh4n::gpu::cpuvertex
