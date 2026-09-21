// SPDX-License-Identifier: GPL-2.0-or-later
//
// The USB transport seam.
//
// Everything above this interface is portable C++ and is unit-tested on a
// host machine with no camera attached. Below it sit two implementations:
//
//   WinUsbTransport   - the real one, talking to WinUSB.sys (src/win)
//   MockTransport     - a scriptable fake, and a capture replayer (src/core)

#ifndef QCAM_USB_H_
#define QCAM_USB_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "qcam/types.h"

namespace qcam {

// USB standard request types, as they appear on the wire in bmRequestType.
constexpr uint8_t kVendorOut = 0x40;  // host -> device, vendor, device target
constexpr uint8_t kVendorIn  = 0xc0;  // device -> host, vendor, device target

// The single vendor request the STV06xx bridge implements. wValue carries the
// bridge register address; the payload carries the data.
constexpr uint8_t kRequestBridge = 0x04;

// The camera's isochronous IN endpoint, and the alternate setting that gives
// it a non-zero bandwidth allocation.
constexpr uint8_t kIsoEndpoint  = 0x81;
constexpr uint8_t kAltIdle      = 0;
constexpr uint8_t kAltStreaming = 1;

// Receives isochronous payload as it arrives. Called on the transport's
// streaming thread; implementations must not block for long.
class IIsoSink {
public:
    virtual ~IIsoSink() = default;

    // One isochronous packet. `len` may be 0 for an empty microframe, which
    // the camera emits whenever it has no data ready; those are normal.
    virtual void OnIsoPacket(const uint8_t* data, size_t len) = 0;

    // Streaming stopped abnormally (unplug, bandwidth loss, ...).
    virtual void OnIsoError(Status status) { (void)status; }
};

struct UsbDeviceInfo {
    uint16_t    vid = 0;
    uint16_t    pid = 0;
    std::string device_path;    // \\?\usb#vid_046d&pid_0840#... on Windows
    std::string friendly_name;
    std::string serial;
    uint8_t     bus_speed = 0;  // 1 = low, 2 = full, 3 = high
};

class IUsbTransport {
public:
    virtual ~IUsbTransport() = default;

    // -- Control endpoint -------------------------------------------------
    virtual Status ControlOut(uint8_t request, uint16_t value, uint16_t index,
                              const uint8_t* data, size_t len) = 0;

    virtual Status ControlIn(uint8_t request, uint16_t value, uint16_t index,
                             uint8_t* data, size_t len, size_t* transferred) = 0;

    // -- Interface / endpoint state ---------------------------------------
    virtual Status SetAltSetting(uint8_t alt) = 0;

    // wMaxPacketSize of the isochronous endpoint in the given alt setting.
    virtual Status GetIsoMaxPacketSize(uint8_t alt, uint16_t* max_packet) = 0;

    // -- Isochronous streaming --------------------------------------------
    // StartIso() must have been preceded by SetAltSetting(kAltStreaming).
    // It returns once streaming is running; packets arrive on `sink` from an
    // internal thread until StopIso() returns.
    virtual Status StartIso(IIsoSink* sink) = 0;
    virtual void   StopIso() = 0;
    virtual bool   IsStreaming() const = 0;

    virtual const UsbDeviceInfo& Info() const = 0;
};

// Platform device discovery. Returns every attached device whose VID/PID is in
// kKnownDevices and that is currently bound to WinUSB.
Status EnumerateDevices(std::vector<UsbDeviceInfo>* out);

// Opens by device path (as returned by EnumerateDevices).
Status OpenDevice(const std::string& device_path,
                  std::unique_ptr<IUsbTransport>* out);

}  // namespace qcam

#endif  // QCAM_USB_H_
