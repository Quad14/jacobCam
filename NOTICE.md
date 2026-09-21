# Provenance and attribution

## Derived work

The STV06xx bridge protocol and the HDCS-1000 register sequences implemented
in this project were derived from the Linux kernel driver
`drivers/media/usb/gspca/stv06xx/`, which is licensed **GPL-2.0-or-later**.

Because that protocol knowledge is embedded in this implementation, this
project is a derivative work and is distributed under the same licence. See
`LICENSE`.

Copyright holders named in the original driver:

- Copyright (c) 2001 Jean-Fredric Clere, Nikolas Zimmermann, Georg Acher,
  Mark Cave-Ayland, Carlo E Prelz, Dick Streefland
- Copyright (c) 2002, 2003 Tuukka Toivonen
- Copyright (c) 2008 Erik Andrén
- Copyright (c) 2008 Chia-I Wu

### What specifically was derived

| Area | Source |
| --- | --- |
| Vendor request `0x04` semantics, register addresses | `stv06xx.h`, `stv06xx.c` |
| I2C command block layout, staging windows `0x0400` / `0x1400` / `0x1410` | `stv06xx.c` |
| Isochronous chunk ids and framing rules | `stv06xx.c` (`stv06xx_pkt_scan`) |
| Bridge init table for HDCS sensors | `stv06xx_hdcs.h` (`stv_bridge_init`) |
| HDCS register map and init table | `stv06xx_hdcs.h` |
| Exposure timing model, gain folding, window arithmetic | `stv06xx_hdcs.c` |
| USB device / bridge-type table | `stv06xx.c` (`device_table`) |
| Part-number to sensor/ASIC mapping | file headers in `stv06xx*` |

The exposure timing model itself originates in the Agilent HDCS-1000
datasheet, section 3.4.5.5 (and 3.5.6.4 for the HDCS-1020).

### What is original to this project

The Windows driver architecture, the WinUSB transport including the
isochronous pipeline, the shared-memory frame ring, the Media Foundation
virtual camera source, the service, the demosaic and colour pipeline, the
auto-exposure controller, the diagnostics tool, and the test suite.

## Trademarks

Logitech and QuickCam are trademarks of Logitech International S.A. Agilent is
a trademark of Agilent Technologies. This project is not affiliated with,
endorsed by, or supported by either company. The names are used only to
identify the hardware this software drives.
