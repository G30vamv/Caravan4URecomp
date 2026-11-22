# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
# Our own code, written from: WP41 and the independent CMake module contract.

mh4n_module(SOURCES ../src/dsp_service.cpp DEPENDS mh4n_ipc mh4n_x_dsp_core)
target_include_directories(mh4n_x_${MH4N_EXTRA_CURRENT} PRIVATE "${MH4N_SRC_DIR}")
