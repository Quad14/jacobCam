// SPDX-License-Identifier: GPL-2.0-or-later
//
// Sensor abstraction. The STV06xx bridge is a dumb pipe; everything about
// image geometry, exposure and gain lives in the sensor sitting on its I2C
// bus. P/N 861037-0000 carries an Agilent HDCS-1000.

#ifndef QCAM_SENSOR_H_
#define QCAM_SENSOR_H_

#include <memory>

#include "qcam/bridge.h"
#include "qcam/types.h"

namespace qcam {

class ISensor {
public:
    virtual ~ISensor() = default;

    virtual const char* Name() const = 0;

    // Reads the sensor's identity register. Returns Ok only when this driver
    // is the right one for the part that answered.
    virtual Status Probe(Stv06xxBridge& bridge) = 0;

    // Bridge init table + sensor reset + sensor init table + window setup.
    virtual Status Init(Stv06xxBridge& bridge) = 0;

    virtual Status Start(Stv06xxBridge& bridge) = 0;
    virtual Status Stop(Stv06xxBridge& bridge) = 0;

    virtual Status SetExposure(Stv06xxBridge& bridge, int value) = 0;
    virtual Status SetGain(Stv06xxBridge& bridge, int value) = 0;

    virtual FrameGeometry Geometry() const = 0;

    // Isochronous wMaxPacketSize to request. The camera is a full-speed
    // device, so this also fixes the achievable frame rate.
    virtual uint16_t PreferredPacketSize() const = 0;
    virtual uint16_t MinimumPacketSize() const = 0;

    // Nominal frames per second at the preferred packet size. Used to seed
    // Media Foundation's frame-rate descriptor.
    virtual double NominalFps() const = 0;

    // Dumps the sensor register file through the log, for diagnostics.
    virtual Status DumpRegisters(Stv06xxBridge& bridge) = 0;
};

// Walks the supported sensors in probe order and returns the one that
// answers. Returns Status::NoDevice when nothing on the bus is recognised.
Status ProbeSensor(Stv06xxBridge& bridge, std::unique_ptr<ISensor>* out);

// Factory for the HDCS-1000/1100 and the pin-compatible HDCS-1020.
std::unique_ptr<ISensor> MakeHdcsSensor();

}  // namespace qcam

#endif  // QCAM_SENSOR_H_
