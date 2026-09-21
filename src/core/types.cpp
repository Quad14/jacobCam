// SPDX-License-Identifier: GPL-2.0-or-later
#include "qcam/types.h"

namespace qcam {

const char* StatusName(Status s) {
    switch (s) {
        case Status::Ok:          return "Ok";
        case Status::NoDevice:    return "NoDevice";
        case Status::Io:          return "Io";
        case Status::Timeout:     return "Timeout";
        case Status::Unsupported: return "Unsupported";
        case Status::InvalidArg:  return "InvalidArg";
        case Status::Busy:        return "Busy";
        case Status::Protocol:    return "Protocol";
        case Status::NoMemory:    return "NoMemory";
        case Status::Cancelled:   return "Cancelled";
    }
    return "Unknown";
}

const char* BridgeName(Bridge b) {
    switch (b) {
        case Bridge::Stv0600: return "STV0600";
        case Bridge::Stv0602: return "STV0602";
        case Bridge::Stv0610: return "STV0610";
        case Bridge::St6422:  return "ST6422";
    }
    return "unknown";
}

// Ordered with the target camera first. The bridge type per PID comes from the
// STV06xx family's published device table; see docs/hardware.md.
const DeviceId kKnownDevices[] = {
    // P/N 861037-0000, M/N V-UB2 - the camera this driver was written for.
    {kVendorLogitech, 0x0840, Bridge::Stv0600, "QuickCam Express"},
    {kVendorLogitech, 0x0850, Bridge::Stv0610, "QuickCam Web / LEGO Cam"},
    {kVendorLogitech, 0x0870, Bridge::Stv0602, "Dexxa WebCam USB"},
    {kVendorLogitech, 0x08f0, Bridge::St6422,  "QuickCam Messenger"},
    {kVendorLogitech, 0x08f5, Bridge::St6422,  "QuickCam Communicate"},
    {kVendorLogitech, 0x08f6, Bridge::St6422,  "QuickCam Messenger (rev 2)"},
};

const size_t kKnownDeviceCount = sizeof(kKnownDevices) / sizeof(kKnownDevices[0]);

const DeviceId* LookupDevice(uint16_t vid, uint16_t pid) {
    for (size_t i = 0; i < kKnownDeviceCount; ++i) {
        if (kKnownDevices[i].vid == vid && kKnownDevices[i].pid == pid)
            return &kKnownDevices[i];
    }
    return nullptr;
}

}  // namespace qcam
