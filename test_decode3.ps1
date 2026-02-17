# Copy MinGW runtime DLLs to ffmpeg-src
$mingw = "C:\msys64\mingw64\bin"
$dest = "F:\hrtf\ffmpeg-src"
$dlls = @(
    "libgcc_s_seh-1.dll",
    "libwinpthread-1.dll",
    "libstdc++-6.dll",
    "zlib1.dll"
)
foreach ($d in $dlls) {
    $src = Join-Path $mingw $d
    if (Test-Path $src) { Copy-Item $src $dest -Force }
}

Set-Location "F:\hrtf\ffmpeg-src"
Remove-Item -Force "F:\hrtf\eac3_objmeta_debug.txt" -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath ".\ffmpeg_g.exe" -ArgumentList "-i","F:\hrtf\spatial_raw.eac3","-t","1","-f","null","NUL" -NoNewWindow -Wait -PassThru -RedirectStandardError "F:\hrtf\ffmpeg_stderr.txt"
Write-Host "Exit code: $($proc.ExitCode)"

if (Test-Path "F:\hrtf\ffmpeg_stderr.txt") {
    Get-Content "F:\hrtf\ffmpeg_stderr.txt" | Select-Object -Last 15
}

# Check debug file in both locations
foreach ($path in @("F:\hrtf\eac3_objmeta_debug.txt", "F:\hrtf\ffmpeg-src\eac3_objmeta_debug.txt")) {
    if (Test-Path $path) {
        Write-Host "`n=== DEBUG FILE: $path ==="
        Get-Content $path | Select-Object -First 30
    }
}
