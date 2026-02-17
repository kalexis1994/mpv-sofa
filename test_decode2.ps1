$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
Set-Location "F:\hrtf"
$proc = Start-Process -FilePath "F:\hrtf\ffmpeg-src\ffmpeg_g.exe" -ArgumentList "-i","F:\hrtf\spatial_raw.eac3","-t","1","-f","null","NUL" -NoNewWindow -Wait -PassThru -RedirectStandardError "F:\hrtf\ffmpeg_stderr.txt"
Write-Host "Exit code: $($proc.ExitCode)"
if (Test-Path "F:\hrtf\ffmpeg_stderr.txt") {
    Get-Content "F:\hrtf\ffmpeg_stderr.txt" | Select-Object -Last 20
}
Write-Host "---"
if (Test-Path "F:\hrtf\eac3_objmeta_debug.txt") {
    Write-Host "DEBUG FILE FOUND:"
    Get-Content "F:\hrtf\eac3_objmeta_debug.txt" | Select-Object -First 30
} else {
    Write-Host "No debug file generated"
}
