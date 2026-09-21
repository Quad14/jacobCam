# qcam — a modern Windows driver for the Logitech QuickCam Express

A user-mode Windows 10/11 driver for a 1999 USB webcam, so it shows up as an
ordinary camera in Teams, Zoom, OBS and the Windows Camera app.

The camera this was written for, identified from the label:

| Field | Value |
| --- | --- |
| Model | **V-UB2** — Logitech QuickCam Express |
| Part number | **861037-0000** |
| USB ID | `046D:0840` |
| Bridge ASIC | STMicroelectronics **STV0600** |
| Image sensor | Agilent/HP **HDCS-1000**, 1/4" CMOS |
| Native output | 360×296, 8-bit Bayer GRBG |
| Frame rate | ~7.9 fps (USB full speed, bandwidth-limited) |
| Bus power | 5 V, 100 mA |

The part number is decisive: `861037` appears verbatim in the STV06xx family's
published hardware table as *"Sensor HDCS1000, ASIC STV0600"*. There is no
guesswork about what is inside this particular camera.

## Why the camera doesn't work today

It predates USB Video Class by about four years. UVC arrived in 2003 and is
what makes a modern webcam driverless; a 1999 camera speaks a vendor-specific
protocol that Windows has no idea about. The original Logitech driver was a
32-bit Windows 98/2000-era kernel driver and will not install — or load — on a
modern 64-bit Windows.

## How this works instead

```
   ┌──────────────────────────────────────────────────────────┐
   │  Teams / Zoom / OBS / Camera app                         │
   └───────────────────────▲──────────────────────────────────┘
                           │  Media Foundation / DirectShow bridge
   ┌───────────────────────┴──────────────────────────────────┐
   │  qcamvcam.dll — MF virtual camera source                 │
   │  loaded by the Windows Frame Server                      │
   └───────────────────────▲──────────────────────────────────┘
                           │  shared-memory frame ring (NV12)
   ┌───────────────────────┴──────────────────────────────────┐
   │  qcamsvc.exe — frame broker (Windows service)            │
   │  owns the device · STV0600 + HDCS-1000 init              │
   │  iso reassembly · demosaic · auto-exposure               │
   └───────────────────────▲──────────────────────────────────┘
                           │  WinUSB API (control + isochronous)
   ┌───────────────────────┴──────────────────────────────────┐
   │  WinUSB.sys — inbox, already Microsoft-signed            │
   │  bound by qcamusb.inf                                    │
   └───────────────────────▲──────────────────────────────────┘
                           │  USB 1.1 full speed
                    [ QuickCam Express ]
```

**There is no custom kernel-mode code.** The only driver binary involved is
`WinUSB.sys`, which ships with Windows. Everything specific to this camera —
the bridge protocol, the sensor register sequences, framing, demosaic,
exposure control — runs in user mode. That means no WHQL submission, no EV
certificate for a kernel driver, no possibility of bugchecking the machine,
and a debugging loop that is just a console tool.

This is only possible because two things landed in Windows after this camera
was discontinued: **user-mode isochronous transfers in WinUSB** (Windows 8.1)
and the **`MFCreateVirtualCamera` API** (Windows 11 build 22000), which lets a
user-mode media source appear to every app as a real camera.

## Components

| Path | What it is |
| --- | --- |
| `driver/qcamusb.inf` | Binds `046D:0840` (and the rest of the STV06xx family) to inbox WinUSB |
| `src/core/` | Portable protocol, framing, demosaic, auto-exposure. No Windows dependency |
| `src/win/` | WinUSB transport, device enumeration, shared-memory ring, vcam registration |
| `src/qcamsvc/` | The frame broker service |
| `src/qcamvcam/` | Media Foundation virtual camera source (COM in-proc server) |
| `src/qcamctl/` | Diagnostics and capture CLI |
| `tests/` | 96 unit tests, runnable with no hardware attached |

## Quick start

```powershell
# Build (Visual Studio 2022 with the Desktop C++ workload)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo

# Install (administrator). See docs/installing.md about driver signing first.
.\scripts\install.ps1 -BinDir .\build\RelWithDebInfo

# Check it
.\build\RelWithDebInfo\qcamctl.exe list
.\build\RelWithDebInfo\qcamctl.exe probe
.\build\RelWithDebInfo\qcamctl.exe capture -n 3
```

`qcamctl` is the tool to reach for when something is wrong: it brings the
camera up one layer at a time and tells you which layer failed.

```
qcamctl list       cameras bound to the WinUSB driver
qcamctl probe      open the device and read back what the sensor says it is
qcamctl regdump    dump every bridge and sensor register
qcamctl capture    write frames to BMP, or the raw Bayer mosaic
qcamctl stream     throughput, frame rate, dropped/short/unknown chunk counts
qcamctl selftest   exercise the whole stack against a mock device
```

## State of the code

Honest summary, because it matters for what you do next.

**Verified here.** The portable core — bridge register encoding, the I2C
staging format, HDCS-1000 probe/init/window/exposure/gain sequences, the
isochronous chunk framer, demosaic, colour conversion, and the auto-exposure
loop — is covered by 96 unit tests that pass, including under
AddressSanitizer and UndefinedBehaviorSanitizer. `qcamctl selftest` drives the
entire stack end to end against a mock transport and decodes a synthetic
frame.

**Not verified here, because it needs the hardware and a Windows machine.**
The WinUSB transport, the INF binding, the service, and the Media Foundation
virtual camera have never been compiled by MSVC or run. They were written
against the documented APIs, and this environment is Linux with no camera
attached. Expect to fix compile errors on the first Windows build.

**Known to need checking on real hardware.** The register sequences come from
the documented STV06xx protocol rather than from a capture of *your* camera.
If frames arrive but look wrong, `qcamctl stream` will say whether the chunk
layer is being parsed correctly, and `qcamctl capture -f raw` gives you the
Bayer mosaic to inspect directly. See `docs/troubleshooting.md`.

**Windows 10.** Everything works except the system-wide camera:
`MFCreateVirtualCamera` is Windows 11 only. On Windows 10 the service and
`qcamctl` still capture; making the camera visible to apps there needs a
DirectShow source filter, which is not written yet.

## Documentation

- [`docs/hardware.md`](docs/hardware.md) — the camera, the chipset, how the label decodes
- [`docs/protocol.md`](docs/protocol.md) — the STV0600 wire protocol, in detail
- [`docs/architecture.md`](docs/architecture.md) — why the stack is shaped this way
- [`docs/building.md`](docs/building.md) — toolchain and build options
- [`docs/installing.md`](docs/installing.md) — installation, and the driver signing problem
- [`docs/troubleshooting.md`](docs/troubleshooting.md) — what to do when it doesn't work

## Licence

**GPL-2.0-or-later.** See [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md).

This is not an arbitrary choice. The STV0600 register sequences and the chunk
framing were derived from the Linux `gspca/stv06xx` driver, which is
GPL-2.0-or-later; work derived from it inherits that licence. If you need this
code under different terms you would have to re-derive the protocol
independently, from USB captures of the hardware. `NOTICE.md` records exactly
what was derived and from where.
