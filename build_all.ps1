$ErrorActionPreference = "Stop"

$hrtfRoot    = "F:\hrtf"
$msys2Bash   = "C:\msys64\usr\bin\bash.exe"

# ---- Step 1: Install FFmpeg DLLs ----
Write-Host "=== Step 1: Install FFmpeg ===" -ForegroundColor Cyan
$env:MSYSTEM = "UCRT64"
$env:CHERE_INVOKING = "1"

& $msys2Bash --login -c 'cd /f/hrtf/ffmpeg-src && make install 2>&1 | tail -20'

# Copy avcodec-62.dll to build\bin
$ffmpegDlls = Get-ChildItem "F:\hrtf\ffmpeg-build\bin\*.dll" -ErrorAction SilentlyContinue
foreach ($dll in $ffmpegDlls) {
    $dest = Join-Path "$hrtfRoot\build\bin" $dll.Name
    Copy-Item $dll.FullName $dest -Force
    Write-Host "  Copied $($dll.Name)"
}

# ---- Step 2: Copy patches to mpv-src ----
Write-Host "`n=== Step 2: Copy patches ===" -ForegroundColor Cyan
Copy-Item "$hrtfRoot\mpv-patches\af_hrtf.c"        "$hrtfRoot\mpv-src\audio\filter\af_hrtf.c" -Force
Copy-Item "$hrtfRoot\mpv-patches\objcoding_qmf.c"        "$hrtfRoot\mpv-src\audio\filter\objcoding_qmf.c" -Force
Copy-Item "$hrtfRoot\mpv-patches\objcoding_qmf.h"        "$hrtfRoot\mpv-src\audio\filter\objcoding_qmf.h" -Force
Copy-Item "$hrtfRoot\mpv-patches\spatial_ext_coeff.h" "$hrtfRoot\mpv-src\audio\filter\spatial_ext_coeff.h" -Force
Write-Host "  Copied af_hrtf.c, objcoding_qmf.c, objcoding_qmf.h, spatial_ext_coeff.h"

# ---- Step 3: Build mpv ----
Write-Host "`n=== Step 3: Build mpv ===" -ForegroundColor Cyan
& $msys2Bash --login -c 'cd /f/hrtf/mpv-src/build && ninja 2>&1'
if ($LASTEXITCODE -ne 0) {
    Write-Host "mpv build FAILED" -ForegroundColor Red
    exit 1
}

# Copy libmpv-2.dll
Copy-Item "$hrtfRoot\mpv-src\build\libmpv-2.dll" "$hrtfRoot\build\bin\libmpv-2.dll" -Force
Write-Host "  Copied libmpv-2.dll"

# ---- Step 4: Build host app ----
Write-Host "`n=== Step 4: Build host app ===" -ForegroundColor Cyan
& $msys2Bash --login -c 'cd /f/hrtf/build && ninja 2>&1'
if ($LASTEXITCODE -ne 0) {
    Write-Host "App build FAILED" -ForegroundColor Red
    exit 1
}

Write-Host "`n=== BUILD COMPLETE ===" -ForegroundColor Green
Write-Host "Run: F:\hrtf\build\bin\mpv-sofa.exe"
