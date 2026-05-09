# build.ps1 - Build mpv (libmpv) + host app (mpv-sofa) and stage runtime in dist/
# Usage: .\build.ps1

$ErrorActionPreference = "Stop"

$hrtfRoot    = "F:\hrtf"
$mpvSrc      = "$hrtfRoot\mpv-src"
$patchDir    = "$hrtfRoot\mpv-patches"
$filterDir   = "$mpvSrc\audio\filter"
$mpvBuildDir = "$mpvSrc\build"
$appBuildDir = "$hrtfRoot\build"
$distDir     = "$hrtfRoot\dist"
$msys2Bash   = "C:\msys64\usr\bin\bash.exe"

# ---- Step 1: Copy af_hrtf.c + module headers to mpv source ----
Write-Host "=== Step 1: Copy af_hrtf sources ===" -ForegroundColor Cyan
Copy-Item "$patchDir\af_hrtf.c" "$filterDir\af_hrtf.c" -Force
Get-ChildItem "$patchDir\af_hrtf_*.h" | ForEach-Object {
    Copy-Item $_.FullName "$filterDir\$($_.Name)" -Force
    Write-Host "  $($_.Name)"
}

# ---- Step 2: Build mpv (MSYS2 UCRT64) ----
Write-Host "`n=== Step 2: Build mpv (MSYS2 UCRT64) ===" -ForegroundColor Cyan
$env:MSYSTEM = "UCRT64"
$env:CHERE_INVOKING = "1"

& $msys2Bash --login -c "cd /f/hrtf/mpv-src/build && ninja 2>&1"
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nmpv build FAILED (exit code $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

# ---- Step 3: Build host app (outputs directly to dist/ via CMAKE_RUNTIME_OUTPUT_DIRECTORY) ----
Write-Host "`n=== Step 3: Build host app ===" -ForegroundColor Cyan

& $msys2Bash --login -c "cd /f/hrtf/build && ninja 2>&1"
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nApp build FAILED (exit code $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

# ---- Step 4: Stage runtime DLLs into dist/ ----
# dist/ should end up with: mpv-sofa.exe, assets/, and every DLL libmpv-2.dll
# transitively depends on.  We copy from three sources:
#   1. Our own libmpv build (libmpv-2.dll)
#   2. Our FFmpeg build (avcodec-62.dll, avutil-60.dll, swresample-6.dll, …)
#   3. Our Rust TrueHD FFI (losslesshd_ffi.dll)
#   4. MSYS2 UCRT64 — the rest of the transitive dependency chain.
Write-Host "`n=== Step 4: Stage runtime DLLs into dist/ ===" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

function CopyIfNewer($src, $dst) {
    if (-not (Test-Path $src)) {
        Write-Host "  SKIP (missing): $src" -ForegroundColor Yellow
        return
    }
    $copy = $true
    if (Test-Path $dst) {
        $sInfo = Get-Item $src
        $dInfo = Get-Item $dst
        if ($sInfo.LastWriteTime -le $dInfo.LastWriteTime -and
            $sInfo.Length -eq $dInfo.Length) {
            $copy = $false
        }
    }
    if ($copy) { Copy-Item $src $dst -Force }
}

# Our builds
CopyIfNewer "$mpvBuildDir\libmpv-2.dll" "$distDir\libmpv-2.dll"
CopyIfNewer "$hrtfRoot\rust_hrtf\target\release\losslesshd_ffi.dll" "$distDir\losslesshd_ffi.dll"

# FFmpeg DLLs
Get-ChildItem -Path "$hrtfRoot\ffmpeg-build\bin" -Filter "*.dll" -ErrorAction SilentlyContinue |
    ForEach-Object { CopyIfNewer $_.FullName "$distDir\$($_.Name)" }

# MSYS2 UCRT64 transitive deps
$msys2Dlls = @(
    "libarchive-13.dll","libass-9.dll","libb2-1.dll","libbluray-3.dll",
    "libbrotlicommon.dll","libbrotlidec.dll","libbz2-1.dll","libcaca-0.dll",
    "libcrypto-3-x64.dll","libdovi.dll","libexpat-1.dll","libfontconfig-1.dll",
    "libfreetype-6.dll","libfribidi-0.dll","libgcc_s_seh-1.dll",
    "libglib-2.0-0.dll","libgraphite2.dll","libharfbuzz-0.dll","libiconv-2.dll",
    "libintl-8.dll","libjpeg-8.dll","liblcms2-2.dll","liblz4.dll","liblzma-5.dll",
    "libmysofa.dll","libpcre2-8-0.dll","libplacebo-351.dll","libpng16-16.dll",
    "libshaderc_shared.dll","libspirv-cross-c-shared.dll","libstdc++-6.dll",
    "libunibreak-6.dll","libva.dll","libva_win32.dll","libwinpthread-1.dll",
    "libxml2-16.dll","libzimg-2.dll","libzstd.dll","zlib1.dll"
)
foreach ($dll in $msys2Dlls) {
    CopyIfNewer "C:\msys64\ucrt64\bin\$dll" "$distDir\$dll"
}

# (Assets are copied by the POST_BUILD step in CMakeLists.txt.)

Write-Host "`n=== Done ===" -ForegroundColor Green
Write-Host "Run:  $distDir\mpv-sofa.exe" -ForegroundColor Green
