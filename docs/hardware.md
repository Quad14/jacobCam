# The hardware

## Reading the label

```
Logitech
M/N: V-UB2        Rating: 5V ⎓, 100mA
P/N: 861037-0000
S/N: L2C95165409                        MADE IN CHINA
FCC — Tested To Comply With FCC Standards
FOR HOME OR OFFICE USE
```

| Field | Meaning |
| --- | --- |
| `M/N: V-UB2` | Logitech's internal model code. `V-U…` is the USB camera series. This one is the **QuickCam Express**, the 1999 consumer model. |
| `P/N: 861037-0000` | The part number, and the useful one. `861037` identifies the exact sensor/ASIC combination. |
| `S/N: L2C95165409` | Unit serial. Not used by the driver — the STV0600 does not expose a serial number string over USB, so the driver identifies devices by their device path instead. |
| `5V ⎓, 100mA` | One USB unit load. Bus-powered, no external supply. Also a hint that it is a full-speed device: high-speed cameras of this era drew far more. |

The part number is what settles the internals. `861037` appears in the STV06xx
family's hardware table as:

```
P/N 861037:      Sensor HDCS1000        ASIC STV0600
```

alongside its siblings:

```
P/N 861050-0010: Sensor HDCS1000        ASIC STV0600
P/N 861050-0020: Sensor Photobit PB100  ASIC STV0600-1 — QuickCam Express
P/N 861055:      Sensor ST VV6410       ASIC STV0610   — LEGO cam
P/N 861075-0040: Sensor HDCS1000        ASIC
P/N 961179-0700: Sensor ST VV6410       ASIC STV0602   — Dexxa WebCam USB
P/N 861040-0000: Sensor ST VV6410       ASIC STV0610   — QuickCam Web
```

This matters more than it looks. Logitech shipped the *same* QuickCam Express
model with **three different sensors** over its life — HDCS-1000, Photobit
PB-0100, and ST VV6410 — behind three bridge revisions. The model name on the
box tells you nothing; the part number does. This driver implements the
HDCS-1000 path, which is what P/N 861037 carries, and probes the sensor
identity register at runtime rather than trusting the USB product id.

## The bridge: STMicroelectronics STV0600

A USB 1.1 camera controller. It is deliberately simple: it has no image
processing of its own, no compression worth the name on this variant, and no
knowledge of the sensor hanging off it. It does three things.

1. Exposes a flat 16-bit register space over a single vendor control request.
2. Acts as an I2C master, so the host can reach the sensor's registers.
3. Pumps pixel data from the sensor onto an isochronous IN endpoint, wrapped
   in a trivial chunk framing layer.

Everything else — deciding the exposure, white balancing, demosaicing — is the
host's problem. In 1999 that was a driver on a Pentium II. Today it is
`src/core/`.

## The sensor: Agilent HDCS-1000

A 1/4-inch CMOS sensor, part of the HP/Agilent HDCS line that later became
Avago and then Broadcom.

| Property | Value |
| --- | --- |
| Pixel array | 360 × 296 usable |
| Colour filter | Bayer, GRBG phase |
| ADC | 10-bit internal, truncated to 8 bits on the wire |
| Pixel clock | 25 MHz |
| I2C address | `0x55`, shifted left one bit on the wire (`0xAA`) |
| Identity register | `0x00` reads `0x08` |
| Exposure control | Row + sub-row integration counters |
| Gain | Four PGA registers, one per Bayer quadrant |

The pin-compatible **HDCS-1020** reads `0x10` from the identity register, has a
352 × 292 usable array at a different array offset, and moves several registers
around. The driver supports both and distinguishes them at probe time.

Two things about this sensor shape the driver:

**No auto-exposure hardware.** None. If nothing writes the exposure registers,
the image stays at whatever the last write left behind. The auto-exposure
controller in `src/core/autoexp.cpp` is not a nicety; without it the camera
produces a black or blown-out picture depending on the room.

**No white balance at all.** The colour filter array is not very selective and
there is no on-sensor correction. The grey-world estimator in
`src/core/decode.cpp` is what stops everything looking green.

## Why ~7.9 fps

The camera is USB **full speed** (12 Mbit/s). The host schedules one
isochronous packet per 1 ms bus frame, and the endpoint's `wMaxPacketSize` is
847 bytes.

```
847 bytes/ms × 1000 ms/s          = 847,000 bytes/s
360 × 296                         = 106,560 bytes per frame
847,000 / 106,560                 ≈ 7.9 frames/s
```

Four bytes of every packet go to the chunk header, so the real figure is a
shade under that. The sensor itself could run faster; the bus is the limit.
This is why the driver advertises a fractional frame rate rather than rounding
up to 15 — see `docs/architecture.md`.

There is a second-order effect: the sensor's pixel sample period (`PSMP`)
trades frame rate against signal quality, and the usable minimum depends on
the bridge revision — 5 on an STV0600, 20 on an STV0602. The driver picks the
right one from the bridge type.

## USB device identifiers

| VID:PID | Product | Bridge | Supported |
| --- | --- | --- | --- |
| `046D:0840` | QuickCam Express | STV0600 | **yes** — this camera |
| `046D:0850` | QuickCam Web / LEGO Cam | STV0610 | binds; sensor (VV6410) not implemented |
| `046D:0870` | Dexxa WebCam USB | STV0602 | binds; sensor (VV6410) not implemented |
| `046D:08F0` | QuickCam Messenger | ST6422 | binds; integrated sensor not implemented |
| `046D:08F5` | QuickCam Communicate | ST6422 | binds; integrated sensor not implemented |
| `046D:08F6` | QuickCam Messenger rev 2 | ST6422 | binds; integrated sensor not implemented |

The INF binds all of them because the transport, framing and imaging layers
are shared; adding one of the missing cameras means writing a sensor back end
against the `ISensor` interface and nothing else. A device that binds but has
an unimplemented sensor reports that clearly at probe time rather than
pretending to work.

## USB descriptor layout

```
Device
 └─ Configuration 1
     └─ Interface 0
         ├─ Alt setting 0   no endpoints (idle, no bandwidth reserved)
         └─ Alt setting 1   1 × isochronous IN, endpoint 0x81
```

The camera reserves no isochronous bandwidth until the host selects alternate
setting 1, which is why the driver only switches to it when streaming actually
starts. Leaving it selected would hold a bandwidth reservation the whole time
the camera is plugged in, and on a busy USB controller that can stop other
devices from enumerating.
