// SPDX-License-Identifier: GPL-2.0-or-later
//
// qcam - Logitech QuickCam Express (STV0600 + HDCS-1000) driver for Windows
//
// Core value types shared by every layer of the stack.

#ifndef QCAM_TYPES_H_
#define QCAM_TYPES_H_

#include <cstddef>
#include <cstdint>

namespace qcam {

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
// The core library is compiled with exceptions disabled on the Windows side
// (it runs inside a service and inside the Frame Server's COM host), so every
// fallible operation returns a Status instead of throwing.
enum class Status : int {
    Ok = 0,
    NoDevice,      // device is not present / was unplugged
    Io,            // USB transfer failed
    Timeout,       // transfer did not complete in time
    Unsupported,   // asked for something this hardware cannot do
    InvalidArg,    // caller error
    Busy,          // device already claimed by another process
    Protocol,      // device answered, but not with something we understand
    NoMemory,
    Cancelled,
};

const char* StatusName(Status s);

inline bool Failed(Status s) { return s != Status::Ok; }
inline bool Succeeded(Status s) { return s == Status::Ok; }

// Evaluate `expr`; propagate a non-Ok Status to the caller.
#define QCAM_TRY(expr)                              \
    do {                                            \
        ::qcam::Status qcam_try_status_ = (expr);   \
        if (::qcam::Failed(qcam_try_status_))       \
            return qcam_try_status_;                \
    } while (0)

// ---------------------------------------------------------------------------
// Hardware identity
// ---------------------------------------------------------------------------
constexpr uint16_t kVendorLogitech = 0x046d;

// The STV06xx family. The bridge type changes a handful of init details, so we
// carry it explicitly rather than inferring it at each site.
enum class Bridge : uint8_t {
    Stv0600 = 0,  // QuickCam Express        (046d:0840)  <- P/N 861037-0000
    Stv0602 = 1,  // Dexxa WebCam USB        (046d:0870)
    Stv0610 = 2,  // LEGO cam / QuickCam Web (046d:0850)
    St6422  = 3,  // QuickCam Messenger/Communicate, sensor integrated
};

const char* BridgeName(Bridge b);

struct DeviceId {
    uint16_t vid;
    uint16_t pid;
    Bridge   bridge;
    const char* marketing_name;
};

// Every STV06xx device this driver knows how to bind. Index 0 is the camera
// this project was written for.
extern const DeviceId kKnownDevices[];
extern const size_t   kKnownDeviceCount;

// Returns nullptr when the vid/pid pair is not one of ours.
const DeviceId* LookupDevice(uint16_t vid, uint16_t pid);

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------
// Exposure and gain are carried through the stack on a normalised 0..255
// scale; each sensor maps that onto its own register semantics.
constexpr int kCtrlMin     = 0;
constexpr int kCtrlMax     = 255;
constexpr int kDefExposure = 48;
constexpr int kDefGain     = 50;

// ---------------------------------------------------------------------------
// Pixel geometry
// ---------------------------------------------------------------------------

// Bayer colour filter array phase, named by the top-left 2x2 quad read in
// raster order. The HDCS-1000 behind an STV0600 delivers GRBG:
//
//     G R G R ...
//     B G B G ...
enum class BayerPhase : uint8_t { GRBG, RGGB, BGGR, GBRG };

struct FrameGeometry {
    uint16_t   width;
    uint16_t   height;
    BayerPhase phase;

    size_t RawSize() const {
        return static_cast<size_t>(width) * static_cast<size_t>(height);
    }
};

// A completed, reassembled raw Bayer frame handed up by the framer.
struct RawFrame {
    const uint8_t* data;    // width*height bytes, 8bpp Bayer
    size_t         size;
    uint64_t       sequence;
    uint64_t       timestamp_100ns;  // capture time, QPC-derived on Windows
    bool           complete;         // false => short frame, padded with grey
};

}  // namespace qcam

#endif  // QCAM_TYPES_H_
