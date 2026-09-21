// SPDX-License-Identifier: GPL-2.0-or-later
//
// STV06xx bridge access. See docs/protocol.md for the wire format.

#include "qcam/bridge.h"

#include <cstring>

#include "qcam/log.h"

namespace qcam {

Stv06xxBridge::Stv06xxBridge(IUsbTransport& transport, Bridge type)
    : transport_(transport), type_(type) {}

Status Stv06xxBridge::WriteReg(uint16_t addr, uint16_t value) {
    // The ASIC latches either one or two bytes depending on the register. The
    // reference implementation keys that off the magnitude of the value, which
    // works because the only 16-bit-wide register in the init paths (the
    // isochronous packet size) is always written with a value above 0xff.
    uint8_t buf[2] = {static_cast<uint8_t>(value & 0xff),
                      static_cast<uint8_t>((value >> 8) & 0xff)};
    const size_t len = (value > 0xff) ? 2u : 1u;

    Status st = transport_.ControlOut(kRequestBridge, addr, 0, buf, len);
    if (Failed(st)) {
        QCAM_LOGW("bridge write 0x%04x = 0x%04x failed: %s", addr, value,
                  StatusName(st));
    } else {
        QCAM_LOGT("bridge write 0x%04x = 0x%04x", addr, value);
    }
    return st;
}

Status Stv06xxBridge::ReadReg(uint16_t addr, uint8_t* value) {
    if (!value) return Status::InvalidArg;

    uint8_t buf = 0;
    size_t transferred = 0;
    Status st = transport_.ControlIn(kRequestBridge, addr, 0, &buf, 1, &transferred);
    if (Failed(st)) {
        QCAM_LOGW("bridge read 0x%04x failed: %s", addr, StatusName(st));
        return st;
    }
    if (transferred != 1) return Status::Protocol;

    *value = buf;
    QCAM_LOGT("bridge read 0x%04x -> 0x%02x", addr, buf);
    return Status::Ok;
}

Status Stv06xxBridge::WriteRegTable(const uint16_t (*table)[2], size_t count) {
    for (size_t i = 0; i < count; ++i)
        QCAM_TRY(WriteReg(table[i][0], table[i][1]));
    return Status::Ok;
}

// The STV0610 latches a staged I2C block only once an extra write lands on
// 0x1704. The other bridges in the family commit on the staging write itself.
Status Stv06xxBridge::WriteSensorFinish() {
    if (type_ != Bridge::Stv0610) return Status::Ok;

    const uint8_t zero = 0;
    return transport_.ControlOut(kRequestBridge, reg::kI2cFlushStv0610, 0, &zero, 1);
}

Status Stv06xxBridge::WriteSensorBytes(const RegPair8* pairs, size_t count) {
    if (!pairs && count) return Status::InvalidArg;

    size_t i = 0;
    while (i < count) {
        uint8_t block[kI2cBlockLen];
        std::memset(block, 0, sizeof(block));

        size_t n = 0;
        for (; n < kI2cMaxBytes && i < count; ++n, ++i) {
            block[kI2cAddrOffset + n] = pairs[i].reg;
            block[kI2cDataOffset + n] = pairs[i].val;
            QCAM_LOGT("i2c write reg 0x%02x = 0x%02x", pairs[i].reg, pairs[i].val);
        }

        block[kI2cSlaveSlot] = i2c_.slave_addr;
        block[kI2cCountSlot] = static_cast<uint8_t>(n - 1);
        block[kI2cCmdSlot]   = kI2cWriteCmd;

        QCAM_TRY(transport_.ControlOut(kRequestBridge, reg::kI2cWriteWindow, 0,
                                       block, kI2cBlockLen));
    }
    return WriteSensorFinish();
}

Status Stv06xxBridge::WriteSensorWords(const RegPair16* pairs, size_t count) {
    if (!pairs && count) return Status::InvalidArg;

    size_t i = 0;
    while (i < count) {
        uint8_t block[kI2cBlockLen];
        std::memset(block, 0, sizeof(block));

        size_t n = 0;
        for (; n < kI2cMaxWords && i < count; ++n, ++i) {
            block[kI2cAddrOffset + n]         = static_cast<uint8_t>(pairs[i].reg);
            block[kI2cDataOffset + n * 2]     = static_cast<uint8_t>(pairs[i].val & 0xff);
            block[kI2cDataOffset + n * 2 + 1] = static_cast<uint8_t>(pairs[i].val >> 8);
        }

        block[kI2cSlaveSlot] = i2c_.slave_addr;
        block[kI2cCountSlot] = static_cast<uint8_t>(n - 1);
        block[kI2cCmdSlot]   = kI2cWriteCmd;

        QCAM_TRY(transport_.ControlOut(kRequestBridge, reg::kI2cWriteWindow, 0,
                                       block, kI2cBlockLen));
    }
    return WriteSensorFinish();
}

Status Stv06xxBridge::WriteSensor(uint8_t reg_addr, uint16_t value) {
    if (i2c_.reg_width == 2) {
        RegPair16 pair{reg_addr, value};
        return WriteSensorWords(&pair, 1);
    }
    RegPair8 pair{reg_addr, static_cast<uint8_t>(value & 0xff)};
    return WriteSensorBytes(&pair, 1);
}

Status Stv06xxBridge::ReadSensor(uint8_t reg_addr, uint16_t* value) {
    if (!value) return Status::InvalidArg;

    QCAM_TRY(WriteReg(reg::kI2cFlush, i2c_.flush_value));

    // Stage a read command: address only, no data, count 0, read opcode. Note
    // that reads stage into 0x1400 while writes stage into 0x0400.
    uint8_t block[kI2cBlockLen];
    std::memset(block, 0, sizeof(block));
    block[kI2cAddrOffset] = reg_addr;
    block[kI2cSlaveSlot]  = i2c_.slave_addr;
    block[kI2cCountSlot]  = 0;
    block[kI2cCmdSlot]    = kI2cReadCmd;

    Status st = transport_.ControlOut(kRequestBridge, reg::kI2cReadWindow, 0,
                                      block, kI2cBlockLen);
    if (Failed(st)) {
        QCAM_LOGW("i2c read: staging address 0x%02x failed: %s", reg_addr,
                  StatusName(st));
        return st;
    }

    // Then collect the result the ASIC clocked back.
    uint8_t result[2] = {0, 0};
    size_t transferred = 0;
    const size_t want = (i2c_.reg_width == 2) ? 2u : 1u;
    st = transport_.ControlIn(kRequestBridge, reg::kI2cReadResult, 0, result,
                              want, &transferred);
    if (Failed(st)) {
        QCAM_LOGW("i2c read: fetching reg 0x%02x failed: %s", reg_addr,
                  StatusName(st));
        return st;
    }
    if (transferred != want) return Status::Protocol;

    *value = (want == 2) ? static_cast<uint16_t>(result[0] | (result[1] << 8))
                         : result[0];
    QCAM_LOGT("i2c read reg 0x%02x -> 0x%04x", reg_addr, *value);
    return Status::Ok;
}

Status Stv06xxBridge::WriteSensorSeq(uint8_t first_reg, const uint8_t* values,
                                     size_t count) {
    if (!values || count == 0 || count >= kI2cMaxBytes) return Status::InvalidArg;
    // Bit 0 of an HDCS register address is the r/w flag, so consecutive
    // architectural registers are two apart. The last address we will touch
    // must still fit in the 8-bit register space.
    if (static_cast<size_t>(first_reg) + (count - 1) * 2 > 0xff)
        return Status::InvalidArg;

    RegPair8 pairs[kI2cMaxBytes];
    uint8_t reg_addr = first_reg;
    for (size_t i = 0; i < count; ++i) {
        pairs[i].reg = reg_addr;
        pairs[i].val = values[i];
        reg_addr = static_cast<uint8_t>(reg_addr + 2);
    }
    return WriteSensorBytes(pairs, count);
}

Status Stv06xxBridge::SetIsoPacketSize(uint16_t bytes) {
    // Writing the low register with a 16-bit value carries both halves.
    return WriteReg(reg::kIsoSizeL, bytes);
}

Status Stv06xxBridge::EnableIso(bool on) {
    return WriteReg(reg::kIsoEnable, on ? 1 : 0);
}

Status Stv06xxBridge::SetLed(bool on) {
    return WriteReg(reg::kLedCtrl, on ? 1 : 0);
}

}  // namespace qcam
