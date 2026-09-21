#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Installs the qcam driver stack for the Logitech QuickCam Express.

.DESCRIPTION
    Four steps, in order:
      1. Install qcamusb.inf, which binds the camera to the inbox WinUSB driver.
      2. Register qcamvcam.dll as the COM media source.
      3. Install and start qcamsvc, which owns the device and publishes frames.
      4. Verify the camera is visible.

    The INF must be signed, or the machine must be in test-signing mode.
    See docs/installing.md.

.PARAMETER BinDir
    Directory holding qcamctl.exe, qcamsvc.exe, qcamvcam.dll and qcamusb.inf.
    Defaults to the directory this script lives in.

.PARAMETER SkipDriver
    Skip the INF install (useful when re-running after only rebuilding).

.EXAMPLE
    .\install.ps1 -BinDir ..\build\RelWithDebInfo
#>
[CmdletBinding()]
param(
    [string] $BinDir = $PSScriptRoot,
    [switch] $SkipDriver,
    [switch] $NoVirtualCamera
)

$ErrorActionPreference = 'Stop'

function Write-Step([string] $Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Assert-File([string] $Path, [string] $What) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$What not found at $Path. Build the project first, or pass -BinDir."
    }
}

$BinDir  = (Resolve-Path -LiteralPath $BinDir).Path
$inf     = Join-Path $BinDir 'qcamusb.inf'
$svc     = Join-Path $BinDir 'qcamsvc.exe'
$ctl     = Join-Path $BinDir 'qcamctl.exe'
$vcam    = Join-Path $BinDir 'qcamvcam.dll'

Assert-File $svc  'qcamsvc.exe'
Assert-File $ctl  'qcamctl.exe'

# ---------------------------------------------------------------------------
Write-Step "Checking Windows version"
$build = [System.Environment]::OSVersion.Version.Build
Write-Host "  Windows build $build"
if ($build -lt 22000) {
    Write-Warning ("Virtual cameras need Windows 11 (build 22000+). " +
                   "The driver and qcamctl will work, but the camera will not " +
                   "appear in Teams/Zoom/OBS on this build.")
    $NoVirtualCamera = $true
}

# ---------------------------------------------------------------------------
if (-not $SkipDriver) {
    Write-Step "Installing the WinUSB binding (qcamusb.inf)"
    Assert-File $inf 'qcamusb.inf'

    # pnputil is the supported way to add a driver package to the store.
    $output = & pnputil.exe /add-driver $inf /install 2>&1
    $output | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) {
        Write-Warning ("pnputil returned $LASTEXITCODE. If it complains about a " +
                       "signature, see docs/installing.md - the package must be " +
                       "signed, or the machine put into test-signing mode.")
    }
} else {
    Write-Step "Skipping driver install (-SkipDriver)"
}

# ---------------------------------------------------------------------------
if (-not $NoVirtualCamera) {
    Write-Step "Registering the virtual camera media source"
    Assert-File $vcam 'qcamvcam.dll'
    & regsvr32.exe /s $vcam
    if ($LASTEXITCODE -ne 0) { throw "regsvr32 failed with exit code $LASTEXITCODE" }
    Write-Host "  registered $vcam"
}

# ---------------------------------------------------------------------------
Write-Step "Installing the frame broker service"
& $svc --install
if ($LASTEXITCODE -ne 0) { throw "qcamsvc --install failed" }

$arguments = @()
if ($NoVirtualCamera) { $arguments += '--no-vcam' }

Write-Host "  starting qcamsvc"
Start-Service -Name 'qcamsvc' -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$service = Get-Service -Name 'qcamsvc' -ErrorAction SilentlyContinue
if ($service) {
    Write-Host "  qcamsvc is $($service.Status)"
} else {
    Write-Warning "qcamsvc did not appear in the service list"
}

# ---------------------------------------------------------------------------
Write-Step "Looking for the camera"
& $ctl list
if ($LASTEXITCODE -ne 0) {
    Write-Warning ("No camera was found. Plug it in and re-run 'qcamctl list'. " +
                   "If it is plugged in, check Device Manager for a device with " +
                   "hardware id USB\VID_046D&PID_0840 and see docs/troubleshooting.md.")
} else {
    Write-Host ""
    Write-Host "Done. Try:" -ForegroundColor Green
    Write-Host "  qcamctl probe            # what the camera reports about itself"
    Write-Host "  qcamctl capture -n 3     # write three BMPs"
    Write-Host "  qcamctl stream -t 10     # throughput and frame statistics"
    if (-not $NoVirtualCamera) {
        Write-Host "  ...and open the Camera app; 'Logitech QuickCam Express (qcam)'"
        Write-Host "  should be in the camera list."
    }
}
