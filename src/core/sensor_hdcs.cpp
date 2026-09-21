// SPDX-License-Identifier: GPL-2.0-or-later
//
// HDCS-1000/1100/1020 sensor driver.
//
// The exposure model follows the HDCS-1000 datasheet section 3.4.5.5 (and
// 3.5.6.4 for the 1020): total integration time is expressed as a whole
// number of row periods plus a sub-row remainder, both of which have to be
// derived from the sensor's own pixel timing.

#include "sensor_hdcs.h"

#include <algorithm>
#include <memory>

#include "qcam/log.h"
#include "qcam/sensor.h"

namespace qcam {
namespace hdcs {

const uint16_t kBridgeInit[][2] = {
    {reg::kIsoEnable, 0x00},
    {reg::kReg23,     0x00},
    {reg::kReg00,     0x1d},
    {reg::kReg01,     0xb5},
    {reg::kReg02,     0xa8},
    {reg::kReg03,     0x95},
    {reg::kReg04,     0x07},
    {reg::kScanRate,  0x20},
    {reg::kYCtrl,     0x01},  // 288-line (CIF) vertical timing
    {reg::kXCtrl,     0x0a},  // 352-column horizontal timing
};
const size_t kBridgeInitCount = sizeof(kBridgeInit) / sizeof(kBridgeInit[0]);

const InitPair kSensorInit[] = {
    // Writing a 1 clears the corresponding status bit.
    {kStatus,  0x7e},
    {kIMask,   0x00},  // no interrupts; we poll nothing
    {kPCtrl,   0x63},  // pad control: bits 6,5,1,0
    {kPDrv,    0x00},
    {kICtrl,   0x20},
    {kITmg,    0x12},
    {kAdcCtrl, 10},    // 10-bit ADC output resolution
};
const size_t kSensorInitCount = sizeof(kSensorInit) / sizeof(kSensorInit[0]);

}  // namespace hdcs

namespace {

using namespace hdcs;

class HdcsSensor final : public ISensor {
public:
    const char* Name() const override {
        switch (variant_) {
            case Variant::Hdcs1x00: return "Agilent HDCS-1000/1100";
            case Variant::Hdcs1020: return "Agilent HDCS-1020";
            default:                return "HDCS (unprobed)";
        }
    }

    Status Probe(Stv06xxBridge& bridge) override {
        bridge.SetI2cProfile(I2cProfile{/*slave_addr=*/0x55 << 1,
                                        /*reg_width=*/1,
                                        /*flush_value=*/0});

        uint16_t ident = 0;
        Status st = bridge.ReadSensor(kIdent, &ident);
        if (Failed(st)) return st;

        if (ident == kIdent1x00) {
            ConfigureAs1x00(bridge.type());
        } else if (ident == kIdent1020) {
            ConfigureAs1020();
        } else {
            QCAM_LOGD("HDCS probe: identity register returned 0x%04x, not ours",
                      ident);
            return Status::NoDevice;
        }

        QCAM_LOGI("%s detected (ident 0x%02x)", Name(), ident);
        return Status::Ok;
    }

    Status Init(Stv06xxBridge& bridge) override {
        if (variant_ == Variant::Unknown) return Status::NoDevice;

        // The STV0602 is a superset part; put it back into STV0600 behaviour
        // so one init table covers both.
        if (bridge.type() == Bridge::Stv0602)
            QCAM_TRY(bridge.WriteReg(reg::kStv0600Emulation, 1));

        QCAM_TRY(bridge.WriteRegTable(kBridgeInit, kBridgeInitCount));

        QCAM_TRY(SoftReset(bridge));

        for (size_t i = 0; i < kSensorInitCount; ++i)
            QCAM_TRY(bridge.WriteSensor(kSensorInit[i].reg, kSensorInit[i].val));

        // Continuous capture; without this the sensor halts after one frame.
        QCAM_TRY(bridge.WriteSensor(ConfigReg(), kConfigContinuous));

        // PGA sample duration. The 1020 packs the ADC start-signal duration
        // one bit higher than the 1x00 does.
        const uint8_t tctrl =
            (variant_ == Variant::Hdcs1020)
                ? static_cast<uint8_t>((kAdcStartSignalDur << 6) | psmp_)
                : static_cast<uint8_t>((kAdcStartSignalDur << 5) | psmp_);
        QCAM_TRY(bridge.WriteSensor(kTCtrl, tctrl));

        QCAM_TRY(SetWindow(bridge, array_.width, array_.height));

        // Land on sane defaults so the first frame out of the camera is
        // viewable even before auto-exposure has converged.
        QCAM_TRY(SetExposure(bridge, kDefExposure));
        QCAM_TRY(SetGain(bridge, kDefGain));
        return Status::Ok;
    }

    Status Start(Stv06xxBridge& bridge) override {
        QCAM_LOGD("HDCS: entering run state");
        return SetState(bridge, State::Run);
    }

    Status Stop(Stv06xxBridge& bridge) override {
        QCAM_LOGD("HDCS: entering sleep state");
        return SetState(bridge, State::Sleep);
    }

    Status SetExposure(Stv06xxBridge& bridge, int value) override {
        if (variant_ == Variant::Unknown) return Status::NoDevice;
        value = std::clamp(value, kCtrlMin, kCtrlMax);

        // Requested integration time, expressed in pixel clocks. The 257
        // factor makes the 0..255 control span roughly 0..2.6 ms of exposure
        // at the default window.
        int cycles = value * kClockMHz * 257;

        // Column time period: the time to shift one column out, including the
        // programmable sample period and the fixed ADC start signal.
        const int ct = exp_.cto + psmp_ + (kAdcStartSignalDur + 2);
        // Column processing period for a whole row.
        const int cp = exp_.cto + (width_ * ct / 2);
        // Row period, including the fixed row overhead.
        const int rp = exp_.rs + cp;
        if (rp <= 0) return Status::Protocol;

        int rowexp = cycles / rp;
        cycles -= rowexp * rp;  // remainder, to be spent in the sub-row counter

        int srowexp = 0;
        int max_srowexp = 0;
        int mnct = 0;  // minimum column periods inside a column processing period
        if (variant_ == Variant::Hdcs1020) {
            srowexp = width_ - (cycles + exp_.er + 13) / ct;
            mnct = (exp_.er + 12 + ct - 1) / ct;
            max_srowexp = width_ - mnct;
        } else {
            srowexp = cp - exp_.er - 6 - cycles;
            mnct = (exp_.er + 5 + ct - 1) / ct;
            max_srowexp = cp - mnct * ct - 1;
        }
        srowexp = std::clamp(srowexp, 0, std::max(0, max_srowexp));

        const State want = state_;
        Status st;
        if (variant_ == Variant::Hdcs1020) {
            const RegPair8 seq[] = {
                {k20Control, 0x00},                                     // halt
                {kRowExpL,   static_cast<uint8_t>(rowexp & 0xff)},
                {kRowExpH,   static_cast<uint8_t>((rowexp >> 8) & 0xff)},
                {k20SRowExp, static_cast<uint8_t>((srowexp >> 2) & 0xff)},
                {k20Error,   0x10},                                     // clear exposure error
                {k20Control, kRunEnable},                               // resume
            };
            st = bridge.WriteSensorBytes(seq, sizeof(seq) / sizeof(seq[0]));
        } else {
            const RegPair8 seq[] = {
                {k00Control,   0x00},
                {kRowExpL,     static_cast<uint8_t>(rowexp & 0xff)},
                {kRowExpH,     static_cast<uint8_t>((rowexp >> 8) & 0xff)},
                {k00SRowExpL,  static_cast<uint8_t>(srowexp & 0xff)},
                {k00SRowExpH,  static_cast<uint8_t>((srowexp >> 8) & 0xff)},
                {kStatus,      0x10},
                {k00Control,   kRunEnable},
            };
            st = bridge.WriteSensorBytes(seq, sizeof(seq) / sizeof(seq[0]));
        }
        if (Failed(st)) return st;

        QCAM_LOGD("HDCS: exposure %d -> rowexp %d, srowexp %d", value, rowexp,
                  srowexp);
        exposure_ = value;

        // The burst above ends by asserting RUN so an in-flight stream picks
        // the new timing up immediately. If we were not running, undo that.
        state_ = State::Run;
        if (want != State::Run) QCAM_TRY(SetState(bridge, want));
        return Status::Ok;
    }

    Status SetGain(Stv06xxBridge& bridge, int value) override {
        if (variant_ == Variant::Unknown) return Status::NoDevice;
        value = std::clamp(value, kCtrlMin, kCtrlMax);

        // Voltage gain is Av = (1 + 19 * v / 127) * (1 + bit7), so the top
        // half of the control range folds into the 2x stage.
        uint8_t g = static_cast<uint8_t>(value);
        if (g > 127) g = static_cast<uint8_t>(0x80 | (g / 2));

        // One PGA register per Bayer quadrant; keep them equal so the gain
        // stage does not introduce a colour cast.
        const uint8_t gains[4] = {g, g, g, g};
        QCAM_TRY(bridge.WriteSensorSeq(kErecPga, gains, 4));
        gain_ = value;
        QCAM_LOGD("HDCS: gain %d -> PGA 0x%02x", value, g);
        return Status::Ok;
    }

    FrameGeometry Geometry() const override {
        // The STV0600 hands the mosaic back with a green-red leading quad.
        return FrameGeometry{width_, height_, BayerPhase::GRBG};
    }

    uint16_t PreferredPacketSize() const override { return 847; }
    uint16_t MinimumPacketSize() const override { return 847; }

    double NominalFps() const override {
        // The camera is full speed, so one packet leaves per 1 ms frame and
        // throughput alone fixes the rate. Four bytes per packet go to the
        // chunk header.
        const double payload = static_cast<double>(PreferredPacketSize()) - 4.0;
        const double frame_bytes = static_cast<double>(width_) * height_;
        if (frame_bytes <= 0.0) return 0.0;
        return payload * 1000.0 / frame_bytes;
    }

    Status DumpRegisters(Stv06xxBridge& bridge) override {
        QCAM_LOGI("HDCS sensor register dump:");
        // Step by two: odd addresses only flip the read/write flag bit.
        for (uint8_t r = kIdent; r <= kRowExpH; r = static_cast<uint8_t>(r + 2)) {
            uint16_t val = 0;
            Status st = bridge.ReadSensor(r, &val);
            if (Failed(st)) {
                QCAM_LOGW("  reg 0x%02x: read failed (%s)", r, StatusName(st));
                return st;
            }
            QCAM_LOGI("  reg 0x%02x = 0x%02x", r, val);
        }
        return Status::Ok;
    }

private:
    enum class Variant : uint8_t { Unknown, Hdcs1x00, Hdcs1020 };
    enum class State : uint8_t { Sleep, Idle, Run };

    struct Array {
        int left = 0, top = 0;
        int width = 0, height = 0;
        int border = 0;
    };

    struct ExposureTiming {
        int cto = 0;  // column timing overhead
        int cpo = 0;  // column processing overhead
        int rs  = 0;  // row sample period constant
        int er  = 0;  // exposure reset duration
    };

    uint8_t ConfigReg() const {
        return variant_ == Variant::Hdcs1020 ? k20Config : k00Config;
    }
    uint8_t ControlReg() const {
        return variant_ == Variant::Hdcs1020 ? k20Control : k00Control;
    }

    void ConfigureAs1x00(Bridge bridge_type) {
        variant_ = Variant::Hdcs1x00;
        array_ = Array{8, 8, k1x00Width, k1x00Height, 4};
        exp_   = ExposureTiming{4, 2, 186, 100};
        // Pixel sample period. The frame rate falls as this rises; 5 is the
        // lowest value that works on an STV0600, 20 on an STV0602.
        psmp_  = (bridge_type == Bridge::Stv0602) ? 20 : 5;
        width_  = k1x00Width;
        height_ = k1x00Height;
    }

    void ConfigureAs1020() {
        variant_ = Variant::Hdcs1020;
        // The 1020's visible area sits inset from the array origin.
        array_ = Array{24, 4, k1020Width, 304, 4};
        exp_   = ExposureTiming{3, 3, 155, 96};
        psmp_  = 6;
        width_  = k1020Width;
        height_ = k1020Height;
    }

    Status SoftReset(Stv06xxBridge& bridge) {
        QCAM_TRY(bridge.WriteSensor(ControlReg(), kSoftReset));
        Status st = bridge.WriteSensor(ControlReg(), 0);
        state_ = State::Idle;
        return st;
    }

    Status SetState(Stv06xxBridge& bridge, State state) {
        if (state_ == state) return Status::Ok;

        // Every transition goes through idle.
        if (state_ != State::Idle) {
            QCAM_TRY(bridge.WriteSensor(ControlReg(), 0));
            state_ = State::Idle;
        }
        if (state == State::Idle) return Status::Ok;

        const uint8_t val = (state == State::Sleep) ? kSleepMode : kRunEnable;
        QCAM_TRY(bridge.WriteSensor(ControlReg(), val));
        state_ = state;
        return Status::Ok;
    }

    // Programs the readout window, centred in the pixel array. The window
    // registers count in units of four pixels.
    Status SetWindow(Stv06xxBridge& bridge, int width, int height) {
        width  = (width + 3) & ~3;
        height = (height + 3) & ~3;
        width = std::min(width, array_.width);

        int y;
        if (variant_ == Variant::Hdcs1020) {
            // The bottom rows of the 1020 are not light-sensitive.
            const int usable = array_.height - 2 * array_.border - k1020BottomSkip;
            height = std::min(height, usable);
            y = (array_.height - k1020BottomSkip - height) / 2 + array_.top;
        } else {
            height = std::min(height, array_.height);
            y = array_.top + (array_.height - height) / 2;
        }
        const int x = array_.left + (array_.width - width) / 2;

        const uint8_t win[4] = {
            static_cast<uint8_t>(y / 4),
            static_cast<uint8_t>(x / 4),
            static_cast<uint8_t>((y + height) / 4 - 1),
            static_cast<uint8_t>((x + width) / 4 - 1),
        };
        QCAM_TRY(bridge.WriteSensorSeq(kFwRow, win, 4));

        width_  = static_cast<uint16_t>(width);
        height_ = static_cast<uint16_t>(height);
        QCAM_LOGD("HDCS: window %dx%d at (%d,%d)", width, height, x, y);
        return Status::Ok;
    }

    Variant        variant_ = Variant::Unknown;
    State          state_   = State::Idle;
    Array          array_;
    ExposureTiming exp_;
    int            psmp_    = 5;
    uint16_t       width_   = k1x00Width;
    uint16_t       height_  = k1x00Height;
    int            exposure_ = kDefExposure;
    int            gain_     = kDefGain;
};

}  // namespace

std::unique_ptr<ISensor> MakeHdcsSensor() {
    return std::unique_ptr<ISensor>(new HdcsSensor());
}

Status ProbeSensor(Stv06xxBridge& bridge, std::unique_ptr<ISensor>* out) {
    if (!out) return Status::InvalidArg;

    // Only the HDCS family is implemented today. The loop is here because the
    // STV0600 bridge is also shipped with Photobit PB-0100 and ST VV6410
    // sensors; adding one means adding a factory to this list.
    std::unique_ptr<ISensor> (*factories[])() = {
        &MakeHdcsSensor,
    };

    for (auto factory : factories) {
        std::unique_ptr<ISensor> sensor = factory();
        Status st = sensor->Probe(bridge);
        if (Succeeded(st)) {
            *out = std::move(sensor);
            return Status::Ok;
        }
        if (st != Status::NoDevice) {
            // A transport failure is not "wrong sensor"; stop and report it.
            return st;
        }
    }

    QCAM_LOGE("no supported sensor answered on the bridge I2C bus");
    return Status::NoDevice;
}

}  // namespace qcam
