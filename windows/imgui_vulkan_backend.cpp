// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
// khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
//
// Dear ImGui's Vulkan backend (MIT, in mh4n/port/app/third_party/imgui/backends), built without Vulkan's prototypes: its
// functions are given to it at run time (menu_gui.cpp), from the loader the process already has.
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#include "backends/imgui_impl_vulkan.cpp"
