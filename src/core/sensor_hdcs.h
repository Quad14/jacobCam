// SPDX-License-Identifier: GPL-2.0-or-later
//
// Agilent/HP HDCS-1000, HDCS-1100 and HDCS-1020 register map.
//
// Bit 0 of every HDCS register address is the read/write flag, so the
// architectural register index is shifted left by one to form the address the
// bridge puts on the wire. All constants below are already shifted.

#ifndef QCAM_SENSOR_HDCS_H_
#define QCAM_SENSOR_HDCS_H_

#include <cstddef>
#include <cstdint>

namespace qcam {
namespace hdcs {

constexpr uint8_t Reg(uint8_t index) { return static_cast<uint8_t>(index << 1); }

// Common to every HDCS part.
constexpr uint8_t kIdent    = Reg(0x00);  // identification
constexpr uint8_t kStatus   = Reg(0x01);
constexpr uint8_t kIMask    = Reg(0x02);  // interrupt mask
constexpr uint8_t kPCtrl    = Reg(0x03);  // pad control
constexpr uint8_t kPDrv     = Reg(0x04);  // pad drive control
constexpr uint8_t kICtrl    = Reg(0x05);  // interface control
constexpr uint8_t kITmg     = Reg(0x06);  // interface timing
constexpr uint8_t kBFrac    = Reg(0x07);  // baud fraction
constexpr uint8_t kBRate    = Reg(0x08);  // baud rate
constexpr uint8_t kAdcCtrl  = Reg(0x09);  // ADC control
constexpr uint8_t kFwRow    = Reg(0x0a);  // first window row
constexpr uint8_t kFwCol    = Reg(0x0b);  // first window column
constexpr uint8_t kLwRow    = Reg(0x0c);  // last window row
constexpr uint8_t kLwCol    = Reg(0x0d);  // last window column
constexpr uint8_t kTCtrl    = Reg(0x0e);  // timing control
constexpr uint8_t kErecPga  = Reg(0x0f);  // PGA gain, even row / even column
constexpr uint8_t kErocPga  = Reg(0x10);  // even row / odd column
constexpr uint8_t kOrecPga  = Reg(0x11);  // odd row / even column
constexpr uint8_t kOrocPga  = Reg(0x12);  // odd row / odd column
constexpr uint8_t kRowExpL  = Reg(0x13);
constexpr uint8_t kRowExpH  = Reg(0x14);

// HDCS-1000 / HDCS-1100 only.
constexpr uint8_t k00SRowExpL = Reg(0x15);
constexpr uint8_t k00SRowExpH = Reg(0x16);
constexpr uint8_t k00Config   = Reg(0x17);
constexpr uint8_t k00Control  = Reg(0x18);

// HDCS-1020 only.
constexpr uint8_t k20SRowExp  = Reg(0x15);
constexpr uint8_t k20Error    = Reg(0x16);
constexpr uint8_t k20ITmg2    = Reg(0x17);
constexpr uint8_t k20ICtrl2   = Reg(0x18);
constexpr uint8_t k20HBlank   = Reg(0x19);
constexpr uint8_t k20VBlank   = Reg(0x1a);
constexpr uint8_t k20Config   = Reg(0x1b);
constexpr uint8_t k20Control  = Reg(0x1c);

// Control register bits.
constexpr uint8_t kRunEnable = 1u << 2;
constexpr uint8_t kSleepMode = 1u << 1;
constexpr uint8_t kSoftReset = 1u << 0;

// Config register bit 3 enables continuous capture; with it clear the sensor
// stops at the end of each frame.
constexpr uint8_t kConfigContinuous = 1u << 3;

// Identification register values.
constexpr uint16_t kIdent1x00 = 0x08;
constexpr uint16_t kIdent1020 = 0x10;

// Native array geometry.
constexpr uint16_t k1x00Width  = 360;
constexpr uint16_t k1x00Height = 296;
constexpr uint16_t k1020Width  = 352;
constexpr uint16_t k1020Height = 292;
constexpr uint16_t k1020BottomSkip = 4;

// Pixel clock, and the fixed ADC start-signal duration used in the exposure
// equations from the datasheet.
constexpr int kClockMHz          = 25;
constexpr int kAdcStartSignalDur = 3;

// Bridge register init table applied before touching the sensor. The 0x15xx
// values configure the video formatter for the HDCS pixel timing.
extern const uint16_t kBridgeInit[][2];
extern const size_t   kBridgeInitCount;

// Sensor register init table.
struct InitPair { uint8_t reg; uint8_t val; };
extern const InitPair kSensorInit[];
extern const size_t   kSensorInitCount;

}  // namespace hdcs
}  // namespace qcam

#endif  // QCAM_SENSOR_HDCS_H_
