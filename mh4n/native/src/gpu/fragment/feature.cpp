// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: feature.h and the gpu_fragment_key_test black-box cases.

#include "feature.h"

#include <algorithm>

namespace mh4n::gpu::fragment {

FeatureRegistry& FeatureRegistry::Global() {
    static FeatureRegistry registry;
    return registry;
}

const FeatureModule* FeatureRegistry::Find(std::string_view name) const {
    const auto slot = std::ranges::find(kFeatureNames, name);
    return slot == kFeatureNames.end() ? nullptr : features_[slot - kFeatureNames.begin()];
}

bool FeatureRegistry::Add(const FeatureModule& feature) {
    const auto slot = std::ranges::find(kFeatureNames, feature.Name());
    if (slot == kFeatureNames.end()) {
        return false;
    }
    auto& entry = features_[slot - kFeatureNames.begin()];
    if (entry != nullptr) {
        return false;
    }
    entry = &feature;
    Rebuild();
    return true;
}

void FeatureRegistry::Rebuild() {
    count_ = 0;
    for (const auto* feature : features_) {
        if (feature != nullptr) {
            ordered_[count_++] = feature;
        }
    }
}

size_t StubbedFeatures(const FragmentKey& key, const FeatureRegistry& features,
                       std::span<const char*> out) {
    size_t written = 0;
    for (const auto* feature : features.All()) {
        if (written == out.size()) {
            break;
        }
        if (const char* name = feature->Stubbed(key)) {
            out[written++] = name;
        }
    }
    return written;
}

detail::FeatureRegistrar::FeatureRegistrar(const FeatureModule& feature) {
    FeatureRegistry::Global().Add(feature);
}

} // namespace mh4n::gpu::fragment
