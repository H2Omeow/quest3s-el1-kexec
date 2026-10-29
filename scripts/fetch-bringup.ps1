<#
    SPDX-License-Identifier: GPL-2.0-only

    Fetch everything the headset needs from a GitHub release and verify it.

        scripts\fetch-bringup.ps1 [-Tag bringup-1] [-Out bringup]

    The five payload files are named exactly as scripts/bringup.sh expects them
    under /data/local/tmp/q3s/, so the directory this produces can be pushed
    as-is:

        adb shell mkdir -p /data/local/tmp/q3s
        adb push bringup/. /data/local/tmp/q3s/

    SHA256SUMS covers the five payload files; a missing or corrupted one aborts
    before anything reaches the device.
#>
[CmdletBinding()]
param(
    [string] $Tag = 'bringup-1',
    [string] $Out = 'bringup',
    [string] $Repo = 'H2Omeow/quest3s-el1-kexec'
)

$ErrorActionPreference = 'Stop'
$base = "https://github.com/$Repo/releases/download/$Tag"
$payload = @('Image.el1', 'initramfs.cpio.gz', 'quest-el1', 'quest_el1_kexec.ko', 'bringup.sh')
$sidecar = @('SHA256SUMS', 'manifest.json')

New-Item -ItemType Directory -Force -Path $Out | Out-Null

function Get-Asset([string] $name) {
    $dst = Join-Path $Out $name
    if ((Test-Path $dst) -and ((Get-Item $dst).Length -gt 0)) {
        Write-Host ("have   {0}" -f $name)
        return
    }
    Write-Host ("fetch  {0}" -f $name)
    Invoke-WebRequest -UseBasicParsing -Uri "$base/$name" -OutFile "$dst.part"
    Move-Item -Force "$dst.part" $dst
}

foreach ($name in ($sidecar + $payload)) { Get-Asset $name }

$expected = @{}
foreach ($line in Get-Content (Join-Path $Out 'SHA256SUMS')) {
    if ($line -match '^([0-9a-f]{64})\s+\*?(.+)$') { $expected[$Matches[2].Trim()] = $Matches[1] }
}

$bad = 0
foreach ($name in $payload) {
    $path = Join-Path $Out $name
    if (-not (Test-Path $path)) { Write-Host "MISSING $name" -ForegroundColor Red; $bad++; continue }
    $actual = (Get-FileHash -Algorithm SHA256 $path).Hash.ToLower()
    $want = $expected[$name]
    if (-not $want) { Write-Host "no checksum for $name" -ForegroundColor Red; $bad++; continue }
    if ($actual -ne $want) {
        Write-Host ("BAD     {0}`n  want {1}`n  got  {2}" -f $name, $want, $actual) -ForegroundColor Red
        $bad++
    } else {
        Write-Host ("ok      {0}" -f $name)
    }
}

if ($bad -ne 0) { throw "$bad problem(s) in $Out - do not push this to the headset" }

Write-Host ""
Write-Host "ok: $Out holds all five files for the headset" -ForegroundColor Green
Write-Host ""
Write-Host "    adb shell mkdir -p /data/local/tmp/q3s"
Write-Host "    adb push $Out/. /data/local/tmp/q3s/"
Write-Host "    adb shell su -c 'sh /data/local/tmp/q3s/bringup.sh stage1'"
