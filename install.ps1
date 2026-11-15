# Install / uninstall ED-FSR (dinput8.dll proxy) into the Elite Dangerous game folder.
#   install.ps1 [-GameDir <path to elite-dangerous-odyssey-64>] [-Uninstall]
# Works from the repo (uses build\dinput8.dll) or from the release zip (dinput8.dll next to this script).
param([string]$GameDir = "D:\Epic Games\EliteDangerous\Products\elite-dangerous-odyssey-64", [switch]$Uninstall)
if (-not (Test-Path "$GameDir\EliteDangerous64.exe")) { "EliteDangerous64.exe not found in $GameDir (use -GameDir)"; exit 1 }
if (Get-Process EliteDangerous64 -ErrorAction SilentlyContinue) { "Close the game first"; exit 1 }
if ($Uninstall) { Remove-Item "$GameDir\dinput8.dll" -ErrorAction SilentlyContinue; "ED-FSR removed"; exit 0 }
$src = @("$PSScriptRoot\dinput8.dll", "$PSScriptRoot\build\dinput8.dll") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $src) { "dinput8.dll not found next to this script or in build\"; exit 1 }
Copy-Item $src "$GameDir\dinput8.dll" -Force
"ED-FSR installed from $src"
"Note: the mod reads its settings and FidelityFX DLLs from D:\ed-fsr (edfsr.ini, bin\)."
