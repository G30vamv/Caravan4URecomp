// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: policy.h, swapchain_test.cpp and the Khronos Vulkan surface specification.

#include "policy.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace mh4n::swapchain {

VkPresentModeKHR ChoosePresentMode(std::span<const VkPresentModeKHR> offered, bool vsync) {
    if (!vsync) {
        for (auto mode : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::ranges::find(offered, mode) != offered.end()) {
                return mode;
            }
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

const char* PresentModeName(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    default: return "UNKNOWN";
    }
}

std::optional<VkSurfaceFormatKHR> ChooseSurfaceFormat(std::span<const VkSurfaceFormatKHR> offered) {
    if (offered.size() == 1 && offered.front().format == VK_FORMAT_UNDEFINED) {
        return VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, offered.front().colorSpace};
    }

    std::optional<VkSurfaceFormatKHR> fallback;
    for (auto format : offered) {
        if (format.format != VK_FORMAT_B8G8R8A8_UNORM && format.format != VK_FORMAT_R8G8B8A8_UNORM) {
            continue;
        }
        if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
        if (!fallback) {
            fallback = format;
        }
    }
    return fallback;
}

VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& caps, u32 requested_width, u32 requested_height) {
    constexpr u32 unspecified = std::numeric_limits<u32>::max();
    if (caps.currentExtent.width != unspecified || caps.currentExtent.height != unspecified) {
        return caps.currentExtent;
    }
    const auto clamp_dimension = [](u32 size, u32 lower, u32 upper) {
        return size == 0 ? 0u : std::clamp(size, lower, upper);
    };
    return {clamp_dimension(requested_width, caps.minImageExtent.width, caps.maxImageExtent.width),
            clamp_dimension(requested_height, caps.minImageExtent.height, caps.maxImageExtent.height)};
}

u32 ChooseImageCount(const VkSurfaceCapabilitiesKHR& caps) {
    const u32 limit = caps.maxImageCount ? caps.maxImageCount : std::numeric_limits<u32>::max();
    return caps.minImageCount < limit ? caps.minImageCount + 1 : limit;
}

VkSurfaceTransformFlagBitsKHR ChooseTransform(const VkSurfaceCapabilitiesKHR& caps) {
    return (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
               ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
               : caps.currentTransform;
}

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& caps) {
    const auto supported = caps.supportedCompositeAlpha;
    return supported ? static_cast<VkCompositeAlphaFlagBitsKHR>(1u << std::countr_zero(supported))
                     : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

DisplayTimeChoice ChooseDisplayTime(bool present_wait_measures_display, bool display_timing_available,
                                    std::string_view registered_name) {
    if (present_wait_measures_display) {
        return {DisplayTimeKind::PresentWait, {}};
    }
    if (display_timing_available) {
        return {DisplayTimeKind::DisplayTiming, {}};
    }
    if (!registered_name.empty()) {
        return {DisplayTimeKind::Registered, std::string(registered_name)};
    }
    return {};
}

std::string DisplayTimeLabel(const DisplayTimeChoice& choice, std::string_view source_name) {
    switch (choice.kind) {
    case DisplayTimeKind::PresentWait: return "present_wait";
    case DisplayTimeKind::DisplayTiming: return "display_timing";
    case DisplayTimeKind::Registered: return source_name.empty() ? choice.registered : std::string(source_name);
    case DisplayTimeKind::None: return "none";
    }
    return "none";
}

}  // namespace mh4n::swapchain
