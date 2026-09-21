// SPDX-License-Identifier: GPL-2.0-or-later
//
// Device discovery for platforms other than Windows. The real implementation
// lives in src/win/enumerate.cpp; this exists so the core library and its
// tests build and run on a developer machine (or in CI) without a camera.

#include "qcam/log.h"
#include "qcam/usb.h"

namespace qcam {

Status EnumerateDevices(std::vector<UsbDeviceInfo>* out) {
    if (!out) return Status::InvalidArg;
    out->clear();
    QCAM_LOGD("device enumeration is only implemented on Windows");
    return Status::Ok;
}

Status OpenDevice(const std::string& device_path,
                  std::unique_ptr<IUsbTransport>* out) {
    (void)device_path;
    (void)out;
    QCAM_LOGE("opening a camera is only implemented on Windows");
    return Status::Unsupported;
}

}  // namespace qcam
