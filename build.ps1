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

# ---- Step 0: Apply local patches to vendored submodules ----
# We carry small modifications to ImGuiFileDialog (e.g. single-click to
# activate places in the sidebar) outside of its repo so the submodule
# stays clean.  --reverse --check tells us if the patch is already on top
# of the working tree, in which case we skip re-applying it.
$ifdPatch = "$hrtfRoot\external\ImGuiFileDialog.patch"
$ifdRepo  = "$hrtfRoot\external\ImGuiFileDialog"
if ((Test-Path $ifdPatch) -and (Test-Path $ifdRepo)) {
    Write-Host "=== Step 0: Apply ImGuiFileDialog patches ===" -ForegroundColor Cyan
    # git apply emits to stderr on the check path even when behaviour is
    # expected, and ErrorActionPreference="Stop" would treat it as fatal.
    # Save/restore the preference around the git calls.
    $savedEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & git -C $ifdRepo apply --reverse --check $ifdPatch *> $null
    $reverseOk = ($LASTEXITCODE -eq 0)
    if ($reverseOk) {
        Write-Host "  patch already applied"
    } else {
        & git -C $ifdRepo apply $ifdPatch *> $null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "  applied $($ifdPatch | Split-Path -Leaf)"
        } else {
            Write-Host "  WARN: patch failed; submodule may be out of sync" -ForegroundColor Yellow
        }
    }
    $ErrorActionPreference = $savedEAP
}

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

# MSYS2 UCRT64 transitive deps — resolved RECURSIVELY from the actual
# binaries with objdump, not a hardcoded list.  A hardcoded list rots the
# moment an MSYS2 package bumps its soname (e.g. libplacebo-351 -> -360,
# libunibreak-6 -> -7), silently shipping a broken dist that only fails
# when launched outside an MSYS2 PATH.  Walking the import tables can't
# drift.  Any import not found under ucrt64/bin is a Windows system DLL
# and is skipped.
$ucrtBin = "C:\msys64\ucrt64\bin"
$objdump = "$ucrtBin\objdump.exe"
if (Test-Path $objdump) {
    $resolved = @{}
    $queue = [System.Collections.Queue]::new()
    # Seed with everything already staged in dist (exe + our DLLs).
    # NOTE: -Include needs a wildcard in -Path (or -Recurse) to match.
    Get-ChildItem -Path "$distDir\*" -Include "*.exe","*.dll" -File |
        ForEach-Object { $queue.Enqueue($_.FullName) }

    while ($queue.Count -gt 0) {
        $bin = $queue.Dequeue()
        $imports = & $objdump -p $bin 2>$null |
            Select-String -Pattern '^\s*DLL Name:\s*(.+)$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
        foreach ($dll in $imports) {
            if ($resolved.ContainsKey($dll)) { continue }
            $src = Join-Path $ucrtBin $dll
            if (Test-Path $src) {
                $resolved[$dll] = $true
                CopyIfNewer $src "$distDir\$dll"
                $queue.Enqueue($src)   # walk its deps too
            }
        }
    }
    Write-Host "  bundled $($resolved.Count) UCRT64 dependency DLLs"
} else {
    Write-Warning "objdump not found at $objdump - cannot resolve runtime DLLs"
}

# (Assets are copied by the POST_BUILD step in CMakeLists.txt.)

# ---- Step 5: Purge runtime artefacts that mpv-sofa drops into dist/ ----
# When the app is launched from dist/ it writes its working files (ImGui
# layout cache, debug logs, the truehd loader log) right next to the
# binaries.  Cleaning them at the end of every build keeps dist/ shippable.
# `mpv-sofa.ini` (the persisted user settings) is preserved deliberately.
Write-Host "`n=== Step 5: Purge runtime artefacts from dist/ ===" -ForegroundColor Cyan
$strayPatterns = @("*.log", "*.txt", "*.ini", "*.wav")
$keepFiles     = @("mpv-sofa.ini")
foreach ($pat in $strayPatterns) {
    Get-ChildItem -Path $distDir -File -Filter $pat -ErrorAction SilentlyContinue |
        Where-Object { $keepFiles -notcontains $_.Name } |
        ForEach-Object {
            Remove-Item $_.FullName -Force
            Write-Host "  removed $($_.Name)"
        }
}

Write-Host "`n=== Done ===" -ForegroundColor Green
Write-Host "Run:  $distDir\mpv-sofa.exe" -ForegroundColor Green
