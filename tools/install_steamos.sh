#!/usr/bin/env bash
# Install the packaged build on SteamOS and register it with Steam.
#
# Run this on the host, NOT inside the build container: it talks to the
# running Steam client and reads the host's own directories.
#
# Shape of the install, and why:
#
#   * A plain directory under ~/Applications, never an AppImage.  Steam's
#     reaper tears down stray processes in the cgroup when a title exits,
#     which kills an AppImage's squashfuse mount; the app then can't read its
#     own resources and dies.  We hit exactly that with ES-DE on this machine.
#   * The launcher is a small script rather than the binary itself.  The
#     desktop entry and the Steam shortcut both point at it, so anything the
#     app needs at launch (an env var, a working directory) has one home
#     instead of being duplicated into shortcuts.vdf where it is invisible.
#   * steamos-add-to-steam rather than editing shortcuts.vdf: Steam rewrites
#     that file from memory when it exits, so an edit made while it runs is
#     simply lost.  The helper goes through the running client instead.
set -euo pipefail

SRC="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/dist-linux}"
PREFIX="$HOME/Applications/mpv-sofa"
LAUNCHER="$PREFIX/mpv-sofa.sh"
DESKTOP="$HOME/.local/share/applications/mpv-sofa.desktop"
ICON_DIR="$HOME/.local/share/icons/hicolor/256x256/apps"

step() { printf '\n\033[1;36m=== %s ===\033[0m\n' "$*"; }
die()  { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[ -x "$SRC/mpv-sofa" ] || die "no packaged build at $SRC — run tools/package_linux.sh first"
[ -d "$SRC/lib" ]      || die "$SRC has no lib/ — the package looks incomplete"

step "installing to $PREFIX"
mkdir -p "$PREFIX"
# Mirror rather than wipe: keeps the install atomic enough that a running
# instance doesn't lose its assets mid-frame.
if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete "$SRC"/ "$PREFIX"/
else
    rm -rf "${PREFIX:?}"/* && cp -a "$SRC"/. "$PREFIX"/
fi
du -sh "$PREFIX"

step "launcher"
cat > "$LAUNCHER" <<'EOF'
#!/usr/bin/env bash
# Launcher for mpv-sofa.  Kept separate from the binary so the desktop entry
# and the Steam shortcut share one definition of how the app starts.
HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"

# Assets are found relative to the binary, so start from its directory; the
# working directory Steam hands us is not ours to rely on.
cd "$HERE" || exit 1

# The bundled libmpv and FFmpeg are reached through the binary's $ORIGIN
# rpath, so LD_LIBRARY_PATH is deliberately NOT set here — leaving it unset
# means every other library still resolves from the system, which is what
# keeps libplacebo and its shaderc coming from the host.

exec "$HERE/mpv-sofa" "$@"
EOF
chmod +x "$LAUNCHER"
echo "  $LAUNCHER"

step "icon"
mkdir -p "$ICON_DIR"
ICON_SRC="$(find "$PREFIX/assets/icons" -maxdepth 1 -iname '*logo*.png' -o -maxdepth 1 -iname 'mpv-sofa*.png' 2>/dev/null | head -1)"
if [ -n "${ICON_SRC:-}" ]; then
    cp -f "$ICON_SRC" "$ICON_DIR/mpv-sofa.png"
    ICON_NAME="mpv-sofa"
    echo "  $ICON_DIR/mpv-sofa.png"
else
    ICON_NAME="applications-multimedia"
    echo "  no bundled icon found — falling back to a stock theme icon"
fi

step "desktop entry"
mkdir -p "$(dirname "$DESKTOP")"
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=mpv-sofa
GenericName=Binaural Media Player
Comment=HRTF binaural spatializer for headphone listening
Exec=$LAUNCHER %f
Icon=$ICON_NAME
Terminal=false
StartupNotify=true
Categories=AudioVideo;Player;Video;
MimeType=video/x-matroska;video/mp4;video/x-msvideo;video/quicktime;video/webm;
Keywords=hrtf;binaural;spatial;atmos;mpv;
EOF
chmod +x "$DESKTOP"
update-desktop-database "$(dirname "$DESKTOP")" 2>/dev/null || true
echo "  $DESKTOP"

step "registering with Steam"
if ! command -v steamos-add-to-steam >/dev/null 2>&1; then
    echo "  steamos-add-to-steam not found — add $LAUNCHER as a non-Steam game by hand"
elif ! pgrep -x steam >/dev/null 2>&1; then
    echo "  Steam isn't running; start it and re-run, or add $LAUNCHER by hand"
else
    steamos-add-to-steam "$DESKTOP" && echo "  requested — confirm the prompt in Steam if one appears"
fi

step "done"
cat <<EOF

Installed:  $PREFIX
Launch:     $LAUNCHER
Config:     ~/.config/mpv-sofa   (settings, imgui layout)
Cache:      ~/.cache/mpv-sofa    (binaural sidecars, scratch)

Nothing is written next to the binary, so $PREFIX can be replaced wholesale
by re-running this script without losing settings.
EOF
