// SPDX-License-Identifier: GPL-2.0-or-later
//
// Bridge wire-format tests. These pin the exact bytes that reach the device,
// because a transposed staging window or an off-by-one count byte produces a
// camera that enumerates, accepts every write, and returns black frames.

#include "test_harness.h"

#include "qcam/bridge.h"
#include "qcam/mock.h"

using namespace qcam;

namespace {

I2cProfile HdcsProfile() {
    return I2cProfile{0x55 << 1, 1, 0};
}

}  // namespace

TEST(BridgeWriteUsesVendorRequest04) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);

    CHECK_OK(bridge.WriteReg(reg::kIsoEnable, 1));

    CHECK_EQ(t.control_log().size(), size_t{1});
    const auto& rec = t.control_log()[0];
    CHECK_EQ(rec.is_in, false);
    CHECK_EQ(int{rec.request}, int{kRequestBridge});
    CHECK_EQ(int{rec.value}, int{reg::kIsoEnable});
    CHECK_EQ(int{rec.index}, 0);
    CHECK_EQ(rec.data.size(), size_t{1});
    CHECK_EQ(int{rec.data[0]}, 1);
}

TEST(BridgeWriteWidensToTwoBytesAboveByteRange) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);

    // The isochronous packet size is the one 16-bit register in the init path.
    CHECK_OK(bridge.SetIsoPacketSize(847));

    const auto* rec = t.LastWriteTo(reg::kIsoSizeL);
    CHECK(rec != nullptr);
    if (!rec) return;
    CHECK_EQ(rec->data.size(), size_t{2});
    CHECK_EQ(int{rec->data[0]}, 847 & 0xff);
    CHECK_EQ(int{rec->data[1]}, 847 >> 8);
}

TEST(BridgeReadReturnsRegisterFile) {
    MockTransport t;
    t.SetBridgeRegister(0x1445, 0x5a);
    Stv06xxBridge bridge(t, Bridge::Stv0600);

    uint8_t value = 0;
    CHECK_OK(bridge.ReadReg(0x1445, &value));
    CHECK_EQ(int{value}, 0x5a);
}

TEST(SensorWriteStagesIntoWriteWindow) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    bridge.SetI2cProfile(HdcsProfile());

    CHECK_OK(bridge.WriteSensor(0x2e, 0x08));

    CHECK_EQ(t.control_log().size(), size_t{1});
    const auto& rec = t.control_log()[0];
    // Writes stage into 0x0400; reads stage into 0x1400. Swapping these is
    // the classic way to get a silent no-op.
    CHECK_EQ(int{rec.value}, int{reg::kI2cWriteWindow});
    CHECK_EQ(rec.data.size(), kI2cBlockLen);
    CHECK_EQ(int{rec.data[kI2cAddrOffset]}, 0x2e);
    CHECK_EQ(int{rec.data[kI2cDataOffset]}, 0x08);
    CHECK_EQ(int{rec.data[kI2cSlaveSlot]}, 0x55 << 1);
    CHECK_EQ(int{rec.data[kI2cCountSlot]}, 0);   // one entry => count-1 == 0
    CHECK_EQ(int{rec.data[kI2cCmdSlot]}, int{kI2cWriteCmd});
}

TEST(SensorBurstSplitsAtSixteenEntries) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    bridge.SetI2cProfile(HdcsProfile());

    RegPair8 pairs[20];
    for (int i = 0; i < 20; ++i)
        pairs[i] = RegPair8{static_cast<uint8_t>(i * 2), static_cast<uint8_t>(i)};

    CHECK_OK(bridge.WriteSensorBytes(pairs, 20));

    // 20 entries => one full block of 16 plus a block of 4.
    CHECK_EQ(t.control_log().size(), size_t{2});
    CHECK_EQ(int{t.control_log()[0].data[kI2cCountSlot]}, 15);
    CHECK_EQ(int{t.control_log()[1].data[kI2cCountSlot]}, 3);
    CHECK_EQ(int{t.control_log()[1].data[kI2cAddrOffset]}, 16 * 2);
    CHECK_EQ(int{t.control_log()[1].data[kI2cDataOffset]}, 16);
}

TEST(SensorReadStagesThenFetchesResult) {
    MockTransport t;
    t.SetSensorRegister(0x00, 0x08);  // HDCS-1000 identity
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    bridge.SetI2cProfile(HdcsProfile());

    uint16_t value = 0;
    CHECK_OK(bridge.ReadSensor(0x00, &value));
    CHECK_EQ(int{value}, 0x08);

    // Expected traffic: flush write, staged read command, result fetch.
    CHECK_EQ(t.control_log().size(), size_t{3});
    CHECK_EQ(int{t.control_log()[0].value}, int{reg::kI2cFlush});
    CHECK_EQ(int{t.control_log()[1].value}, int{reg::kI2cReadWindow});
    CHECK_EQ(int{t.control_log()[1].data[kI2cCmdSlot]}, int{kI2cReadCmd});
    CHECK_EQ(t.control_log()[2].is_in, true);
    CHECK_EQ(int{t.control_log()[2].value}, int{reg::kI2cReadResult});
}

TEST(SensorSeqStepsAddressByTwo) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    bridge.SetI2cProfile(HdcsProfile());

    const uint8_t window[4] = {2, 2, 75, 91};
    CHECK_OK(bridge.WriteSensorSeq(0x14, window, 4));

    CHECK_EQ(t.control_log().size(), size_t{1});
    const auto& d = t.control_log()[0].data;
    // Bit 0 of an HDCS address is the r/w flag, so registers step by two.
    CHECK_EQ(int{d[kI2cAddrOffset + 0]}, 0x14);
    CHECK_EQ(int{d[kI2cAddrOffset + 1]}, 0x16);
    CHECK_EQ(int{d[kI2cAddrOffset + 2]}, 0x18);
    CHECK_EQ(int{d[kI2cAddrOffset + 3]}, 0x1a);
    CHECK_EQ(int{d[kI2cDataOffset + 0]}, 2);
    CHECK_EQ(int{d[kI2cDataOffset + 3]}, 91);
    CHECK_EQ(int{d[kI2cCountSlot]}, 3);
}

TEST(Stv0610NeedsExplicitFlushAfterBurst) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0610);
    bridge.SetI2cProfile(HdcsProfile());

    CHECK_OK(bridge.WriteSensor(0x2e, 0x08));

    // Staging write, then the 0x1704 commit the STV0610 requires.
    CHECK_EQ(t.control_log().size(), size_t{2});
    CHECK_EQ(int{t.control_log()[1].value}, int{reg::kI2cFlushStv0610});
}

TEST(Stv0600DoesNotEmitTheStv0610Flush) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    bridge.SetI2cProfile(HdcsProfile());

    CHECK_OK(bridge.WriteSensor(0x2e, 0x08));
    CHECK_EQ(t.CountWritesTo(reg::kI2cFlushStv0610), size_t{0});
}

TEST(BridgePropagatesTransportFailure) {
    MockTransport t;
    t.FailControlOut(Status::Io);
    Stv06xxBridge bridge(t, Bridge::Stv0600);

    CHECK_STATUS(bridge.WriteReg(reg::kIsoEnable, 1), Status::Io);
}

TEST(SensorSeqRejectsOutOfRangeRun) {
    MockTransport t;
    Stv06xxBridge bridge(t, Bridge::Stv0600);
    bridge.SetI2cProfile(HdcsProfile());

    const uint8_t values[4] = {1, 2, 3, 4};
    // 0xfa + 3*2 = 0x100, past the end of the 8-bit register space.
    CHECK_STATUS(bridge.WriteSensorSeq(0xfa, values, 4), Status::InvalidArg);
    CHECK_STATUS(bridge.WriteSensorSeq(0x14, values, 0), Status::InvalidArg);
}
