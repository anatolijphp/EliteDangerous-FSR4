# Analyze every .rdc in D:\ed-fsr\captures that has no *_analysis folder yet (or -Force).
param([switch]$Force)
$rd = "D:\tools\renderdoc\qrenderdoc.exe"
Get-ChildItem D:\ed-fsr\captures -Filter *.rdc | ForEach-Object {
    $out = Join-Path $_.DirectoryName ($_.BaseName + "_analysis")
    if ((Test-Path "$out\summary.txt") -and -not $Force) { "skip $($_.Name)"; return }
    $env:EDFSR_CAPTURE = $_.FullName; $env:EDFSR_OUT = $out
    "analyzing $($_.Name) ..."
    $p = Start-Process $rd -ArgumentList '--py', 'D:\ed-fsr\tools\analyze_capture.py' -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(1800000)) { $p.Kill(); "  TIMEOUT" }
    if (Test-Path "$out\error.txt") { "  ERROR -> $out\error.txt" } else { "  ok -> $out\summary.txt" }
}
