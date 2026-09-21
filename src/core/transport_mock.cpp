// SPDX-License-Identifier: GPL-2.0-or-later
#include "qcam/mock.h"

#include <algorithm>
#include <cstring>

#include "qcam/bridge.h"
#include "qcam/framer.h"
#include "qcam/log.h"

namespace qcam {

MockTransport::MockTransport() {
    info_.vid = kVendorLogitech;
    info_.pid = 0x0840;
    info_.device_path = "mock://qcam";
    info_.friendly_name = "Mock QuickCam Express";
    info_.bus_speed = 2;
}

Status MockTransport::ControlOut(uint8_t request, uint16_t value, uint16_t index,
                                 const uint8_t* data, size_t len) {
    if (Failed(fail_out_)) return fail_out_;

    ControlRecord rec;
    rec.is_in   = false;
    rec.request = request;
    rec.value   = value;
    rec.index   = index;
    rec.data.assign(data, data + len);
    log_.push_back(rec);

    // Single-byte writes land in the emulated bridge register file so a
    // later read sees what was written.
    if (len >= 1 && value != reg::kI2cWriteWindow && value != reg::kI2cReadWindow)
        bridge_regs_[value] = data[0];

    // A staged I2C read command selects the sensor register that the next
    // read of kI2cReadResult will answer with.
    if (value == reg::kI2cReadWindow && len == kI2cBlockLen &&
        data[kI2cCmdSlot] == kI2cReadCmd) {
        pending_i2c_reg_   = data[kI2cAddrOffset];
        pending_i2c_valid_ = true;
    }

    return Status::Ok;
}

Status MockTransport::ControlIn(uint8_t request, uint16_t value, uint16_t index,
                                uint8_t* data, size_t len, size_t* transferred) {
    if (Failed(fail_in_)) return fail_in_;
    if (!data || len == 0) return Status::InvalidArg;

    std::memset(data, 0, len);

    if (value == reg::kI2cReadResult && pending_i2c_valid_) {
        const auto it = sensor_regs_.find(pending_i2c_reg_);
        const uint16_t v = (it != sensor_regs_.end()) ? it->second : 0xffff;
        data[0] = static_cast<uint8_t>(v & 0xff);
        if (len >= 2) data[1] = static_cast<uint8_t>(v >> 8);
    } else {
        const auto it = bridge_regs_.find(value);
        data[0] = (it != bridge_regs_.end()) ? it->second : 0x00;
    }

    ControlRecord rec;
    rec.is_in   = true;
    rec.request = request;
    rec.value   = value;
    rec.index   = index;
    rec.data.assign(data, data + len);
    log_.push_back(rec);

    if (transferred) *transferred = len;
    return Status::Ok;
}

Status MockTransport::SetAltSetting(uint8_t alt) {
    alt_ = alt;
    return Status::Ok;
}

Status MockTransport::GetIsoMaxPacketSize(uint8_t alt, uint16_t* max_packet) {
    if (!max_packet) return Status::InvalidArg;
    *max_packet = (alt == kAltStreaming) ? iso_max_packet_ : 0;
    return Status::Ok;
}

Status MockTransport::StartIso(IIsoSink* sink) {
    if (!sink) return Status::InvalidArg;
    sink_ = sink;
    streaming_ = true;
    return Status::Ok;
}

void MockTransport::StopIso() {
    streaming_ = false;
    sink_ = nullptr;
}

size_t MockTransport::PumpIso() {
    if (!sink_) return 0;
    size_t n = 0;
    for (const auto& packet : iso_queue_) {
        sink_->OnIsoPacket(packet.data(), packet.size());
        n++;
    }
    iso_queue_.clear();
    return n;
}

const MockTransport::ControlRecord* MockTransport::LastWriteTo(uint16_t addr) const {
    for (auto it = log_.rbegin(); it != log_.rend(); ++it) {
        if (!it->is_in && it->value == addr) return &(*it);
    }
    return nullptr;
}

size_t MockTransport::CountWritesTo(uint16_t addr) const {
    size_t n = 0;
    for (const auto& rec : log_)
        if (!rec.is_in && rec.value == addr) n++;
    return n;
}

// --- Packet construction helpers ------------------------------------------

std::vector<uint8_t> BuildIsoPacket(const std::vector<TestChunk>& chunks) {
    std::vector<uint8_t> out;
    for (const auto& c : chunks) {
        out.push_back(static_cast<uint8_t>(c.id >> 8));
        out.push_back(static_cast<uint8_t>(c.id & 0xff));
        out.push_back(static_cast<uint8_t>(c.payload.size() >> 8));
        out.push_back(static_cast<uint8_t>(c.payload.size() & 0xff));
        out.insert(out.end(), c.payload.begin(), c.payload.end());
    }
    return out;
}

std::vector<std::vector<uint8_t>> BuildFramePackets(const std::vector<uint8_t>& frame,
                                                    size_t payload_per_packet) {
    std::vector<std::vector<uint8_t>> packets;
    if (payload_per_packet == 0) return packets;

    packets.push_back(BuildIsoPacket({{chunk::kSof0, {}}}));

    size_t offset = 0;
    while (offset < frame.size()) {
        const size_t n = std::min(payload_per_packet, frame.size() - offset);
        std::vector<uint8_t> payload(frame.begin() + offset,
                                     frame.begin() + offset + n);
        packets.push_back(BuildIsoPacket({{chunk::kData0, std::move(payload)}}));
        offset += n;
    }

    packets.push_back(BuildIsoPacket({{chunk::kEof0, {}}}));
    return packets;
}

}  // namespace qcam
