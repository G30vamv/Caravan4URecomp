// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: sync.h, impl.h, plume_render_interface.h, gpu_backend_sync_test.cpp and VkPipelineCacheHeaderVersionOne.

#include "impl.h"

#include <algorithm>

#include "mh4n/log.h"

namespace mh4n::gpu::rhi {
namespace {

constexpr auto kLog = "RHI";

u32 ReadLittleEndianWord(std::span<const u8> blob, size_t offset) {
    return static_cast<u32>(blob[offset]) |
           (static_cast<u32>(blob[offset + 1]) << 8) |
           (static_cast<u32>(blob[offset + 2]) << 16) |
           (static_cast<u32>(blob[offset + 3]) << 24);
}

}  // namespace

std::string sync::CheckPipelineCacheBlob(std::span<const u8> blob, u32 vendor_id, u32 device_id,
                                        std::span<const u8, 16> uuid) {
    if (blob.size() < kPipelineCacheHeaderSize) {
        return "truncated header";
    }
    if (ReadLittleEndianWord(blob, 0) != kPipelineCacheHeaderSize) {
        return "invalid header size";
    }
    if (ReadLittleEndianWord(blob, 4) != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {
        return "unsupported header version";
    }
    if (ReadLittleEndianWord(blob, 8) != vendor_id) {
        return "vendor ID differs";
    }
    if (ReadLittleEndianWord(blob, 12) != device_id) {
        return "device ID differs";
    }
    if (!std::equal(uuid.begin(), uuid.end(), blob.begin() + 16)) {
        return "UUID differs";
    }
    return {};
}

bool Backend::LoadPipelineCache(std::span<const u8> blob) {
    Backend::Impl& backend = impl();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(backend.physical_device, &properties);
    const std::string error = sync::CheckPipelineCacheBlob(blob, properties.vendorID, properties.deviceID,
                                                         properties.pipelineCacheUUID);
    if (!error.empty()) {
        MH4N_LOG(Info, kLog, "Ignoring pipeline cache: {}", error);
        return false;
    }
    std::lock_guard lock(SyncState(backend).cache_mutex);
    if (!backend.device->loadPipelineCache(blob.data(), blob.size())) {
        MH4N_LOG(Info, kLog, "Ignoring pipeline cache: the device refused the data");
        return false;
    }
    return true;
}

std::vector<u8> Backend::SavePipelineCache() {
    Backend::Impl& backend = impl();
    std::lock_guard lock(SyncState(backend).cache_mutex);
    std::vector<u8> blob;
    if (!backend.device->savePipelineCache(blob)) {
        return {};
    }
    return blob;
}

}  // namespace mh4n::gpu::rhi
