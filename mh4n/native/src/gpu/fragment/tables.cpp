// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: tables.h, native/bindings.h, the RHI ring contract and gpu_compose_test.

#include "tables.h"

#include <array>
#include <optional>

#include "feature.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {

struct TableRings::Impl {
    struct RingState {
        std::unique_ptr<rhi::Ring> ring;
        std::optional<rhi::RingAllocation> mapping;
        u64 used = 0;
        u64 wraps = 0;
        bool reload = false;
    };

    std::array<RingState, 2> rings;

    RingState& Get(TableRing ring) { return rings[static_cast<size_t>(ring)]; }
    const RingState& Get(TableRing ring) const { return rings[static_cast<size_t>(ring)]; }
};

TableRings::TableRings() : impl_(std::make_unique<Impl>()) {}
TableRings::~TableRings() = default;

rhi::Result<std::unique_ptr<TableRings>> TableRings::Create(rhi::Backend& backend, const TableRingsDesc& desc) {
    // Bindings 3–5 expose these arrays through storage-buffer descriptors.
    auto lf = backend.CreateRing(desc.lf_bytes, rhi::BufferUsage::Storage, "lighting and fog tables");
    if (!lf) {
        return rhi::Error{lf.error()};
    }
    auto general = backend.CreateRing(desc.general_bytes, rhi::BufferUsage::Storage, "procedural texture tables");
    if (!general) {
        return rhi::Error{general.error()};
    }
    auto result = std::unique_ptr<TableRings>(new TableRings);
    result->impl_->Get(TableRing::Lf).ring = std::move(*lf);
    result->impl_->Get(TableRing::General).ring = std::move(*general);
    return result;
}

const rhi::Buffer& TableRings::Buffer(TableRing ring) const {
    return *impl_->Get(ring).ring->GetBuffer();
}

rhi::Result<bool> TableRings::Begin(TableRing ring, u64 max_bytes) {
    auto& state = impl_->Get(ring);
    if (state.mapping) {
        return rhi::Error{"table upload session already open"};
    }
    if (max_bytes == 0 || max_bytes % kTableAlignment != 0) {
        return rhi::Error{"table upload reservation must be a nonzero multiple of 16 bytes"};
    }
    auto mapped = state.ring->Map(max_bytes, kTableAlignment);
    if (!mapped) {
        return rhi::Error{mapped.error()};
    }
    if (mapped->wrapped) {
        ++state.wraps;
        state.reload = true;
    }
    state.mapping = *mapped;
    state.used = 0;
    return state.reload;
}

rhi::Result<TableSlice> TableRings::Take(TableRing ring, u64 bytes) {
    auto& state = impl_->Get(ring);
    if (!state.mapping) {
        return rhi::Error{"table upload session is closed"};
    }
    if (bytes % kTableAlignment != 0 || bytes > state.mapping->data.size() - state.used) {
        return rhi::Error{"table upload slice must be aligned and fit the reservation"};
    }
    const TableSlice slice{state.mapping->data.subspan(state.used, bytes), state.mapping->offset + state.used};
    state.used += bytes;
    return slice;
}

rhi::Result<void> TableRings::End(TableRing ring) {
    auto& state = impl_->Get(ring);
    if (!state.mapping) {
        return rhi::Error{"table upload session is closed"};
    }
    const auto committed = state.ring->Commit(state.used);
    state.mapping.reset();
    if (committed && state.used != 0) {
        state.reload = false;
    }
    return committed;
}

u64 TableRings::Wraps(TableRing ring) const {
    return impl_->Get(ring).wraps;
}

rhi::Result<UploadSummary> UploadTables(const pica::PicaState& state, TableRings& rings, FragmentBlock& block,
                                        UniformDirty& dirty) {
    return UploadTables(state, rings, block, dirty, FeatureRegistry::Global());
}

rhi::Result<UploadSummary> UploadTables(const pica::PicaState& state, TableRings& rings, FragmentBlock& block,
                                        UniformDirty& dirty, const FeatureRegistry& features) {
    TableDirty need{state.lighting.lut_dirty, state.fog.lut_dirty, state.proctex.table_dirty};
    const bool upload_lf = need.lighting != 0 || need.fog;
    const bool upload_general = need.proctex != 0;
    UploadSummary summary{.uploaded = upload_lf || upload_general};
    if (!summary.uploaded) {
        return summary;
    }

    if (upload_lf) {
        const auto started = rings.Begin(TableRing::Lf, kLfRingMaxUpload);
        if (!started) {
            return rhi::Error{started.error()};
        }
        summary.lf_wrapped = *started;
        if (summary.lf_wrapped) {
            need.lighting = pica::Lighting::kAllDirty;
            need.fog = true;
        }
    }
    if (upload_general) {
        const auto started = rings.Begin(TableRing::General, kGeneralRingMaxUpload);
        if (!started) {
            if (upload_lf) {
                rings.End(TableRing::Lf);
            }
            return rhi::Error{started.error()};
        }
        summary.general_wrapped = *started;
        if (summary.general_wrapped) {
            need.proctex = pica::ProcTex::kAllDirty;
        }
    }

    // Publish the new offsets only after the ring reservations have been committed.
    FragmentBlock updated = block;
    UniformDirty updated_dirty = dirty;
    TableUpload upload{state, rings, updated, updated_dirty, need};
    for (const auto* feature : features.All()) {
        feature->UploadTables(upload);
    }

    const auto lf_commit = upload_lf ? rings.End(TableRing::Lf) : rhi::Result<void>{};
    const auto general_commit = upload_general ? rings.End(TableRing::General) : rhi::Result<void>{};
    if (!lf_commit) {
        return rhi::Error{lf_commit.error()};
    }
    if (!general_commit) {
        return rhi::Error{general_commit.error()};
    }
    block = updated;
    dirty = updated_dirty;
    return summary;
}

} // namespace mh4n::gpu::fragment
