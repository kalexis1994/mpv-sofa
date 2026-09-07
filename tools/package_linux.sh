#!/usr/bin/env bash
# Assemble a self-contained Linux tree that runs on the SteamOS host.
#
# Deliberately NOT an AppImage.  Steam's reaper tears down stray processes in
# the cgroup when a title exits, which kills an AppImage's squashfuse mount;
# the app then can't read its own resources and dies on the next asset load.
# A plain directory with $ORIGIN rpaths has no such moving parts.
#
# What ships, and what doesn't:
#
#   * libmpv and the FFmpeg libraries ship, because they are ours — patched
#     with af_hrtf and the spatial coefficient exports, and no system copy
#     could stand in for them.
#   * libplacebo does NOT ship.  It was built here only so mpv could link a
#     matching soname; the host has libplacebo.so.351 and its own copy pulls
#     in the host's libshaderc_shared, which is the name Arch uses.  Shipping
#     ours would drag Debian's differently-named shaderc along with it.
#   * libjpeg does ship: Debian builds it as libjpeg.so.62 where Arch uses
#     libjpeg.so.8, so there is no host copy under the name mpv asks for.
#
# Everything else (libass, glfw, lcms2, vulkan, wayland, X11) has identical
# sonames on both sides and resolves from the system.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/dist-linux}"
MPV_BUILD="$ROOT/mpv-src/build"
FFMPEG_LIB="$ROOT/ffmpeg-build/lib"

step() { printf '\n\033[1;36m=== %s ===\033[0m\n' "$*"; }
die()  { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

command -v patchelf >/dev/null || die "patchelf is required (apt-get install patchelf)"
[ -x "$ROOT/dist/mpv-sofa" ] || die "build the app first — dist/mpv-sofa is missing"

step "staging into $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/lib"
cp -a "$ROOT/dist/mpv-sofa" "$OUT/"
cp -a "$ROOT/assets" "$OUT/assets"

step "copying the libraries that are ours"
# -f, not a bare glob: meson leaves a libmpv.so.<ver>.p *directory* of objects
# next to the library, and it matches the same pattern.
for f in "$MPV_BUILD"/libmpv.so.*; do
    [ -f "$f" ] && [ ! -L "$f" ] && cp -a "$f" "$OUT/lib/"
done
for f in "$FFMPEG_LIB"/lib*.so.*; do
    [ -f "$f" ] && [ ! -L "$f" ] && cp -a "$f" "$OUT/lib/"
done
# FFmpeg consumers link the major-version soname, so recreate those symlinks.
( cd "$OUT/lib"
  for f in lib*.so.*.*; do
      soname="$(patchelf --print-soname "$f" 2>/dev/null || true)"
      [ -n "$soname" ] && [ "$soname" != "$f" ] && ln -sf "$f" "$soname"
  done )

step "copying libraries the host names differently"
# Resolved from the build container, where these do exist.
for lib in libjpeg.so.62; do
    src="$(ldconfig -p | grep -m1 "$lib" | awk '{print $NF}')" || true
    [ -n "${src:-}" ] && [ -f "$src" ] || die "could not locate $lib in the build container"
    cp -aL "$src" "$OUT/lib/"
    echo "  $lib"
done

step "rewriting rpaths"
# The build tree bakes in absolute paths (to ~/sofa-deps for libplacebo, to the
# ffmpeg build dir for the rest).  Those directories do not exist on the target
# and, worse, would win over the host's own copies.  Point everything at the
# bundle and let anything not found there resolve from the system.
patchelf --set-rpath '$ORIGIN/lib' "$OUT/mpv-sofa"
for f in "$OUT"/lib/*.so*; do
    [ -L "$f" ] && continue
    patchelf --set-rpath '$ORIGIN' "$f"
done

step "result"
du -sh "$OUT"
echo "  libraries shipped:"
( cd "$OUT/lib" && ls -1 *.so.* | grep -v '^lib.*\.so\.[0-9]*$' | sed 's/^/    /' )

cat <<EOF

Verify on the host (outside any container) with:
    ldd $OUT/mpv-sofa | grep -E 'not found|GLIBC'
Anything listed there is still missing.
EOF
