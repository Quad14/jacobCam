// SPDX-License-Identifier: GPL-2.0-or-later
//
// STV0600/0602/0610 bridge register and I2C access.
//
// The bridge exposes a flat 16-bit register space through a single vendor
// control request (0x04). Sensor registers are reached indirectly by staging
// a command block into the bridge's I2C window and letting the ASIC clock it
// out. See docs/protocol.md for the wire-level description.

#ifndef QCAM_BRIDGE_H_
#define QCAM_BRIDGE_H_

#include "qcam/types.h"
#include "qcam/usb.h"

namespace qcam {

// -- Bridge register addresses ----------------------------------------------
// The 0x14xx block is the ASIC control page; 0x15xx is the video formatter.
namespace reg {

constexpr uint16_t kI2cPartner        = 0x1420;
constexpr uint16_t kI2cRegValPairsM1  = 0x1421;
constexpr uint16_t kI2cReadWriteTgl   = 0x1422;
constexpr uint16_t kI2cFlush          = 0x1423;
constexpr uint16_t kI2cSuccReadRegVal = 0x1424;

constexpr uint16_t kIsoEnable         = 0x1440;
constexpr uint16_t kScanRate          = 0x1443;
constexpr uint16_t kLedCtrl           = 0x1445;
constexpr uint16_t kStv0600Emulation  = 0x1446;

constexpr uint16_t kReg00             = 0x1500;
constexpr uint16_t kReg01             = 0x1501;
constexpr uint16_t kReg02             = 0x1502;
constexpr uint16_t kReg03             = 0x1503;
constexpr uint16_t kReg04             = 0x1504;

constexpr uint16_t kIsoSizeL          = 0x15c1;
constexpr uint16_t kIsoSizeH          = 0x15c2;

// 1 => 288 lines (CIF), 2 => 144 lines (QCIF)
constexpr uint16_t kYCtrl             = 0x15c3;
// 0x0a => 352 columns, 0x06 => 176 columns
constexpr uint16_t kXCtrl             = 0x1680;

constexpr uint16_t kReset             = 0x1620;
constexpr uint16_t kReg23             = 0x0423;

// Windows into which I2C command blocks are staged.
constexpr uint16_t kI2cWriteWindow    = 0x0400;
constexpr uint16_t kI2cReadWindow     = 0x1400;
constexpr uint16_t kI2cReadResult     = 0x1410;
// STV0610 needs an explicit flush write after an I2C burst.
constexpr uint16_t kI2cFlushStv0610   = 0x1704;

// Scan range used by the register dump diagnostic.
constexpr uint16_t kDumpFirst         = 0x1400;
constexpr uint16_t kDumpLast          = 0x160f;

}  // namespace reg

// The staged I2C command block is a fixed 0x23 bytes:
//   [0x00..0x0f] register addresses      (up to 16 byte-wide entries)
//   [0x10..0x1f] register values         (or 8 word-wide entries)
//   [0x20]       I2C slave address
//   [0x21]       entry count minus one
//   [0x22]       command: 1 = write, 3 = read
constexpr size_t  kI2cBlockLen   = 0x23;
constexpr size_t  kI2cAddrOffset = 0x00;
constexpr size_t  kI2cDataOffset = 0x10;
constexpr size_t  kI2cSlaveSlot  = 0x20;
constexpr size_t  kI2cCountSlot  = 0x21;
constexpr size_t  kI2cCmdSlot    = 0x22;
constexpr size_t  kI2cMaxBytes   = 16;
constexpr size_t  kI2cMaxWords   = 8;
constexpr uint8_t kI2cWriteCmd   = 1;
constexpr uint8_t kI2cReadCmd    = 3;

constexpr unsigned kControlTimeoutMs = 5000;

// A sensor register/value pair, byte-wide.
struct RegPair8 {
    uint8_t reg;
    uint8_t val;
};

// Word-wide, for sensors with 16-bit registers (PB0100).
struct RegPair16 {
    uint16_t reg;
    uint16_t val;
};

// How a particular sensor is addressed over the bridge's I2C master.
struct I2cProfile {
    uint8_t slave_addr;  // already shifted left one bit where applicable
    uint8_t reg_width;   // 1 or 2 bytes per value
    uint8_t flush_value; // written to kI2cFlush before a read
};

class Stv06xxBridge {
public:
    Stv06xxBridge(IUsbTransport& transport, Bridge type);

    Bridge type() const { return type_; }
    IUsbTransport& transport() { return transport_; }

    // -- Bridge registers --------------------------------------------------
    // Values above 0xff are sent as two bytes, matching the ASIC's behaviour
    // for the 16-bit registers (notably kIsoSizeL).
    Status WriteReg(uint16_t addr, uint16_t value);
    Status ReadReg(uint16_t addr, uint8_t* value);

    // Applies a table of {address, value} pairs, stopping at the first error.
    Status WriteRegTable(const uint16_t (*table)[2], size_t count);

    // -- Sensor registers over I2C ----------------------------------------
    void SetI2cProfile(const I2cProfile& profile) { i2c_ = profile; }
    const I2cProfile& i2c_profile() const { return i2c_; }

    Status WriteSensor(uint8_t reg, uint16_t value);
    Status ReadSensor(uint8_t reg, uint16_t* value);

    // Burst writes. The bridge accepts up to 16 byte-wide (8 word-wide)
    // register writes per staged block; longer runs are split automatically.
    Status WriteSensorBytes(const RegPair8* pairs, size_t count);
    Status WriteSensorWords(const RegPair16* pairs, size_t count);

    // Writes `count` consecutive registers starting at `first_reg`, advancing
    // the address by two per entry (the HDCS register map keeps the r/w flag
    // in bit 0, so architectural registers are two apart).
    Status WriteSensorSeq(uint8_t first_reg, const uint8_t* values, size_t count);

    // -- Streaming control -------------------------------------------------
    Status SetIsoPacketSize(uint16_t bytes);
    Status EnableIso(bool on);
    Status SetLed(bool on);

private:
    Status WriteSensorFinish();

    IUsbTransport& transport_;
    Bridge         type_;
    I2cProfile     i2c_{0, 1, 0};
};

}  // namespace qcam

#endif  // QCAM_BRIDGE_H_
