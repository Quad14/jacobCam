// SPDX-License-Identifier: GPL-2.0-or-later
#include "qcam/autoexp.h"

#include <algorithm>

#include "qcam/decode.h"
#include "qcam/log.h"

namespace qcam {

int MeasureBayerLuma(const uint8_t* bayer, const FrameGeometry& geom) {
    if (!bayer || geom.width == 0 || geom.height == 0) return 0;

    // Green sites carry most of the luminance and there are twice as many of
    // them, so metering on green alone is both cheaper and less noisy than
    // demosaicing first.
    //
    // Metering is centre-weighted: the outer eighth of the frame is ignored so
    // a bright window behind the subject does not drive the whole picture dark.
    const int mx = geom.width / 8;
    const int my = geom.height / 8;
    const int x0 = mx, x1 = geom.width - mx;
    const int y0 = my, y1 = geom.height - my;

    uint64_t sum = 0;
    uint64_t count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (BayerColorAt(geom.phase, x, y) != 1) continue;
            sum += bayer[static_cast<size_t>(y) * geom.width + x];
            count++;
        }
    }
    if (!count) return 0;
    return static_cast<int>(sum / count);
}

AutoExposure::AutoExposure(const AutoExposureConfig& cfg) : cfg_(cfg) {
    Reset();
}

void AutoExposure::Reset(int exposure, int gain) {
    state_.exposure = std::clamp(exposure, cfg_.exposure_min, cfg_.exposure_max);
    state_.gain     = std::clamp(gain, cfg_.gain_min, cfg_.gain_max);
    state_.converged = false;
    state_.measured_luma = 0;
    settle_ = 0;
}

void AutoExposure::SetManual(int exposure, int gain) {
    cfg_.enabled = false;
    state_.exposure = std::clamp(exposure, kCtrlMin, kCtrlMax);
    state_.gain     = std::clamp(gain, kCtrlMin, kCtrlMax);
    state_.converged = true;
}

void AutoExposure::SetEnabled(bool on) {
    if (cfg_.enabled == on) return;
    cfg_.enabled = on;
    if (on) {
        state_.converged = false;
        settle_ = 0;
    }
}

bool AutoExposure::Update(const uint8_t* bayer, const FrameGeometry& geom) {
    return UpdateWithLuma(MeasureBayerLuma(bayer, geom));
}

bool AutoExposure::UpdateWithLuma(int luma) {
    state_.measured_luma = luma;
    if (!cfg_.enabled) return false;

    // A write to the exposure registers only shows up a frame or two later;
    // acting on stale frames makes the loop oscillate.
    if (settle_ > 0) { settle_--; return false; }

    const int error = cfg_.target_luma - luma;
    if (std::abs(error) <= cfg_.tolerance) {
        state_.converged = true;
        return false;
    }
    state_.converged = false;

    const int old_exposure = state_.exposure;
    const int old_gain     = state_.gain;

    // Proportional step, sized relative to the current operating point so the
    // loop behaves the same at both ends of the range.
    const float ratio = (luma > 0)
        ? static_cast<float>(cfg_.target_luma) / static_cast<float>(luma)
        : cfg_.max_step;
    // Clamped symmetrically in the multiplicative domain, so correcting a
    // two-stop error takes the same number of frames in either direction.
    const float max_step = (cfg_.max_step > 1.0f) ? cfg_.max_step : 2.0f;
    const float damped = std::clamp(1.0f + (ratio - 1.0f) * cfg_.gain_kp,
                                    1.0f / max_step, max_step);

    if (error > 0) {
        // Too dark. Spend exposure time first: it is free of noise, unlike
        // the analogue gain stage.
        if (state_.exposure < cfg_.exposure_max) {
            int next = static_cast<int>(state_.exposure * damped + 0.5f);
            if (next == state_.exposure) next++;
            state_.exposure = std::min(next, cfg_.exposure_max);
        } else if (state_.gain < cfg_.gain_max) {
            int next = static_cast<int>(state_.gain * damped + 0.5f);
            if (next == state_.gain) next++;
            state_.gain = std::min(next, cfg_.gain_max);
        }
    } else {
        // Too bright. Unwind gain before exposure, for the same reason.
        if (state_.gain > cfg_.gain_min) {
            int next = static_cast<int>(state_.gain * damped + 0.5f);
            if (next == state_.gain) next--;
            state_.gain = std::max(next, cfg_.gain_min);
        } else if (state_.exposure > cfg_.exposure_min) {
            int next = static_cast<int>(state_.exposure * damped + 0.5f);
            if (next == state_.exposure) next--;
            state_.exposure = std::max(next, cfg_.exposure_min);
        }
    }

    const bool changed = (state_.exposure != old_exposure) || (state_.gain != old_gain);
    if (changed) {
        settle_ = cfg_.settle_frames;
        QCAM_LOGT("AE: luma %d -> exposure %d, gain %d", luma, state_.exposure,
                  state_.gain);
    } else {
        // Already at a rail and still off target; stop reporting churn.
        state_.converged = true;
    }
    return changed;
}

}  // namespace qcam
