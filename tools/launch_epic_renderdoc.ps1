# Starts Epic Games Launcher under RenderDoc with child-process hooking,
# so Epic -> EDLaunch -> EliteDangerous64.exe gets RenderDoc injected at startup.
# Captures (F12 / PrtScn in game) are saved as D:\ed-fsr\captures\ed_frameN.rdc
$epic = "C:\Program Files\Epic Games\Launcher\Portal\Binaries\Win64\EpicGamesLauncher.exe"
if (Get-Process EpicGamesLauncher -ErrorAction SilentlyContinue) {
    Write-Host "Epic Games Launcher is running. Exit it fully (tray icon -> Exit) and run this script again." -ForegroundColor Yellow
    exit 1
}
New-Item -ItemType Directory -Force D:\ed-fsr\captures | Out-Null
& D:\tools\renderdoc\renderdoccmd.exe capture --opt-hook-children --opt-ref-all-resources `
    -c "D:\ed-fsr\captures\ed" $epic
Write-Host "Epic started under RenderDoc. Launch Elite Dangerous from Epic as usual; press F12 in game to capture."
