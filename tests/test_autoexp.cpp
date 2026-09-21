// SPDX-License-Identifier: GPL-2.0-or-later
//
// Auto-exposure controller tests. The loop drives real hardware registers, so
// the properties that matter are convergence, stability at the rails, and not
// oscillating while a write is still in flight.

#include "test_harness.h"

#include <vector>

#include "qcam/autoexp.h"
#include "qcam/decode.h"

using namespace qcam;

namespace {

FrameGeometry Geom(uint16_t w = 32, uint16_t h = 32) {
    return FrameGeometry{w, h, BayerPhase::GRBG};
}

// Simulates a scene whose measured luma responds linearly to exposure*gain,
// so the controller can be run in closed loop.
int SimulateLuma(int exposure, int gain, double scene_gain) {
    const double v = exposure * (gain / 50.0) * scene_gain;
    return static_cast<int>(v > 255 ? 255 : (v < 0 ? 0 : v));
}

}  // namespace

TEST(MeasureLumaUsesGreenSites) {
    const auto g = Geom();
    std::vector<uint8_t> mosaic(g.RawSize());
    for (int y = 0; y < g.height; ++y) {
        for (int x = 0; x < g.width; ++x) {
            // Green sites at 200, everything else at 0.
            mosaic[static_cast<size_t>(y) * g.width + x] =
                (BayerColorAt(g.phase, x, y) == 1) ? 200 : 0;
        }
    }
    CHECK_EQ(MeasureBayerLuma(mosaic.data(), g), 200);
}

TEST(MeasureLumaIsCentreWeighted) {
    const auto g = Geom(64, 64);
    std::vector<uint8_t> mosaic(g.RawSize(), 0);
    // Bright border only; the centre is black.
    for (int y = 0; y < g.height; ++y)
        for (int x = 0; x < g.width; ++x)
            if (x < 4 || y < 4 || x >= 60 || y >= 60)
                mosaic[static_cast<size_t>(y) * g.width + x] = 255;

    // A bright rim must not drag the metering; the subject is in the middle.
    CHECK_EQ(MeasureBayerLuma(mosaic.data(), g), 0);
}

TEST(AutoExposureConvergesOnADarkScene) {
    AutoExposureConfig cfg;
    cfg.settle_frames = 0;
    AutoExposure ae(cfg);
    ae.Reset(20, 20);

    const double scene = 1.4;
    for (int i = 0; i < 200; ++i) {
        const int luma = SimulateLuma(ae.state().exposure, ae.state().gain, scene);
        ae.UpdateWithLuma(luma);
    }

    const int final_luma =
        SimulateLuma(ae.state().exposure, ae.state().gain, scene);
    CHECK_NEAR(final_luma, cfg.target_luma, cfg.tolerance + 2);
    CHECK(ae.state().converged);
}

TEST(AutoExposureConvergesOnABrightScene) {
    AutoExposureConfig cfg;
    cfg.settle_frames = 0;
    AutoExposure ae(cfg);
    ae.Reset(250, 110);

    const double scene = 3.0;
    for (int i = 0; i < 200; ++i)
        ae.UpdateWithLuma(SimulateLuma(ae.state().exposure, ae.state().gain, scene));

    const int final_luma =
        SimulateLuma(ae.state().exposure, ae.state().gain, scene);
    CHECK_NEAR(final_luma, cfg.target_luma, cfg.tolerance + 2);
}

TEST(AutoExposureStepIsBoundedAndSymmetric) {
    AutoExposureConfig cfg;
    cfg.settle_frames = 0;
    AutoExposure ae(cfg);

    // A single pitch-black frame must not slam the controller to the rail;
    // recovery from an overshoot is much slower than the overshoot itself.
    ae.Reset(40, 50);
    ae.UpdateWithLuma(1);
    CHECK(ae.state().exposure <= static_cast<int>(40 * cfg.max_step) + 1);

    // And the same error in the other direction moves by the same factor.
    ae.Reset(40, 50);
    ae.UpdateWithLuma(255);
    CHECK(ae.state().gain >= static_cast<int>(50 / cfg.max_step) - 1);
}

TEST(AutoExposurePrefersExposureOverGain) {
    AutoExposureConfig cfg;
    cfg.settle_frames = 0;
    AutoExposure ae(cfg);
    ae.Reset(30, 50);

    // Far too dark. The invariant is that gain stays put for as long as there
    // is exposure headroom left, because analogue gain on this sensor is
    // visibly noisy; only once exposure is railed may gain move.
    bool exposure_grew = false;
    for (int i = 0; i < 20; ++i) {
        const int gain_before = ae.state().gain;
        const bool railed = ae.state().exposure >= cfg.exposure_max;
        ae.UpdateWithLuma(10);
        if (!railed) {
            CHECK_EQ(ae.state().gain, gain_before);
            if (ae.state().exposure > 30) exposure_grew = true;
        }
    }
    CHECK(exposure_grew);
    CHECK_EQ(ae.state().exposure, cfg.exposure_max);
}

TEST(AutoExposureReachesForGainOnlyAtTheExposureRail) {
    AutoExposureConfig cfg;
    cfg.settle_frames = 0;
    AutoExposure ae(cfg);
    ae.Reset(cfg.exposure_max, 50);

    for (int i = 0; i < 5; ++i) ae.UpdateWithLuma(10);

    CHECK_EQ(ae.state().exposure, cfg.exposure_max);
    CHECK(ae.state().gain > 50);
}

TEST(AutoExposureUnwindsGainBeforeExposure) {
    AutoExposureConfig cfg;
    cfg.settle_frames = 0;
    AutoExposure ae(cfg);
    ae.Reset(200, 100);

    for (int i = 0; i < 5; ++i) ae.UpdateWithLuma(250);

    CHECK(ae.state().gain < 100);
    CHECK_EQ(ae.state().exposure, 200);
}

TEST(AutoExposureRespectsTheDeadband) {
    AutoExposureConfig cfg;
    cfg.settle_frames = 0;
    AutoExposure ae(cfg);
    ae.Reset(100, 50);

    // Inside the tolerance band nothing should move, or the picture pumps.
    CHECK_EQ(ae.UpdateWithLuma(cfg.target_luma), false);
    CHECK_EQ(ae.UpdateWithLuma(cfg.target_luma + cfg.tolerance), false);
    CHECK_EQ(ae.UpdateWithLuma(cfg.target_luma - cfg.tolerance), false);
    CHECK_EQ(ae.state().exposure, 100);
    CHECK(ae.state().converged);
}

TEST(AutoExposureWaitsForWritesToTakeEffect) {
    AutoExposureConfig cfg;
    cfg.settle_frames = 2;
    AutoExposure ae(cfg);
    ae.Reset(100, 50);

    CHECK_EQ(ae.UpdateWithLuma(10), true);   // acts
    CHECK_EQ(ae.UpdateWithLuma(10), false);  // stale frame
    CHECK_EQ(ae.UpdateWithLuma(10), false);  // stale frame
    CHECK_EQ(ae.UpdateWithLuma(10), true);   // acts again
}

TEST(AutoExposureStopsReportingChangeAtTheRails) {
    AutoExposureConfig cfg;
    cfg.settle_frames = 0;
    AutoExposure ae(cfg);
    ae.Reset(cfg.exposure_max, cfg.gain_max);

    // Pinned at both rails and still too dark: report converged rather than
    // rewriting the same registers on every frame forever.
    for (int i = 0; i < 10; ++i) ae.UpdateWithLuma(5);
    CHECK_EQ(ae.UpdateWithLuma(5), false);
    CHECK_EQ(ae.state().exposure, cfg.exposure_max);
    CHECK_EQ(ae.state().gain, cfg.gain_max);
}

TEST(AutoExposureNeverExceedsConfiguredLimits) {
    AutoExposureConfig cfg;
    cfg.settle_frames = 0;
    cfg.gain_max = 90;
    cfg.exposure_max = 200;
    AutoExposure ae(cfg);
    ae.Reset(10, 10);

    for (int i = 0; i < 500; ++i) ae.UpdateWithLuma(0);
    CHECK(ae.state().exposure <= 200);
    CHECK(ae.state().gain <= 90);

    for (int i = 0; i < 500; ++i) ae.UpdateWithLuma(255);
    CHECK(ae.state().exposure >= cfg.exposure_min);
    CHECK(ae.state().gain >= cfg.gain_min);
}

TEST(ManualOverrideDisablesTheLoop) {
    AutoExposure ae;
    ae.SetManual(77, 33);

    CHECK_EQ(ae.enabled(), false);
    CHECK_EQ(ae.state().exposure, 77);
    CHECK_EQ(ae.state().gain, 33);
    CHECK_EQ(ae.UpdateWithLuma(0), false);
    CHECK_EQ(ae.state().exposure, 77);
}

TEST(ReenablingResumesControl) {
    AutoExposureConfig cfg;
    cfg.settle_frames = 0;
    AutoExposure ae(cfg);
    ae.SetManual(77, 33);
    ae.SetEnabled(true);

    CHECK_EQ(ae.UpdateWithLuma(10), true);
    CHECK(ae.state().exposure > 77);
}

TEST(AutoExposureStillRecordsLumaWhileDisabled) {
    AutoExposure ae;
    ae.SetManual(50, 50);
    ae.UpdateWithLuma(137);
    // Useful for the diagnostics readout even with the loop off.
    CHECK_EQ(ae.state().measured_luma, 137);
}
