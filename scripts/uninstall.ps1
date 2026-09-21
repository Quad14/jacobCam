#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Removes the qcam driver stack.

.DESCRIPTION
    Unwinds install.ps1: stops and deletes the service, removes the registered
    virtual camera, unregisters the COM server, and optionally removes the
    driver package from the driver store.

.PARAMETER BinDir
    Directory holding qcamsvc.exe and qcamvcam.dll.

.PARAMETER RemoveDriver
    Also remove the qcamusb driver package, so the camera goes back to being
    an unrecognised USB device.
#>
[CmdletBinding()]
param(
    [string] $BinDir = $PSScriptRoot,
    [switch] $RemoveDriver
)

$ErrorActionPreference = 'Continue'

function Write-Step([string] $Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

$BinDir = (Resolve-Path -LiteralPath $BinDir).Path
$svc    = Join-Path $BinDir 'qcamsvc.exe'
$vcam   = Join-Path $BinDir 'qcamvcam.dll'

Write-Step "Stopping the service"
Stop-Service -Name 'qcamsvc' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Step "Removing the registered virtual camera"
if (Test-Path -LiteralPath $svc) {
    & $svc --remove-vcam
} else {
    Write-Warning "qcamsvc.exe not found; skipping virtual camera removal"
}

Write-Step "Unregistering the COM media source"
if (Test-Path -LiteralPath $vcam) {
    & regsvr32.exe /u /s $vcam
    Write-Host "  unregistered $vcam"
} else {
    Write-Warning "qcamvcam.dll not found; skipping"
}

Write-Step "Removing the service"
if (Test-Path -LiteralPath $svc) { & $svc --uninstall }

if ($RemoveDriver) {
    Write-Step "Removing the driver package"
    # Find our package by its provider name, then delete it.
    $packages = & pnputil.exe /enum-drivers
    $current  = $null
    $matches  = @()
    foreach ($line in $packages) {
        if ($line -match '^Published Name\s*:\s*(\S+)') { $current = $Matches[1] }
        if ($line -match 'qcam' -and $current) {
            $matches += $current
            $current = $null
        }
    }
    if ($matches.Count -eq 0) {
        Write-Host "  no qcam driver package found in the driver store"
    }
    foreach ($package in ($matches | Select-Object -Unique)) {
        Write-Host "  deleting $package"
        & pnputil.exe /delete-driver $package /uninstall /force
    }
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
