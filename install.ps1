# Install / uninstall ED-FSR (dinput8.dll proxy) into the game folder
param([switch]$Uninstall)
$g = "D:\Epic Games\EliteDangerous\Products\elite-dangerous-odyssey-64"
if (Get-Process EliteDangerous64 -ErrorAction SilentlyContinue) { "Close the game first"; exit 1 }
if ($Uninstall) { Remove-Item "$g\dinput8.dll" -ErrorAction SilentlyContinue; "ED-FSR removed" }
else { Copy-Item D:\ed-fsr\build\dinput8.dll "$g\dinput8.dll" -Force; "ED-FSR installed" }
