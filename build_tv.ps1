# build_tv.ps1 — package the HaloSound webOS TV app into dist\ as a
# sideloadable .ipk.  Separate from build.ps1 (which builds the Windows
# player + server): this target is the TV client, built with the webOS
# CLI (ares-package).  Install it on an LG TV with:
#   ares-install --device tv dist\com.halosound.app_<ver>_all.ipk
#
# Requires the webOS CLI (@webos-tools/cli) on PATH and the SIMD/scalar
# WASM already built (halosound\build.sh wasm).

$ErrorActionPreference = "Stop"
$root   = Split-Path -Parent $MyInvocation.MyCommand.Path
$appDir = Join-Path $root "halosound\app"
$dist   = Join-Path $root "dist"

if (-not (Get-Command ares-package -ErrorAction SilentlyContinue)) {
    Write-Error "ares-package not found. Install the webOS CLI: npm i -g @webos-tools/cli"
    exit 1
}

# The Emscripten glue trips ares' minifier; --no-minify ships it verbatim.
Write-Host "=== Packaging HaloSound TV app -> dist\ ===" -ForegroundColor Cyan
New-Item -ItemType Directory -Force $dist | Out-Null
ares-package $appDir --no-minify --outdir $dist
if ($LASTEXITCODE -ne 0) { Write-Error "ares-package failed"; exit 1 }

$ipk = Get-ChildItem "$dist\com.halosound.app_*_all.ipk" | Select-Object -Last 1
Write-Host "`n=== Done ===" -ForegroundColor Green
Write-Host "Sideload:  ares-install --device tv `"$($ipk.FullName)`"" -ForegroundColor Green
