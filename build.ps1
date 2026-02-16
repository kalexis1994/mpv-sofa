# build.ps1 - Build everything: mpv (libmpv) + host app (mpv-sofa)
# Usage: .\build.ps1

$ErrorActionPreference = "Stop"

$hrtfRoot    = "F:\hrtf"
$mpvSrc      = "$hrtfRoot\mpv-src"
$patchFile   = "$hrtfRoot\mpv-patches\af_hrtf.c"
$destFile    = "$mpvSrc\audio\filter\af_hrtf.c"
$mpvBuildDir = "$mpvSrc\build"
$appBuildDir = "$hrtfRoot\build"
$msys2Bash   = "C:\msys64\usr\bin\bash.exe"
$dllSrc      = "$mpvBuildDir\libmpv-2.dll"
$dllDest     = "$appBuildDir\bin\libmpv-2.dll"

# ---- Step 1: Copy af_hrtf.c to mpv source ----
Write-Host "=== Step 1: Copy af_hrtf.c ===" -ForegroundColor Cyan
Copy-Item $patchFile $destFile -Force
Write-Host "Copied $patchFile -> $destFile"

# ---- Step 2: Build mpv (MSYS2 UCRT64) ----
Write-Host "`n=== Step 2: Build mpv (MSYS2 UCRT64) ===" -ForegroundColor Cyan
$env:MSYSTEM = "UCRT64"
$env:CHERE_INVOKING = "1"

& $msys2Bash --login -c "cd /f/hrtf/mpv-src/build && ninja 2>&1"
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nmpv build FAILED (exit code $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

# ---- Step 3: Copy libmpv-2.dll ----
Write-Host "`n=== Step 3: Copy libmpv-2.dll ===" -ForegroundColor Cyan
if (Test-Path $dllSrc) {
    Copy-Item $dllSrc $dllDest -Force
    Write-Host "Copied $dllSrc -> $dllDest"
} else {
    Write-Host "WARNING: $dllSrc not found, skipping copy" -ForegroundColor Yellow
}

# ---- Step 4: Build host app (CMake + Ninja via MSYS2) ----
Write-Host "`n=== Step 4: Build host app ===" -ForegroundColor Cyan

& $msys2Bash --login -c "cd /f/hrtf/build && ninja 2>&1"
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nApp build FAILED (exit code $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "`n=== Done ===" -ForegroundColor Green
