// SPDX-License-Identifier: GPL-2.0-or-later
//
// Auto exposure / auto gain.
//
// The HDCS-1000 has no metering hardware whatsoever: if nobody drives the
// exposure registers the picture is whatever the last write left behind.
// This is a plain integrating controller over frame luma, with the split
// between exposure time and analogue gain biased toward exposure (gain on
// this sensor is visibly noisy above ~120).

#ifndef QCAM_AUTOEXP_H_
#define QCAM_AUTOEXP_H_

#include <cstdint>

#include "qcam/types.h"

namespace qcam {

struct AutoExposureConfig {
    int   target_luma   = 118;   // 0..255, slightly below mid to protect highlights
    int   tolerance     = 6;     // deadband, in luma counts
    float gain_kp       = 0.35f; // proportional step toward the target
    // Largest multiplicative step per frame. Without this the loop slams from
    // a black frame straight to both rails and then crawls back down, because
    // the raw ratio is unbounded upward but bounded below by 1.
    float max_step      = 2.0f;
    int   exposure_min  = 4;
    int   exposure_max  = 255;
    int   gain_min      = 8;
    int   gain_max      = 120;   // above this the sensor is mostly noise
    int   settle_frames = 2;     // frames to skip after a write takes effect
    bool  enabled       = true;
};

struct AutoExposureState {
    int  exposure = kDefExposure;
    int  gain     = kDefGain;
    int  measured_luma = 0;
    bool converged = false;
};

class AutoExposure {
public:
    explicit AutoExposure(const AutoExposureConfig& cfg = {});

    void SetConfig(const AutoExposureConfig& cfg) { cfg_ = cfg; }
    const AutoExposureConfig& config() const { return cfg_; }

    void Reset(int exposure = kDefExposure, int gain = kDefGain);

    // Measures the mean luma of a Bayer frame (sampling the green sites, which
    // carry most of the luminance) and updates the control state.
    //
    // Returns true when exposure or gain changed and the caller should push
    // the new values to the sensor.
    bool Update(const uint8_t* bayer, const FrameGeometry& geom);

    // Same, but from a caller-supplied luma measurement.
    bool UpdateWithLuma(int luma);

    const AutoExposureState& state() const { return state_; }

    // Manual override: stops the controller and pins the values.
    void SetManual(int exposure, int gain);
    bool enabled() const { return cfg_.enabled; }
    void SetEnabled(bool on);

private:
    AutoExposureConfig cfg_;
    AutoExposureState  state_;
    int                settle_ = 0;
};

// Mean luma over the green CFA sites of a mosaic frame.
int MeasureBayerLuma(const uint8_t* bayer, const FrameGeometry& geom);

}  // namespace qcam

#endif  // QCAM_AUTOEXP_H_
