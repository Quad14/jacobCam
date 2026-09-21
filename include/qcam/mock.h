// SPDX-License-Identifier: GPL-2.0-or-later
//
// Scriptable fake transport.
//
// Everything above IUsbTransport can be exercised on a machine with no camera
// attached: the mock records control transfers for assertions, emulates the
// bridge's I2C read window well enough for sensor probing to succeed, and
// replays canned isochronous packets into the framer.

#ifndef QCAM_MOCK_H_
#define QCAM_MOCK_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "qcam/types.h"
#include "qcam/usb.h"

namespace qcam {

class MockTransport final : public IUsbTransport {
public:
    struct ControlRecord {
        bool                 is_in;
        uint8_t              request;
        uint16_t             value;    // bridge register address
        uint16_t             index;
        std::vector<uint8_t> data;
    };

    MockTransport();

    // -- Scripting ---------------------------------------------------------
    void SetInfo(const UsbDeviceInfo& info) { info_ = info; }

    // Value returned by a bridge register read.
    void SetBridgeRegister(uint16_t addr, uint8_t value) { bridge_regs_[addr] = value; }

    // Value returned when the sensor register `reg` is read over I2C. The
    // mock watches for a staged read command and answers accordingly.
    void SetSensorRegister(uint8_t reg, uint16_t value) { sensor_regs_[reg] = value; }

    // Makes every subsequent transfer of the given direction fail.
    void FailControlOut(Status st) { fail_out_ = st; }
    void FailControlIn(Status st) { fail_in_ = st; }

    void SetIsoMaxPacketSize(uint16_t bytes) { iso_max_packet_ = bytes; }

    // -- Isochronous replay ------------------------------------------------
    void QueueIsoPacket(std::vector<uint8_t> packet) {
        iso_queue_.push_back(std::move(packet));
    }
    // Delivers every queued packet to the sink, synchronously, on the calling
    // thread. Tests stay deterministic this way.
    size_t PumpIso();

    // -- Inspection --------------------------------------------------------
    const std::vector<ControlRecord>& control_log() const { return log_; }
    void ClearLog() { log_.clear(); }
    uint8_t alt_setting() const { return alt_; }

    // Convenience: find the last write to a bridge register, or nullptr.
    const ControlRecord* LastWriteTo(uint16_t addr) const;
    size_t CountWritesTo(uint16_t addr) const;

    // -- IUsbTransport -----------------------------------------------------
    Status ControlOut(uint8_t request, uint16_t value, uint16_t index,
                      const uint8_t* data, size_t len) override;
    Status ControlIn(uint8_t request, uint16_t value, uint16_t index,
                     uint8_t* data, size_t len, size_t* transferred) override;
    Status SetAltSetting(uint8_t alt) override;
    Status GetIsoMaxPacketSize(uint8_t alt, uint16_t* max_packet) override;
    Status StartIso(IIsoSink* sink) override;
    void   StopIso() override;
    bool   IsStreaming() const override { return streaming_; }
    const UsbDeviceInfo& Info() const override { return info_; }

private:
    UsbDeviceInfo                     info_;
    std::vector<ControlRecord>        log_;
    std::map<uint16_t, uint8_t>       bridge_regs_;
    std::map<uint8_t, uint16_t>       sensor_regs_;
    std::vector<std::vector<uint8_t>> iso_queue_;
    IIsoSink*                         sink_          = nullptr;
    uint8_t                           alt_           = 0;
    uint16_t                          iso_max_packet_ = 847;
    bool                              streaming_     = false;
    Status                            fail_out_      = Status::Ok;
    Status                            fail_in_       = Status::Ok;

    // Register selected by the most recent staged I2C read command.
    uint8_t  pending_i2c_reg_   = 0;
    bool     pending_i2c_valid_ = false;
};

// Builds a well-formed isochronous packet from a list of chunks, for tests
// and for the replay tool.
struct TestChunk {
    uint16_t             id;
    std::vector<uint8_t> payload;
};
std::vector<uint8_t> BuildIsoPacket(const std::vector<TestChunk>& chunks);

// Splits `frame` into data chunks of at most `chunk_size` bytes and wraps the
// result in SOF/EOF, producing the packet sequence a real camera would emit.
std::vector<std::vector<uint8_t>> BuildFramePackets(const std::vector<uint8_t>& frame,
                                                    size_t payload_per_packet);

}  // namespace qcam

#endif  // QCAM_MOCK_H_
