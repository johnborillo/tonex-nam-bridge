# Fetches third-party sources at the exact commits this project was tested with (Windows).
# Usage (PowerShell, from the repo root):  powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
# The ASIO SDK is not fetched here: download it from Steinberg (see docs/WINDOWS_ASIO.md) and
# extract it to deps\asiosdk.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Deps = Join-Path $Root "deps"
New-Item -ItemType Directory -Force -Path $Deps | Out-Null

function Fetch($Url, $Dir, $Commit) {
    $Path = Join-Path $Deps $Dir
    if (-not (Test-Path $Path)) { git clone --recurse-submodules $Url $Path }
    git -C $Path fetch --quiet origin
    git -C $Path checkout --quiet $Commit
    git -C $Path submodule update --init --recursive --quiet
}

# MIT. Same commit as the macOS build, where output was verified bit-identical to NAM's renderer.
Fetch "https://github.com/sdatkinson/NeuralAmpModelerCore.git" "NeuralAmpModelerCore" "d3189ed482ded880c128bf312941266c4fa5ceec"

if (-not (Test-Path (Join-Path $Deps "asiosdk\common\asio.h"))) {
    Write-Host "ASIO SDK missing: extract Steinberg's ASIO SDK so that deps\asiosdk\common\asio.h exists."
}
Write-Host "deps ready"
