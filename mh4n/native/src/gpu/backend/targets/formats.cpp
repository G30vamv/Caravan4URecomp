// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: formats.h, rhi.h, gpu_backend_targets_test.cpp and the Vulkan formats specification.

#include "formats.h"
#include "gpu/backend/core/impl.h"

#include <initializer_list>

namespace mh4n::gpu::rhi {
namespace targets {
namespace {

struct FormatInfo {
    VkFormat vk = VK_FORMAT_UNDEFINED;
    u32 color_bytes = 0;
    bool depth = false;
};

FormatInfo Describe(Format format) {
    switch (format) {
    case Format::Undefined: return {};
    case Format::Rgb8:
    case Format::Rgba8Unorm: return {VK_FORMAT_R8G8B8A8_UNORM, 4};
    case Format::Rgb565:
    case Format::R5G6B5: return {VK_FORMAT_R5G6B5_UNORM_PACK16, 2};
    case Format::Rgb5A1:
    case Format::R5G5B5A1: return {VK_FORMAT_R5G5B5A1_UNORM_PACK16, 2};
    case Format::Rgba4:
    case Format::R4G4B4A4: return {VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2};
    case Format::R8Unorm: return {VK_FORMAT_R8_UNORM, 1};
    case Format::Rg8Unorm: return {VK_FORMAT_R8G8_UNORM, 2};
    case Format::Rgba8Snorm: return {VK_FORMAT_R8G8B8A8_SNORM, 4};
    case Format::Bgra8Unorm: return {VK_FORMAT_B8G8R8A8_UNORM, 4};
    case Format::B5G6R5: return {VK_FORMAT_B5G6R5_UNORM_PACK16, 2};
    case Format::B5G5R5A1: return {VK_FORMAT_B5G5R5A1_UNORM_PACK16, 2};
    case Format::A1R5G5B5: return {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2};
    case Format::B4G4R4A4: return {VK_FORMAT_B4G4R4A4_UNORM_PACK16, 2};
    case Format::R32Uint: return {VK_FORMAT_R32_UINT, 4};
    case Format::Rgba16Float: return {VK_FORMAT_R16G16B16A16_SFLOAT, 8};
    case Format::Bc1Unorm: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    case Format::Bc3Unorm: return {VK_FORMAT_BC3_UNORM_BLOCK};
    case Format::Bc5Unorm: return {VK_FORMAT_BC5_UNORM_BLOCK};
    case Format::Bc7Unorm: return {VK_FORMAT_BC7_UNORM_BLOCK};
    case Format::D16Unorm: return {VK_FORMAT_D16_UNORM, 0, true};
    case Format::D24S8: return {VK_FORMAT_D24_UNORM_S8_UINT, 0, true};
    case Format::D32Float: return {VK_FORMAT_D32_SFLOAT, 0, true};
    case Format::D32S8: return {VK_FORMAT_D32_SFLOAT_S8_UINT, 0, true};
    case Format::R32Float: return {VK_FORMAT_R32_SFLOAT, 4};
    case Format::Rg32Float: return {VK_FORMAT_R32G32_SFLOAT, 8};
    case Format::Rgb32Float: return {VK_FORMAT_R32G32B32_SFLOAT, 12};
    case Format::Rgba32Float: return {VK_FORMAT_R32G32B32A32_SFLOAT, 16};
    case Format::R8Uint: return {VK_FORMAT_R8_UINT, 1};
    case Format::R8Sint: return {VK_FORMAT_R8_SINT, 1};
    case Format::Rg8Uint: return {VK_FORMAT_R8G8_UINT, 2};
    case Format::Rg8Sint: return {VK_FORMAT_R8G8_SINT, 2};
    case Format::Rgba8Uint: return {VK_FORMAT_R8G8B8A8_UINT, 4};
    case Format::Rgba8Sint: return {VK_FORMAT_R8G8B8A8_SINT, 4};
    case Format::R16Uint: return {VK_FORMAT_R16_UINT, 2};
    case Format::R16Sint: return {VK_FORMAT_R16_SINT, 2};
    case Format::Rg16Sint: return {VK_FORMAT_R16G16_SINT, 4};
    case Format::Rgba16Sint: return {VK_FORMAT_R16G16B16A16_SINT, 8};
    }
    return {};
}

ChosenFormat ChoosePacked(std::initializer_list<Format> candidates, FormatUse use, const SupportFn& support) {
    for (Format candidate : candidates) {
        const FormatSupport available = support(candidate);
        if (available.sampled && (use == FormatUse::Sampled || available.attachment)) {
            return {candidate, Conversion::None};
        }
    }
    return {Format::Rgba8Unorm, Conversion::UnpackToRgba8};
}

}  // namespace

VkFormat ToVkFormat(Format format) {
    return Describe(format).vk;
}

u32 TexelBytes(Format format) {
    return Describe(format).color_bytes;
}

ChosenFormat ChooseFormatWith(Format format, FormatUse use, const Capabilities& caps, const SupportFn& support) {
    switch (format) {
    case Format::Rgb8:
        return {Format::Rgba8Unorm, Conversion::Rgb8ToRgba8};
    case Format::Rgb565:
        return ChoosePacked({Format::R5G6B5, Format::B5G6R5}, use, support);
    case Format::Rgb5A1:
        return ChoosePacked({Format::R5G5B5A1, Format::B5G5R5A1, Format::A1R5G5B5}, use, support);
    case Format::Rgba4:
        return ChoosePacked({Format::R4G4B4A4, Format::B4G4R4A4}, use, support);
    case Format::D24S8:
        if (!caps.d24s8) {
            return {Format::D32S8, Conversion::DepthStencilTo32};
        }
        break;
    case Format::Bc1Unorm:
    case Format::Bc3Unorm:
    case Format::Bc5Unorm:
    case Format::Bc7Unorm:
        if (!caps.bc7 || !support(format).sampled) {
            return {Format::Rgba8Unorm, Conversion::DecodeBc};
        }
        break;
    default:
        break;
    }
    return {format, Conversion::None};
}

}  // namespace targets

FormatSupport Backend::QueryFormat(Format format) const {
    const auto info = targets::Describe(format);
    if (info.vk == VK_FORMAT_UNDEFINED) {
        return {};
    }
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(impl().physical_device, info.vk, &properties);
    const VkFormatFeatureFlags features = properties.optimalTilingFeatures;
    const auto attachment = info.depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                       : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    return {
        .attachment = (features & attachment) != 0,
        .sampled = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0,
        .storage = (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0,
        .blit_source = (features & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0,
        .blit_destination = (features & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0,
    };
}

ChosenFormat Backend::ChooseFormat(Format format, FormatUse use) const {
    return targets::ChooseFormatWith(format, use, Caps(), [this](Format candidate) {
        return QueryFormat(candidate);
    });
}

}  // namespace mh4n::gpu::rhi
