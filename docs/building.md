# Building

## What you need

| | |
| --- | --- |
| **Visual Studio 2022 or newer** | "Desktop development with C++" workload |
| **Windows 11 SDK** | 10.0.22000 or later — `mfvirtualcamera.h` and `mfsensorgroup.lib` are not in older SDKs. CI builds against 10.0.26100 |
| **CMake** | 3.20 or later (ships with Visual Studio) |

No WDK is required. There is no kernel-mode code, and `qcamusb.inf` references
only the inbox WinUSB driver. You will want the WDK's signing tools for
distribution — see `docs/installing.md` — but not to build.

## Windows

```powershell
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo
```

No `-G`: CMake's default generator on Windows is the newest Visual Studio it
can find. Pinning one (`-G "Visual Studio 17 2022"`) works but ages badly —
it is exactly what broke CI when the hosted runner image moved on. `-A`
selects the target architecture and every Visual Studio generator accepts it.

Output in `build\RelWithDebInfo\`:

```
qcamctl.exe     diagnostics and capture
qcamsvc.exe     the frame broker service
qcamvcam.dll    the Media Foundation virtual camera source
qcamusb.inf     copied from driver/ by the build
```

For ARM64, use `-A ARM64`. The INF already covers `NTarm64`.

Ninja works too and is considerably faster:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

## Linux or macOS

The portable core and the tests build anywhere with a C++17 compiler. This is
how the protocol, framing and imaging layers are developed and tested without
a camera:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/qcam_tests
./build/qcamctl selftest
```

`qcamctl` builds here too. It cannot open a camera — device enumeration is
Windows-only — but `selftest` drives the whole stack against the mock
transport, which is a genuine smoke test of everything above `IUsbTransport`.

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `QCAM_BUILD_TESTS` | `ON` | Build `qcam_tests` |
| `QCAM_ASAN` | `OFF` | AddressSanitizer + UBSan (non-MSVC) |

The demosaic reads neighbouring pixels and clamps at the frame edges, which is
exactly the kind of code that hides an off-by-one. Run it under the
sanitizers when touching `src/core/decode.cpp`:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DQCAM_ASAN=ON
cmake --build build-asan && ./build-asan/qcam_tests
```

## Tests

```bash
./build/qcam_tests             # everything
./build/qcam_tests Hdcs        # only tests whose name contains "Hdcs"
./build/qcam_tests -v          # with driver logging
```

The suite has no dependencies — the harness is `tests/test_harness.h`, about a
hundred lines. Adding a test is a `TEST(Name) { ... }` block in any file in
`tests/`; the file needs adding to `CMakeLists.txt`.

What is covered:

| File | What it pins |
| --- | --- |
| `test_bridge.cpp` | Exact bytes of every control transfer, I2C block layout, staging windows, burst splitting, bridge-specific quirks |
| `test_hdcs.cpp` | Sensor probe, init tables, window arithmetic, exposure timing against hand-worked values, gain folding, power states |
| `test_framer.cpp` | Chunk parsing, every SOF/EOF encoding, short and over-long frames, truncated chunks, bridge-specific data-chunk rules |
| `test_decode.cpp` | Bayer phases, demosaic correctness on flat fields, colour conversion, crop and scale, format validation |
| `test_autoexp.cpp` | Convergence, deadband, rail behaviour, exposure-before-gain, settle timing |
| `test_camera.cpp` | End-to-end: open, probe, init, stream synthetic packets, decode |

## Layout

```
include/qcam/      public headers for the core library
src/core/          portable: protocol, framing, imaging   (no Windows)
src/win/           WinUSB transport, enumeration, ring, vcam registration
src/qcamsvc/       the service
src/qcamvcam/      the Media Foundation source (COM in-proc server)
src/qcamctl/       the CLI
driver/            the INF
tests/             unit tests
scripts/           install/uninstall PowerShell
```

The rule the layout enforces: anything in `src/core/` that needs to touch
hardware does it through `IUsbTransport`. If a `#include <windows.h>` appears
there, the tests stop building on Linux, which is the intended alarm.
