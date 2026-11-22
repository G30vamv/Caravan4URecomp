// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: drift_controller.h and audio_out_test.cpp simulated clock results.

#include "drift_controller.h"

#include <algorithm>
#include <cmath>

namespace mh4n::audio_out {

double DriftController::Update(double fill_frames, double target_frames, double dt_seconds) {
    if (!(target_frames > 0.0 && dt_seconds > 0.0) ||
        !std::isfinite(target_frames) || !std::isfinite(dt_seconds) || !std::isfinite(fill_frames)) {
        return factor_;
    }

    const double error = 0.5 * ((fill_frames - target_frames) / target_frames);
    double correction = 1.0 + (error * dt_seconds) / kTweakTimeScale;
    if (correction < 1.0) {
        correction = correction * correction * correction;
    }
    const double desired = std::clamp(correction, kMinFactor, kMaxFactor);
    const double weight = -std::expm1(-dt_seconds / kLowPassTimeScale);
    factor_ += weight * (desired - factor_);
    return factor_;
}

}  // namespace mh4n::audio_out
