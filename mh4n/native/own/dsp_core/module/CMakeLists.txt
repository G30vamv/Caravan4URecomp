# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
# Our own code, written from: WP40/WP42 and the independent CMake module contract.

mh4n_module(SOURCES ../src/dsp_native.cpp)
target_include_directories(mh4n_x_${MH4N_EXTRA_CURRENT} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
