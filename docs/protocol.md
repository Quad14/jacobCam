# The STV0600 protocol

Everything here is implemented in `src/core/bridge.cpp`,
`src/core/sensor_hdcs.cpp` and `src/core/framer.cpp`, and pinned by the tests
in `tests/test_bridge.cpp`, `tests/test_hdcs.cpp` and `tests/test_framer.cpp`.

## Bridge registers

The bridge exposes a flat 16-bit register space through exactly one vendor
request.

| Field | Write | Read |
| --- | --- | --- |
| `bmRequestType` | `0x40` (host→device, vendor, device) | `0xC0` (device→host) |
| `bRequest` | `0x04` | `0x04` |
| `wValue` | register address | register address |
| `wIndex` | `0` | `0` |
| `wLength` | 1 or 2 | 1 |

Reads always return a single byte. Writes send one byte, or two when the value
exceeds `0xFF` — the only register in the init paths where that happens is the
isochronous packet size.

### Register map

```
0x0400   I2C write staging window        (23-byte block)
0x0423   REG23 — cleared during init
0x1400   I2C read staging window         (23-byte block)
0x1410   I2C read result
0x1420   I2C partner address
0x1421   I2C register/value pair count minus one
0x1422   I2C read/write toggle
0x1423   I2C flush
0x1424   I2C successive read register values
0x1440   isochronous enable              (1 = stream, 0 = stop)
0x1443   scan rate
0x1445   LED control
0x1446   STV0600 emulation mode          (STV0602 only)
0x1500   REG00 ─┐
0x1501   REG01   │
0x1502   REG02   ├─ video formatter timing
0x1503   REG03   │
0x1504   REG04 ─┘
0x15c1   isochronous packet size, low
0x15c2   isochronous packet size, high
0x15c3   Y control    — 1 = 288 lines (CIF), 2 = 144 lines (QCIF)
0x1620   reset
0x1680   X control    — 0x0A = 352 columns, 0x06 = 176 columns
0x1704   I2C commit   (STV0610 only)
```

## Reaching the sensor over I2C

The bridge's I2C master is driven by staging a fixed **23-byte (`0x23`)
command block** into a window, whereupon the ASIC clocks it out.

```
offset  0x00 .. 0x0F   register addresses   (up to 16 byte-wide entries)
offset  0x10 .. 0x1F   register values      (or 8 word-wide entries)
offset  0x20           I2C slave address
offset  0x21           entry count minus one
offset  0x22           command: 1 = write, 3 = read
```

The subtlety that will cost you an afternoon if you get it wrong:

> **Writes stage into `0x0400`. Reads stage into `0x1400`.**

They are different windows. Staging a read command into the write window
produces no error — the transfer succeeds, the ASIC does nothing useful, and
the subsequent result read returns stale data. `tests/test_bridge.cpp` asserts
both addresses explicitly for this reason.

### Writing sensor registers

```
1.  Build the 23-byte block: addresses at 0x00, values at 0x10,
    slave at 0x20, count-1 at 0x21, command 1 at 0x22.
2.  Control OUT, wValue = 0x0400, 23 bytes.
3.  On an STV0610 only: control OUT, wValue = 0x1704, one zero byte.
    Other bridges commit on the staging write itself.
```

More than 16 register writes are split across several blocks automatically.

### Reading a sensor register

```
1.  Write the sensor's flush value to 0x1423.
2.  Build a block with the register address at 0x00, slave at 0x20,
    count 0 at 0x21, command 3 at 0x22.
3.  Control OUT, wValue = 0x1400, 23 bytes.
4.  Control IN, wValue = 0x1410, 1 byte (or 2 for word-wide sensors).
```

### HDCS register addressing

Bit 0 of an HDCS register address is the read/write flag, so the architectural
register index is shifted left one bit to form the address on the wire, and
consecutive registers are **two apart**:

```
HDCS_IDENT   index 0x00  →  address 0x00
HDCS_STATUS  index 0x01  →  address 0x02
HDCS_FWROW   index 0x0a  →  address 0x14
HDCS_FWCOL   index 0x0b  →  address 0x16
```

`Stv06xxBridge::WriteSensorSeq()` exists to get this stepping right when
writing a run of consecutive registers, such as the four window registers or
the four PGA gain registers.

## Bringing the camera up

```
  1.  Wait ~250 ms after enumeration. The ASIC does not answer I2C
      reliably before that.

  2.  Read sensor register 0x00. 0x08 = HDCS-1000/1100, 0x10 = HDCS-1020.
      Anything else is a sensor this driver does not implement.

  3.  STV0602 only: write 1 to 0x1446 (STV0600 emulation).

  4.  Bridge init table:
          0x1440 = 0x00     isochronous off while we configure
          0x0423 = 0x00
          0x1500 = 0x1d
          0x1501 = 0xb5
          0x1502 = 0xa8
          0x1503 = 0x95
          0x1504 = 0x07
          0x1443 = 0x20     scan rate
          0x15c3 = 0x01     288-line vertical timing
          0x1680 = 0x0a     352-column horizontal timing

  5.  Sensor soft reset: CONTROL = 1, then CONTROL = 0.

  6.  Sensor init table:
          STATUS  = 0x7e    write 1 to clear each status bit
          IMASK   = 0x00    no interrupts
          PCTRL   = 0x63    pad control
          PDRV    = 0x00
          ICTRL   = 0x20
          ITMG    = 0x12
          ADCCTRL = 10      10-bit ADC output

  7.  CONFIG  = bit 3       continuous capture; without this the sensor
                            stops after a single frame

  8.  TCTRL   = (3 << 5) | PSMP   for HDCS-1000/1100
                (3 << 6) | PSMP   for HDCS-1020
      PSMP is 5 on an STV0600, 20 on an STV0602, 6 on the 1020.

  9.  Window registers (see below).

 10.  Initial exposure and gain.
```

## The readout window

The window registers count in units of **four pixels**, and the window is
centred in the pixel array:

```
x    = array.left + (array.width  - width)  / 2
y    = array.top  + (array.height - height) / 2

FWROW = y / 4
FWCOL = x / 4
LWROW = (y + height) / 4 - 1
LWCOL = (x + width)  / 4 - 1
```

For the HDCS-1000 at its full 360 × 296, with the array origin at (8, 8):

```
FWROW = 2,  FWCOL = 2,  LWROW = 75,  LWCOL = 91
```

## Exposure

Integration time is expressed as a whole number of row periods plus a sub-row
remainder. Both come out of the sensor's own pixel timing, from the HDCS-1000
datasheet section 3.4.5.5.

```
cycles  = value × 25 MHz × 257

ct      = cto + PSMP + (ADC_START_SIG_DUR + 2)     column time period
cp      = cto + (width × ct / 2)                   column processing period
rp      = rs + cp                                  row period

rowexp  = cycles / rp
cycles -= rowexp × rp                              remainder

HDCS-1000/1100:
    srowexp     = cp - er - 6 - cycles
    mnct        = ceil((er + 5) / ct)
    max_srowexp = cp - mnct × ct - 1

HDCS-1020:
    srowexp     = width - (cycles + er + 13) / ct
    mnct        = ceil((er + 12) / ct)
    max_srowexp = width - mnct
```

with the per-part timing constants:

| | cto | cpo | rs | er |
| --- | --- | --- | --- | --- |
| HDCS-1000/1100 | 4 | 2 | 186 | 100 |
| HDCS-1020 | 3 | 3 | 155 | 96 |

Worked through for the default control value of 48, at 360 wide with PSMP 5:

```
cycles  = 48 × 25 × 257  = 308,400
ct      = 4 + 5 + 5      = 14
cp      = 4 + 360×14/2   = 2,524
rp      = 186 + 2,524    = 2,710
rowexp  = 308,400/2,710  = 113        remainder 2,170
srowexp = 2,524 - 100 - 6 - 2,170     = 248
```

`tests/test_hdcs.cpp` asserts exactly these numbers.

The register burst that applies an exposure **halts and restarts the sensor**:

```
CONTROL   = 0x00      stop
ROWEXPL   = rowexp & 0xff
ROWEXPH   = rowexp >> 8
SROWEXPL  = srowexp & 0xff
SROWEXPH  = srowexp >> 8
STATUS    = 0x10      clear the exposure error flag
CONTROL   = 0x04      run
```

Because that final write asserts RUN unconditionally, the driver restores the
previous power state afterwards if the sensor was not supposed to be running.

## Gain

Voltage gain is `Av = (1 + 19·v/127) × (1 + bit7)`, so control values above 127
fold into the doubling bit:

```
if (v > 127) v = 0x80 | (v / 2);
```

The value is written to all four PGA registers (`ERECPGA`, `EROCPGA`,
`ORECPGA`, `OROCPGA`) — one per Bayer quadrant. Setting them unequally
produces a checkerboard colour cast.

## Starting the stream

```
1.  Select alternate setting 1 on interface 0.
2.  Write the endpoint's actual wMaxPacketSize to 0x15c1.
    Telling the ASIC a larger size than the host controller granted makes
    every packet over-run.
3.  Sensor CONTROL = 0x04 (run).
4.  Write 1 to 0x1440 (isochronous enable).
```

Stopping reverses it: `0x1440 = 0`, sensor to sleep, alternate setting 0.

## The isochronous stream

Each packet carries an integral number of chunks. A chunk is a four-byte
header followed by its payload:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          chunk id (BE)        |      payload length (BE)      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        payload ...                            |
```

| Chunk id | Meaning |
| --- | --- |
| `0x8001`, `0x8005`, `0xC001`, `0xC005` | start of frame (zero length) |
| `0x8002`, `0x8006`, `0xC002` | end of frame (zero length) |
| `0x0200`, `0x4200` | image data |
| `0x0005` | 11 bytes, unknown; appears just before end of frame in compressed mode |
| `0x0100` | 2 bytes, unknown; appears a few times per transfer |
| `0x42FF` | seen occasionally on the ST6422 |

On the **ST6422** only, any chunk whose high byte is `0x02` is image data —
that part varies the low byte. On every other bridge, `0x0207` is an unknown
chunk, not data. The framer keys this off the bridge type.

The payload is raw 8-bit Bayer in GRBG phase, in raster order, no padding.
A complete 360 × 296 frame is 106,560 bytes spread across roughly 126 packets.

### Framing rules the driver applies

- Data before the first start-of-frame is discarded. Joining a running stream
  mid-frame is normal at startup.
- A start-of-frame while a frame is already in progress abandons the partial
  frame.
- A chunk header claiming more payload than the packet contains invalidates
  the frame in flight; it means the stream is out of sync.
- More data than the geometry allows is clipped, and the frame is still
  delivered rather than dropped.
- A frame that ends short is padded with mid-grey and flagged `complete =
  false`, so a dropped tail shows as a grey band instead of last frame's
  content.

All of these are covered by tests.
