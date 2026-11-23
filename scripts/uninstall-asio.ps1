# Unregisters the NAM Bridge ASIO driver (needs admin).
# Usage (from the repo root):  powershell -ExecutionPolicy Bypass -File scripts\uninstall-asio.ps1
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Dll = Join-Path $Root "build\asio\Release\NAMBridgeASIO.dll"
if (-not (Test-Path $Dll)) { throw "Not found: $Dll" }
$p = Start-Process -FilePath "$env:WINDIR\System32\regsvr32.exe" -ArgumentList "/u", "/s", "`"$Dll`"" -Verb RunAs -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 /u failed with exit code $($p.ExitCode)" }
Write-Host "Unregistered the NAM Bridge ASIO driver."
