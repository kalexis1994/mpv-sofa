$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
Remove-Item -Force "F:\hrtf\eac3_objmeta_debug.txt" -ErrorAction SilentlyContinue
& "F:\hrtf\ffmpeg-src\ffmpeg_g.exe" -i "F:\hrtf\spatial_raw.eac3" -t 1 -f null NUL 2>&1
Write-Host "---"
if (Test-Path "F:\hrtf\eac3_objmeta_debug.txt") {
    Write-Host "DEBUG FILE FOUND:"
    Get-Content "F:\hrtf\eac3_objmeta_debug.txt"
} else {
    Write-Host "No debug file generated"
}
