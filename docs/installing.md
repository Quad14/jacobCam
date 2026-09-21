# Installing

Four pieces go in, in this order:

1. `qcamusb.inf` — binds the camera to the inbox WinUSB driver.
2. `qcamvcam.dll` — registered as a COM server.
3. `qcamsvc.exe` — installed and started as a service.
4. The virtual camera registration, which the service creates on startup.

`scripts\install.ps1` does all four. Run it from an elevated PowerShell:

```powershell
.\scripts\install.ps1 -BinDir .\build\RelWithDebInfo
```

Read the signing section first, because step 1 will fail without it.

## Driver signing — the part that will stop you

`qcamusb.inf` contains no code, but Windows still requires the **driver
package** to be signed before it will install on 64-bit Windows. There is no
way around this; it is not specific to this project.

Two routes.

### Development: test signing

Turns off the signature requirement on one machine. Fine for your own use,
unacceptable for distribution — it puts a watermark on the desktop and weakens
a real security boundary.

```powershell
# Elevated. Requires reboot, and Secure Boot must be off.
bcdedit /set testsigning on
```

Then create a test certificate and sign the package. The tools come from the
Windows Driver Kit:

```powershell
# One-off: make a self-signed test certificate
New-SelfSignedCertificate -Type Custom -Subject "CN=qcam test" `
    -KeyUsage DigitalSignature -FriendlyName "qcam test" `
    -CertStoreLocation "Cert:\LocalMachine\My" `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")

# Build a catalog for the package, then sign it
inf2cat /driver:driver /os:10_X64,10_ARM64 /verbose
signtool sign /fd sha256 /s My /n "qcam test" /t http://timestamp.digicert.com `
    driver\qcamusb.cat
```

The certificate must also be trusted on the machine — import it into both
`Trusted Root Certification Authorities` and `Trusted Publishers` under
`LocalMachine`.

To undo:

```powershell
bcdedit /set testsigning off
```

### Distribution: attestation signing

For anyone else to install this without weakening their machine, the package
needs attestation signing through the Microsoft **Partner Center** hardware
dashboard. That needs:

- A Partner Center hardware account.
- An **EV code-signing certificate** (a hardware token from a CA; this is the
  expensive part, typically a few hundred dollars a year).
- Uploading the package as a `.cab`; Microsoft signs it and returns it.

Attestation signing is available for INF-only packages like this one and does
**not** require WHQL testing — which is one of the practical benefits of the
no-kernel-code design.

## Installing the driver package manually

```powershell
pnputil /add-driver driver\qcamusb.inf /install
```

If the camera is already plugged in and sitting as an unknown device, force a
rescan or unplug and replug it. Confirm with:

```powershell
pnputil /enum-devices /class USBDevice
```

It should appear as *"Logitech QuickCam Express (qcam)"* under **Universal
Serial Bus devices** in Device Manager. If it is still under **Other devices**
with a yellow mark, the INF did not take — see `docs/troubleshooting.md`.

## Registering the virtual camera

```powershell
regsvr32 build\RelWithDebInfo\qcamvcam.dll
```

This writes the COM registration under
`HKLM\Software\Classes\CLSID\{9BB2B860-94A0-4B47-ADF7-7F3FBCA2FB6E}`. The DLL
must stay where it is — the registration records its path.

**The DLL is loaded by the Windows Frame Server, not by the application.** Put
it somewhere readable by `LOCAL SERVICE`; a per-user directory will not work.
`C:\Program Files\qcam\` is the right sort of place.

## Installing the service

```powershell
build\RelWithDebInfo\qcamsvc.exe --install
Start-Service qcamsvc
```

It runs as LocalSystem and starts automatically. It creates the virtual camera
registration on startup, so the camera appears in application pickers even
before the hardware is plugged in — it simply produces blank frames until then.

To run it in the foreground instead, which is how you debug it:

```powershell
Stop-Service qcamsvc
build\RelWithDebInfo\qcamsvc.exe --console -v
```

Useful options:

```
--size WxH        published frame size (default 352x288)
--no-vcam         do not register a system-wide camera
--name "TEXT"     friendly name shown in app camera pickers
```

If you change `--size`, the virtual camera adopts the live format when it is
instantiated. Restart the Frame Server (or reboot) after changing it, or
applications that already have the source loaded will keep the old format.

## Verifying

```powershell
qcamctl list               # is the device bound to WinUSB?
qcamctl probe              # does the sensor answer?
qcamctl capture -n 3       # do frames arrive and decode?
qcamctl stream -t 10       # frame rate and error counters
```

Then open the Camera app. *"Logitech QuickCam Express (qcam)"* should be in the
camera list.

## Uninstalling

```powershell
.\scripts\uninstall.ps1 -BinDir .\build\RelWithDebInfo -RemoveDriver
```

Without `-RemoveDriver` the WinUSB binding stays in place, which is what you
want when reinstalling a rebuilt binary.

## Windows 10

The driver, the service and `qcamctl` all work. The system-wide camera does
not: `MFCreateVirtualCamera` is Windows 11 build 22000 and later.
`install.ps1` detects this and skips that step.

Making the camera visible to applications on Windows 10 needs a DirectShow
source filter, which is not implemented. The frame ring is the integration
point for one — it is a documented shared-memory format, and a DirectShow
filter would read it exactly the way `qcamvcam` does.
