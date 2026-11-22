// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: Runtime::Prepare, determinism_test.cpp and the runner's --help output.

#include "mh4n/options.h"

MH4N_OPTION("runner", "--deterministic", "",
            "use guest cycles for the clock and schedule service replies at fixed ticks; "
            "requires a code library built with --native --deterministic");
MH4N_OPTION("runner", "--seed", "N",
            "with --deterministic, draw reproducible slices from half to one and a half times "
            "--slice-us (4274 us by default); an empty event queue caps the stretch at the default");
