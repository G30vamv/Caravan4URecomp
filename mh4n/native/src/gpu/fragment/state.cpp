// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: state.h, the RHI ring contract and gpu_fragment_key_test and gpu_compose_test.

#include "state.h"

#include <algorithm>
#include <cstring>

#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {
namespace {

constexpr auto kFragmentKeyMask = [] {
    decltype(pica::DirtyRegs::qwords) mask{};
    for (u32 reg = 0; reg < pica::kNumRegs; ++reg) {
        if (IsFragmentKeyRegister(reg)) {
            mask[reg / 64] |= 1ull << (reg % 64);
        }
    }
    return mask;
}();

bool KeyRegistersDirty(const pica::DirtyRegs& dirty) {
    for (size_t i = 0; i < kFragmentKeyMask.size(); ++i) {
        if ((dirty.qwords[i] & kFragmentKeyMask[i]) != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

FragmentState::FragmentState(const DeviceProfile& profile)
    : FragmentState(profile, FeatureRegistry::Global()) {}

FragmentState::FragmentState(const DeviceProfile& profile, const FeatureRegistry& features)
    : profile_(profile), features_(features) {}

rhi::Result<FragmentState::Prepared> FragmentState::Prepare(const pica::PicaState& state,
                                                           const UniformContext& context, TableRings* rings) {
    Prepared result{};
    UniformDirty dirty{.all = first_};

    // Try the fallible operation before changing the key, so a retry can report the same key change.
    if (rings != nullptr) {
        const auto upload = UploadTables(state, *rings, block_, dirty, features_);
        if (!upload) {
            return rhi::Error{upload.error()};
        }
        result.tables_uploaded = upload->uploaded;
    }
    if (first_ || KeyRegistersDirty(state.dirty)) {
        const FragmentKey key = MakeKey(state, features_);
        ++key_extractions_;
        result.key_changed = StoreKey(key, block_);
        if (result.key_changed || first_) {
            needs_ = NeedsOf(key, profile_, features_);
        }
    }
    FillUniforms(state, block_, dirty, context, features_);
    result.block_changed = dirty.changed || result.key_changed;
    first_ = false;
    ++draws_;
    return result;
}

rhi::Result<std::unique_ptr<FragmentUniformRing>> FragmentUniformRing::Create(rhi::Backend& backend, u64 ring_bytes) {
    auto ring = backend.CreateRing(ring_bytes, rhi::BufferUsage::Uniform, "fragment uniforms");
    if (!ring) {
        return rhi::Error{ring.error()};
    }
    auto result = std::unique_ptr<FragmentUniformRing>(new FragmentUniformRing);
    result->ring_ = std::move(*ring);
    result->alignment_ = std::max(kFragmentBlockAlignment, backend.Caps().uniform_buffer_offset_alignment);
    return result;
}

FragmentUniformRing::~FragmentUniformRing() = default;

rhi::Result<FragmentUniformRing::Allocation> FragmentUniformRing::Write(const FragmentBlock& block, bool changed) {
    if (have_last_ && !changed) {
        ++reuses_;
        auto reused = last_;
        reused.reused = true;
        return reused;
    }

    const u64 reserved = (kFragmentBlockBytes + alignment_ - 1) / alignment_ * alignment_;
    const auto mapped = ring_->Map(reserved, alignment_);
    if (!mapped) {
        return rhi::Error{mapped.error()};
    }
    std::memcpy(mapped->data.data(), &block, kFragmentBlockBytes);
    if (const auto committed = ring_->Commit(reserved); !committed) {
        return rhi::Error{committed.error()};
    }

    last_ = {ring_->GetBuffer().get(), mapped->offset, kFragmentBlockBytes, false};
    have_last_ = true;
    ++allocations_;
    return last_;
}

} // namespace mh4n::gpu::fragment
