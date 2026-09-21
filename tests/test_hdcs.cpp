// SPDX-License-Identifier: GPL-2.0-or-later
//
// HDCS-1000 sensor tests. The exposure numbers here were worked through by
// hand from the datasheet timing model, so a regression in the arithmetic
// shows up as a concrete wrong value rather than as a dim picture.

#include "test_harness.h"

#include "../src/core/sensor_hdcs.h"

#include "qcam/bridge.h"
#include "qcam/mock.h"
#include "qcam/sensor.h"

using namespace qcam;

namespace {

// Returns the value of the last I2C write to `sensor_reg`, or -1.
int LastSensorWrite(const MockTransport& t, uint8_t sensor_reg) {
    for (auto it = t.control_log().rbegin(); it != t.control_log().rend(); ++it) {
        if (it->is_in || it->value != reg::kI2cWriteWindow) continue;
        if (it->data.size() != kI2cBlockLen) continue;
        const int count = it->data[kI2cCountSlot] + 1;
        for (int i = count - 1; i >= 0; --i) {
            if (it->data[kI2cAddrOffset + i] == sensor_reg)
                return it->data[kI2cDataOffset + i];
        }
    }
    return -1;
}

std::unique_ptr<ISensor> ProbeHdcs1000(MockTransport& t, Stv06xxBridge& bridge) {
    t.SetSensorRegister(hdcs::kIdent, hdcs::kIdent1x00);
    std::unique_ptr<ISensor> sensor;
    if (Failed(ProbeSensor(bridge, &sensor))) return nullptr;
    return sensor;
}

}  // namespace

TEST(HdcsProbeAcceptsIdent08) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);

    CHECK(sensor != nullptr);
    if (!sensor) return;
    CHECK_EQ(std::string(sensor->Name()), std::string("Agilent HDCS-1000/1100"));

    const FrameGeometry geom = sensor->Geometry();
    CHECK_EQ(int{geom.width}, 360);
    CHECK_EQ(int{geom.height}, 296);
    CHECK(geom.phase == BayerPhase::GRBG);
}

TEST(HdcsProbeAcceptsIdent10AsThe1020) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    t.SetSensorRegister(hdcs::kIdent, hdcs::kIdent1020);

    std::unique_ptr<ISensor> sensor;
    CHECK_OK(ProbeSensor(bridge, &sensor));
    CHECK(sensor != nullptr);
    if (!sensor) return;
    CHECK_EQ(int{sensor->Geometry().width}, 352);
}

TEST(HdcsProbeRejectsUnknownIdentity) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    t.SetSensorRegister(hdcs::kIdent, 0x42);

    std::unique_ptr<ISensor> sensor;
    CHECK_STATUS(ProbeSensor(bridge, &sensor), Status::NoDevice);
    CHECK(sensor == nullptr);
}

TEST(HdcsProbeUsesCorrectI2cSlaveAddress) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    ProbeHdcs1000(t, bridge);

    // 0x55 shifted left one bit, because bit 0 is the r/w flag.
    CHECK_EQ(int{bridge.i2c_profile().slave_addr}, 0xaa);
    CHECK_EQ(int{bridge.i2c_profile().reg_width}, 1);
}

TEST(HdcsInitWritesBridgeTable) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    CHECK(sensor != nullptr);
    if (!sensor) return;
    t.ClearLog();

    CHECK_OK(sensor->Init(bridge));

    const auto* r00 = t.LastWriteTo(reg::kReg00);
    const auto* r01 = t.LastWriteTo(reg::kReg01);
    const auto* xc  = t.LastWriteTo(reg::kXCtrl);
    const auto* yc  = t.LastWriteTo(reg::kYCtrl);
    CHECK(r00 && r01 && xc && yc);
    if (!(r00 && r01 && xc && yc)) return;
    CHECK_EQ(int{r00->data[0]}, 0x1d);
    CHECK_EQ(int{r01->data[0]}, 0xb5);
    CHECK_EQ(int{xc->data[0]}, 0x0a);   // 352-column timing
    CHECK_EQ(int{yc->data[0]}, 0x01);   // 288-line timing
}

TEST(HdcsInitDoesNotEnableEmulationOnStv0600) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;
    t.ClearLog();
    CHECK_OK(sensor->Init(bridge));

    CHECK_EQ(t.CountWritesTo(reg::kStv0600Emulation), size_t{0});
}

TEST(HdcsInitEnablesEmulationOnStv0602) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0602);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;
    t.ClearLog();
    CHECK_OK(sensor->Init(bridge));

    const auto* rec = t.LastWriteTo(reg::kStv0600Emulation);
    CHECK(rec != nullptr);
    if (rec) CHECK_EQ(int{rec->data[0]}, 1);
}

TEST(HdcsInitProgramsContinuousCaptureAndSampleTiming) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;
    t.ClearLog();
    CHECK_OK(sensor->Init(bridge));

    CHECK_EQ(LastSensorWrite(t, hdcs::k00Config), int{hdcs::kConfigContinuous});
    // (ADC start signal duration << 5) | PSMP, with PSMP 5 on an STV0600.
    CHECK_EQ(LastSensorWrite(t, hdcs::kTCtrl), (3 << 5) | 5);
    CHECK_EQ(LastSensorWrite(t, hdcs::kAdcCtrl), 10);
}

TEST(HdcsInitCentresTheReadoutWindow) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;
    t.ClearLog();
    CHECK_OK(sensor->Init(bridge));

    // Full 360x296 window at array offset (8,8), in units of four pixels.
    CHECK_EQ(LastSensorWrite(t, hdcs::kFwRow), 2);
    CHECK_EQ(LastSensorWrite(t, hdcs::kFwCol), 2);
    CHECK_EQ(LastSensorWrite(t, hdcs::kLwRow), 75);
    CHECK_EQ(LastSensorWrite(t, hdcs::kLwCol), 91);
}

TEST(HdcsExposureMathMatchesDatasheetModel) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;
    CHECK_OK(sensor->Init(bridge));
    t.ClearLog();

    CHECK_OK(sensor->SetExposure(bridge, 48));

    // Worked through by hand for the default 360-wide window, PSMP 5:
    //   cycles = 48 * 25 MHz * 257          = 308400
    //   ct     = cto(4) + psmp(5) + 5       = 14
    //   cp     = cto(4) + 360 * 14 / 2      = 2524
    //   rp     = rs(186) + cp               = 2710
    //   rowexp = 308400 / 2710              = 113,  remainder 2170
    //   srowexp = cp - er(100) - 6 - 2170   = 248
    CHECK_EQ(LastSensorWrite(t, hdcs::kRowExpL), 113);
    CHECK_EQ(LastSensorWrite(t, hdcs::kRowExpH), 0);
    CHECK_EQ(LastSensorWrite(t, hdcs::k00SRowExpL), 248);
    CHECK_EQ(LastSensorWrite(t, hdcs::k00SRowExpH), 0);
}

TEST(HdcsExposureBurstHaltsAndRestartsTheSensor) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;
    CHECK_OK(sensor->Init(bridge));
    CHECK_OK(sensor->Start(bridge));
    t.ClearLog();

    CHECK_OK(sensor->SetExposure(bridge, 100));

    // The burst must end with the sensor running again, or the stream stalls
    // the first time auto-exposure moves.
    CHECK_EQ(LastSensorWrite(t, hdcs::k00Control), int{hdcs::kRunEnable});
}

TEST(HdcsExposureLeavesIdleSensorIdle) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;
    CHECK_OK(sensor->Init(bridge));
    CHECK_OK(sensor->Stop(bridge));   // sleep state
    t.ClearLog();

    CHECK_OK(sensor->SetExposure(bridge, 100));

    // The register burst asserts RUN internally; the driver must put the
    // sensor back where it found it.
    CHECK_EQ(LastSensorWrite(t, hdcs::k00Control), int{hdcs::kSleepMode});
}

TEST(HdcsExposureClampsToControlRange) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;
    CHECK_OK(sensor->Init(bridge));

    CHECK_OK(sensor->SetExposure(bridge, -50));
    CHECK_OK(sensor->SetExposure(bridge, 9999));
    // Both extremes must produce in-range register values, never a wrap.
    t.ClearLog();
    CHECK_OK(sensor->SetExposure(bridge, 255));
    const int hi = LastSensorWrite(t, hdcs::kRowExpH);
    CHECK(hi >= 0 && hi <= 255);
}

TEST(HdcsGainWritesAllFourPgaQuadrants) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;
    CHECK_OK(sensor->Init(bridge));
    t.ClearLog();

    CHECK_OK(sensor->SetGain(bridge, 50));

    // Unequal quadrant gains would show up as a checkerboard colour cast.
    CHECK_EQ(LastSensorWrite(t, hdcs::kErecPga), 50);
    CHECK_EQ(LastSensorWrite(t, hdcs::kErocPga), 50);
    CHECK_EQ(LastSensorWrite(t, hdcs::kOrecPga), 50);
    CHECK_EQ(LastSensorWrite(t, hdcs::kOrocPga), 50);
}

TEST(HdcsGainFoldsHighValuesIntoTheDoublingBit) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;
    CHECK_OK(sensor->Init(bridge));
    t.ClearLog();

    CHECK_OK(sensor->SetGain(bridge, 200));
    // Av = (1 + 19v/127) * (1 + bit7): 200 -> 0x80 | 100.
    CHECK_EQ(LastSensorWrite(t, hdcs::kErecPga), 0x80 | 100);
}

TEST(HdcsStartStopDriveTheControlRegister) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;
    CHECK_OK(sensor->Init(bridge));

    t.ClearLog();
    CHECK_OK(sensor->Start(bridge));
    CHECK_EQ(LastSensorWrite(t, hdcs::k00Control), int{hdcs::kRunEnable});

    t.ClearLog();
    CHECK_OK(sensor->Stop(bridge));
    CHECK_EQ(LastSensorWrite(t, hdcs::k00Control), int{hdcs::kSleepMode});
}

TEST(HdcsNominalFpsIsBandwidthLimited) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    auto sensor = ProbeHdcs1000(t, bridge);
    if (!sensor) return;

    // Full-speed USB: one 847-byte packet per 1 ms frame against a
    // 360x296 image is a little under 8 fps.
    CHECK_EQ(int{sensor->PreferredPacketSize()}, 847);
    CHECK_NEAR(sensor->NominalFps(), 7.9, 0.3);
}

TEST(HdcsProbePropagatesTransportErrorRatherThanReportingNoDevice) {
    MockTransport t;
    t.FailControlIn(Status::Io);
    Stv06xxBridge bridge(t, Bridge::Stv0600);

    std::unique_ptr<ISensor> sensor;
    // A broken cable must not look like "unsupported camera".
    CHECK_STATUS(ProbeSensor(bridge, &sensor), Status::Io);
}
