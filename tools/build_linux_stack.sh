#!/usr/bin/env bash
# Build the patched FFmpeg + mpv stack on Linux.
#
# The authoritative recipe is .github/workflows/build.yml (apply_patches.sh is
# stale — it predates objcoding_qmf.c and the pffft split).  This script is
# that recipe with the Windows-isms removed, plus three fixups the Windows CI
# never needs:
#
#   1. libavcodec/Makefile never lists mlp_atmos_coeff.o, so the translation
#      unit defining the exported globals is simply never compiled.  On the
#      MinGW side the link happened to succeed because the patch's users are
#      resolved lazily through the import library; on ELF it is a hard
#      undefined reference.
#   2. libavcodec/libavcodec.v ends in "local: *", which hides every symbol
#      that isn't av*.  The three spatial globals would be invisible to
#      dlsym() even once they are compiled in.
#   3. mlpdsp.h is missing a prototype the patch's own caller needs, and
#      FFmpeg builds with -Werror=missing-prototypes.  (This one the CI does
#      handle; it is reproduced here.)
#
# Usage: tools/build_linux_stack.sh [ffmpeg|mpv|all]   (default: all)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FFMPEG_SRC="$ROOT/ffmpeg-src"
FFMPEG_BUILD="$ROOT/ffmpeg-build"
MPV_SRC="$ROOT/mpv-src"
MPV_COMMIT=9ccd8899ff28c4f6bdd113026d307af3d46133c5
JOBS="$(nproc)"

step() { printf '\n\033[1;36m=== %s ===\033[0m\n' "$*"; }
die()  { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

build_ffmpeg() {
    step "FFmpeg n8.0"
    if [ ! -d "$FFMPEG_SRC" ]; then
        git clone --depth 1 --branch n8.0 https://github.com/FFmpeg/FFmpeg.git "$FFMPEG_SRC"
    fi
    cd "$FFMPEG_SRC"

    if [ ! -f .spatial-patched ]; then
        step "applying spatial audio patch"
        local p="$ROOT/ffmpeg-patches/ffmpeg-spatial-audio.patch"
        local tmp; tmp="$(mktemp)"
        # Strip CRLF, then keep everything from the first diff header onward.
        #
        # NOT the CI's `sed '1,/^diff --git/{/^diff --git/!d}'`: a range whose
        # end address is searched from the line *after* the start means that
        # when line 1 is already a diff header — which it is — the range runs
        # to the *second* header instead, deleting the whole first file's
        # section.  That silently dropped every mlpdec.c hunk, i.e. all of the
        # Atmos work, leaving only mlpdsp.c patched and no error anywhere.
        sed 's/\r$//' "$p" | sed -n '/^diff --git/,$p' > "$tmp"

        # Recompute every hunk header from its own body.  The committed patch
        # has headers whose declared line counts disagree with what follows —
        # the very first one says "@@ -27,6 +27,7 @@" while adding two lines
        # to six of context, so the new count should be 8.  git apply refuses
        # outright and GNU patch calls it a malformed patch, which is why this
        # file has to be normalised rather than just fuzz-matched.
        python3 - "$tmp" <<'PY'
import re, sys
path = sys.argv[1]
lines = open(path, encoding="utf-8", errors="surrogateescape").read().splitlines(True)
out, i, fixed = [], 0, 0
hdr = re.compile(r'^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@(.*)$')
while i < len(lines):
    m = hdr.match(lines[i])
    if not m:
        out.append(lines[i]); i += 1; continue
    start, j, old, new = i + 1, i + 1, 0, 0
    while j < len(lines):
        l = lines[j]
        if l.startswith('@@') or l.startswith('diff --git') or l.startswith('--- ') or l.startswith('+++ '):
            break
        if l.startswith('\\'):            # "\ No newline at end of file"
            j += 1; continue
        c = l[0] if l else ' '
        if c == ' ': old += 1; new += 1
        elif c == '-': old += 1
        elif c == '+': new += 1
        else: break
        j += 1
    rebuilt = f"@@ -{m.group(1)},{old} +{m.group(2)},{new} @@{m.group(3)}\n"
    if rebuilt != lines[i]:
        fixed += 1
    out.append(rebuilt)
    out.extend(lines[start:j])
    i = j
open(path, "w", encoding="utf-8", errors="surrogateescape").write("".join(out))
print(f"  normalised {fixed} malformed hunk header(s)")
PY
        # GNU patch rather than git apply: even normalised, the context in a
        # few hunks needs fuzz-matching against n8.0.
        patch -p1 --no-backup-if-mismatch < "$tmp"
        rm -f "$tmp"
        touch .spatial-patched
    fi

    # Guard against the failure above ever recurring quietly.
    for f in libavcodec/mlpdec.c libavcodec/mlpdsp.c; do
        grep -q 'spatial_ext_coeff\|ff_mlp_rematrix_interp_channel' "$f" \
            || die "$f did not receive the spatial patch"
    done
    echo "  both mlpdec.c and mlpdsp.c carry the patch"

    step "fixup 1/3 — ff_mlp_rematrix_interp_channel prototype"
    python3 - <<'PY'
import re, sys
path = "libavcodec/mlpdsp.h"
src  = open(path, encoding="utf-8").read()
if "ff_mlp_rematrix_interp_channel" in src:
    print("  already declared")
else:
    proto = ("void ff_mlp_rematrix_interp_channel(int32_t *samples, const int32_t *seed_coeffs,\n"
             "    const int32_t *delta_coeffs, const uint8_t *bypassed_lsbs, const int8_t *noise_buffer,\n"
             "    int index, unsigned int sample_offset, unsigned int samples_per_au, unsigned int dest_ch,\n"
             "    uint16_t blockpos, unsigned int maxchan, int matrix_noise_shift,\n"
             "    int access_unit_size_pow2, int32_t mask);\n\n")
    endif = "#endif /* AVCODEC_MLPDSP_H */"
    if endif not in src:
        sys.exit("could not find the include guard in mlpdsp.h")
    open(path, "w", encoding="utf-8").write(src.replace(endif, proto + endif))
    print("  prototype injected")
PY

    step "copying the spatial export TU"
    cp -f "$ROOT/ffmpeg-patches/mlp_atmos_coeff.c"   libavcodec/mlp_atmos_coeff.c
    cp -f "$ROOT/ffmpeg-patches/spatial_ext_coeff.h" libavcodec/spatial_ext_coeff.h

    step "fixup 2/3 — register mlp_atmos_coeff.o in libavcodec/Makefile"
    python3 - <<'PY'
import re
path = "libavcodec/Makefile"
src  = open(path, encoding="utf-8").read()
if "mlp_atmos_coeff.o" in src:
    print("  already registered")
else:
    # Append to the existing OBJS lines for both decoders that read the globals,
    # rather than adding our own line, so the object is pulled in exactly when
    # one of those decoders is enabled.
    n = 0
    def add(m):
        global n
        n += 1
        return m.group(0).rstrip() + " mlp_atmos_coeff.o\n"
    src, _ = re.subn(r'^OBJS-\$\(CONFIG_(?:MLP|TRUEHD)_DECODER\)\s*\+=.*\n',
                     add, src, flags=re.M)
    if n == 0:
        raise SystemExit("could not find the MLP/TRUEHD OBJS lines")
    open(path, "w", encoding="utf-8").write(src)
    print(f"  appended to {n} OBJS line(s)")
PY

    step "fixup 3/3 — export the spatial globals from libavcodec.v"
    python3 - <<'PY'
path = "libavcodec/libavcodec.v"
src  = open(path, encoding="utf-8").read()
if "g_spatial_ext_coeff" in src:
    print("  already exported")
else:
    # The script is "{ global: av*; ...; local: *; };" — anything not listed
    # before "local: *" is hidden, dlsym() included.
    if "local:" not in src:
        raise SystemExit("unexpected version script layout")
    src = src.replace("local:",
                      "g_spatial_ext_coeff; g_spatial_ext_objmeta; g_objcoding_data;\n        local:",
                      1)
    open(path, "w", encoding="utf-8").write(src)
    print("  three globals added to the global: list")
PY

    if [ ! -f config.h ]; then
        step "configure"
        ./configure \
            --prefix="$FFMPEG_BUILD" \
            --arch=x86_64 \
            --disable-debug --disable-doc \
            --enable-shared --enable-gpl --enable-version3 \
            --enable-runtime-cpudetect --enable-zlib --enable-iconv \
            --enable-vaapi --enable-vulkan
    fi

    step "make -j$JOBS"
    make -j"$JOBS"
    make install

    step "verifying the exported globals"
    local so; so="$(ls "$FFMPEG_BUILD"/lib/libavcodec.so.* 2>/dev/null | head -1)"
    [ -n "$so" ] || die "libavcodec.so not found in $FFMPEG_BUILD/lib"
    nm -D --defined-only "$so" | grep -E 'g_spatial_ext_coeff|g_spatial_ext_objmeta|g_objcoding_data' \
        || die "the spatial globals are NOT exported — fixup 2 or 3 did not take"
    echo "  all three globals are exported"
    cd "$ROOT"
}

build_mpv() {
    step "mpv @ ${MPV_COMMIT:0:8}"
    if [ ! -d "$MPV_SRC/.git" ]; then
        mkdir -p "$MPV_SRC"
        git -C "$MPV_SRC" init -q
        git -C "$MPV_SRC" fetch --depth 1 https://github.com/mpv-player/mpv.git "$MPV_COMMIT"
        git -C "$MPV_SRC" checkout -q FETCH_HEAD
    fi
    cd "$MPV_SRC"

    step "copying the HRTF filter and its dependencies"
    cp -f "$ROOT"/mpv-patches/af_hrtf.c           audio/filter/af_hrtf.c
    cp -f "$ROOT"/mpv-patches/af_hrtf_*.h         audio/filter/
    cp -f "$ROOT"/mpv-patches/objcoding_qmf.c     audio/filter/objcoding_qmf.c
    cp -f "$ROOT"/mpv-patches/objcoding_qmf.h     audio/filter/objcoding_qmf.h
    cp -f "$ROOT"/mpv-patches/spatial_ext_coeff.h audio/filter/spatial_ext_coeff.h
    cp -f "$ROOT"/external/pffft/pffft.c          audio/filter/pffft.c
    cp -f "$ROOT"/external/pffft/pffft.h          audio/filter/pffft.h
    cp -f "$ROOT"/external/pffft/pffft_common.c   audio/filter/pffft_common.c
    cp -f "$ROOT"/external/pffft/pffft_priv_impl.h audio/filter/pffft_priv_impl.h
    mkdir -p audio/filter/simd
    cp -f "$ROOT"/external/pffft/simd/*.h         audio/filter/simd/

    step "registering the filter"
    grep -q 'af_hrtf;' filters/user_filters.h || \
        sed -i '/extern const struct mp_user_filter_entry af_drop;/a extern const struct mp_user_filter_entry af_hrtf;' filters/user_filters.h
    grep -q '&af_hrtf,' filters/user_filters.c || \
        sed -i '/&af_drop,/a\    \&af_hrtf,' filters/user_filters.c
    grep -q "option('hrtf'" meson.options || \
        echo "option('hrtf', type: 'feature', value: 'auto', description: 'HRTF binaural audio filter (libmysofa)')" >> meson.options

    python3 - <<'PY'
path = "meson.build"
src  = open(path, encoding="utf-8").read()
if "features += {'hrtf'" in src:
    print("  meson.build already has the HRTF block")
else:
    lines = src.splitlines(keepends=True)
    idx = next((i for i, l in enumerate(lines) if 'af_rubberband.c' in l), None)
    if idx is None:
        raise SystemExit("could not find af_rubberband.c in meson.build")
    end = next((j for j in range(idx, len(lines)) if lines[j].strip() == 'endif'), None)
    if end is None:
        raise SystemExit("could not find the endif after af_rubberband.c")
    block = ("\n# HRTF binaural spatialization filter\n"
             "libmysofa = dependency('libmysofa', required: get_option('hrtf'))\n"
             "features += {'hrtf': libmysofa.found()}\n"
             "if features['hrtf']\n"
             "    dependencies += libmysofa\n"
             "    sources += files('audio/filter/af_hrtf.c', 'audio/filter/pffft.c',\n"
             "                     'audio/filter/pffft_common.c', 'audio/filter/objcoding_qmf.c')\n"
             "endif\n")
    lines.insert(end + 1, block)
    open(path, "w", encoding="utf-8").write("".join(lines))
    print("  HRTF block inserted into meson.build")
PY

    step "meson setup + compile"
    # The patched FFmpeg must win over any system one, both now and at runtime.
    export PKG_CONFIG_PATH="$FFMPEG_BUILD/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    [ -d build ] || meson setup build --buildtype=release -Dlibmpv=true -Dhrtf=enabled \
        -Dc_link_args="-Wl,-rpath,$FFMPEG_BUILD/lib"
    meson compile -C build

    step "verifying"
    ldd build/libmpv.so.2 | grep -E 'libavcodec' || true
    LD_LIBRARY_PATH="$FFMPEG_BUILD/lib" ./build/mpv --af=help 2>&1 | grep -i hrtf \
        && echo "  af_hrtf is registered" \
        || die "af_hrtf did not register in the mpv build"
    cd "$ROOT"
}

case "${1:-all}" in
    ffmpeg) build_ffmpeg ;;
    mpv)    build_mpv ;;
    all)    build_ffmpeg; build_mpv ;;
    *)      die "usage: $0 [ffmpeg|mpv|all]" ;;
esac

step "done"
