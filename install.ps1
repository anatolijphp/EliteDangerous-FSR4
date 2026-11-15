# Manual install / uninstall of ED-FSR into the Elite Dangerous game folder (alternative to ED-FSR-Setup-*.exe).
#   install.ps1 [-GameDir <folder containing EliteDangerous64.exe>] [-Uninstall]
# Run it from the unpacked release zip / CI artifact: dinput8.dll and the edfsr\ folder must be next to this script.
# Result: <GameDir>\dinput8.dll and <GameDir>\edfsr\ (edfsr.ini, bin\*.dll, logs\).
param([string]$GameDir = "D:\Epic Games\EliteDangerous\Products\elite-dangerous-odyssey-64", [switch]$Uninstall)
$ErrorActionPreference = "Stop"
if (-not (Test-Path "$GameDir\EliteDangerous64.exe")) { "EliteDangerous64.exe not found in $GameDir (use -GameDir)"; exit 1 }
if (Get-Process EliteDangerous64 -ErrorAction SilentlyContinue) { "Close the game first"; exit 1 }

$dll = Join-Path $GameDir "dinput8.dll"
$bak = "$dll.edfsr-backup"
$root = Join-Path $GameDir "edfsr"
function Test-OurDll($path) { (Test-Path $path) -and ([IO.File]::ReadAllText($path, [Text.Encoding]::ASCII)).Contains("ED-FSR") }

if ($Uninstall) {
    if (Test-OurDll $dll) { Remove-Item $dll -Force }
    if ((Test-Path $bak) -and -not (Test-Path $dll)) { Move-Item $bak $dll; "Restored the previous dinput8.dll from backup" }
    if (Test-Path $root) { Remove-Item $root -Recurse -Force }
    "ED-FSR removed"
    exit 0
}

$srcDll = Join-Path $PSScriptRoot "dinput8.dll"
$srcRoot = Join-Path $PSScriptRoot "edfsr"
if (-not (Test-Path $srcDll) -or -not (Test-Path "$srcRoot\bin")) { "dinput8.dll and edfsr\bin\ not found next to this script (run it from the unpacked package)"; exit 1 }

if ((Test-Path $dll) -and -not (Test-OurDll $dll)) {
    Copy-Item $dll $bak -Force
    "Existing dinput8.dll is not ED-FSR (another mod?); backed up to $bak (restored by -Uninstall)"
}
Copy-Item $srcDll $dll -Force
New-Item -ItemType Directory -Force "$root\bin", "$root\logs" | Out-Null
Copy-Item "$srcRoot\bin\*.dll" "$root\bin" -Force
# keep existing settings
if (-not (Test-Path "$root\edfsr.ini")) { Copy-Item "$srcRoot\edfsr.ini" "$root\edfsr.ini" }
if (Test-Path "$PSScriptRoot\README.md") { Copy-Item "$PSScriptRoot\README.md" "$root\README.md" -Force }
"ED-FSR installed into $GameDir"
