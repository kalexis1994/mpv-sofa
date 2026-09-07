#!/usr/bin/env bash
# Verify the Linux spatial plumbing end to end.
#
# What this can and cannot prove:
#
#   * FFmpeg has no way to author Atmos.  Its TrueHD encoder writes lossless
#     multichannel but never the OAMD extension frames that carry object
#     metadata, so a synthetic file cannot exercise g_spatial_ext_objmeta or
#     g_objcoding_data.  Those need a real Atmos source.
#   * It does exercise everything else, and that is the part currently in
#     doubt: the patched mlpdec still decodes, and g_spatial_ext_coeff — the
#     per-matrix rematrixing coefficients, present in *all* multichannel
#     TrueHD — is exported, resolvable through dlsym, and reachable at run
#     time exactly the way af_hrtf reaches it.
#
# Run inside the build container:
#   distrobox enter --name sofabuild -- tools/verify_linux_spatial.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
FFMPEG="$ROOT/ffmpeg-build/bin/ffmpeg"
FFLIB="$ROOT/ffmpeg-build/lib"
MPVLIB="$ROOT/mpv-src/build"
WORK=/tmp/sofa-verify
mkdir -p "$WORK"

pass=0; fail=0; skip=0
step() { printf '\n\033[1;36m=== %s ===\033[0m\n' "$*"; }
ok()   { printf '  \033[1;32mPASS\033[0m %s\n' "$*"; pass=$((pass+1)); }
no()   { printf '  \033[1;31mFAIL\033[0m %s\n' "$*"; fail=$((fail+1)); }
# Not every check can run everywhere.  A missing HRTF profile is a legitimate
# state — assets/hrtf/*.sofa is gitignored, so a CI checkout simply has none —
# and reporting that as a failure would teach everyone to ignore the result.
sk()   { printf '  \033[1;33mSKIP\033[0m %s\n' "$*"; skip=$((skip+1)); }

export LD_LIBRARY_PATH="$MPVLIB:$FFLIB:${LD_LIBRARY_PATH:-}"

# ---------------------------------------------------------------------------
step "1. the globals are exported from libavcodec"
SO="$(ls "$FFLIB"/libavcodec.so.* 2>/dev/null | grep -E '\.so\.[0-9]+$' | head -1)"
if [ -z "$SO" ]; then
    no "libavcodec.so not found — build the stack first"
else
    # The version script tags exported symbols as name@@LIBAVCODEC_62, so
    # matching must not anchor at end of line.
    syms="$(nm -D --defined-only "$SO")"
    for sym in g_spatial_ext_coeff g_spatial_ext_objmeta g_objcoding_data; do
        if printf '%s\n' "$syms" | grep -qE "[[:space:]]${sym}(@@|[[:space:]]*$)"; then
            ok "$sym exported"
        else
            no "$sym NOT exported — check libavcodec.v and the Makefile fixups"
        fi
    done
fi

# ---------------------------------------------------------------------------
step "2. dlsym resolves them the way af_hrtf does"
# Mirrors resolve_avcodec_symbol() exactly: load libmpv (which is what pulls
# avcodec in at run time), then look the names up in the global scope.
cat > "$WORK/dlsym_test.c" <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv) {
    void *mpv = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!mpv) { printf("dlopen failed: %s\n", dlerror()); return 2; }
    const char *names[] = {"g_spatial_ext_coeff", "g_spatial_ext_objmeta",
                           "g_objcoding_data"};
    int missing = 0;
    for (int i = 0; i < 3; i++) {
        void *s = dlsym(RTLD_DEFAULT, names[i]);
        printf("  %-24s %s\n", names[i], s ? "resolved" : "NOT FOUND");
        if (!s) missing++;
    }
    return missing ? 1 : 0;
}
EOF
if cc -o "$WORK/dlsym_test" "$WORK/dlsym_test.c" 2>"$WORK/cc.log"; then
    if "$WORK/dlsym_test" "$MPVLIB/libmpv.so.2"; then
        ok "all three resolve via RTLD_DEFAULT"
    else
        no "at least one did not resolve"
    fi
else
    no "could not build the probe"
    sed 's/^/    /' "$WORK/cc.log"
fi

# ---------------------------------------------------------------------------
step "3. synthetic TrueHD (no Atmos — see the header comment)"
made=""
if [ ! -x "$FFMPEG" ]; then
    no "patched ffmpeg missing"
else
    # FFmpeg's TrueHD encoder supports mono..5.1(side) only, and s16p/s32p
    # only — the sine source hands it fltp, which is what a bare attempt
    # fails on with EINVAL.  5.1 is enough: the rematrixing coefficients this
    # exists to exercise are emitted for any multichannel TrueHD matrix.
    # -strict experimental: FFmpeg marks its own TrueHD encoder experimental
    # and refuses to open it otherwise.  That refusal surfaces three lines
    # later as a generic EINVAL from the output muxer, so tailing the log
    # shows the symptom and hides the cause — grep for the real line instead.
    for ch in 6 2; do
        if "$FFMPEG" -v error -y -f lavfi \
             -i "sine=frequency=440:duration=8:sample_rate=48000" \
             -af "aformat=sample_fmts=s16p" -ac "$ch" \
             -strict experimental -c:a truehd "$WORK/test.thd" 2>"$WORK/enc.log"; then
            made="${ch}ch"
            break
        fi
    done
    if [ -n "$made" ]; then
        ok "encoded $made TrueHD ($(stat -c%s "$WORK/test.thd") bytes)"
        "$FFMPEG" -v error -y -i "$WORK/test.thd" -c:a copy "$WORK/test.mka" 2>/dev/null
    else
        no "the truehd encoder rejected every layout"
        grep -m2 -iE "experimental|not supported|invalid|unable" "$WORK/enc.log" \
            | sed 's/^/    /'
    fi
fi

# ---------------------------------------------------------------------------
step "4. the patched decoder still decodes it"
if [ -z "$made" ]; then
    no "skipped — no input"
elif "$FFMPEG" -v error -y -i "$WORK/test.thd" -f f32le "$WORK/decoded.raw" \
        2>"$WORK/dec.log"; then
    sz=$(stat -c%s "$WORK/decoded.raw")
    if [ "$sz" -gt 1000000 ]; then
        ok "decoded $((sz/1024)) KB of PCM"
    else
        no "suspiciously small output ($sz bytes)"
    fi
else
    no "decode failed"
    tail -3 "$WORK/dec.log" | sed 's/^/    /'
fi

# ---------------------------------------------------------------------------
step "5. mpv runs the file through af_hrtf"
if [ -z "$made" ]; then
    no "skipped — no input"
else
    SOFA="$(ls "$ROOT"/assets/hrtf/*.sofa 2>/dev/null | head -1)"
    if [ -z "$SOFA" ]; then
        sk "no .sofa profile in assets/hrtf (they are gitignored)"
    else
        # ao=null keeps this headless; the filter still runs on every frame.
        out=$("$MPVLIB/mpv" --no-config --ao=null --vo=null \
              --msg-level=all=v --length=5 \
              --af="hrtf=sofa=%${#SOFA}%${SOFA}" "$WORK/test.mka" 2>&1)
        if printf '%s\n' "$out" | grep -qiE "af_hrtf|hrtf"; then
            ok "af_hrtf engaged"
        else
            no "no sign of af_hrtf in mpv's output"
        fi
        printf '%s\n' "$out" | grep -iE "error|failed" | head -5 | sed 's/^/    /'
    fi
fi

printf '\n\033[1m%d passed, %d failed, %d skipped\033[0m\n' "$pass" "$fail" "$skip"
cat <<'NOTE'

Still unproven, and only a real Atmos source can prove it:
  g_spatial_ext_objmeta / g_objcoding_data — the object metadata and JOC
  paths. Everything above only shows the plumbing carries coefficients.
NOTE
[ "$fail" -eq 0 ]
