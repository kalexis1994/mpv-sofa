#!/usr/bin/env bash
# Create the build container for the SteamOS target.
#
# The binary has to run on the SteamOS host, outside any container, so the
# build environment's ABI must be one the host can satisfy:
#
#   * glibc.  Current Arch ships 2.44 while SteamOS 3.8 has 2.41, and anything
#     built against the former dies at load with "GLIBC_2.43 not found".
#     Debian 13 has 2.41 exactly — same as the host, no guessing, and its
#     repositories are signed and current (pinning Arch's archive instead
#     means downgrading the keyring to re-enable revoked signing keys, which
#     is a fragile thing to depend on).
#
#   * Shared library sonames.  libass.so.9, libglfw.so.3, liblcms2.so.2,
#     libvulkan.so.1 and libshaderc_shared.so.1 all match between Debian 13
#     and SteamOS.  libplacebo does not: Debian has .349, SteamOS has .351.
#     So that one is built from source at the host's exact version — at run
#     time the host's copy satisfies it and nothing needs shipping.
#
# The only things left to ship next to the app are the ones that are genuinely
# ours: the patched libmpv and the patched FFmpeg.
set -euo pipefail

NAME="${CONTAINER_NAME:-sofabuild}"
IMAGE="docker.io/library/debian:trixie"
# Must match `pacman -Q libplacebo` on the SteamOS host.
LIBPLACEBO_VER="${LIBPLACEBO_VER:-v7.351.0}"
DEPS_PREFIX="$HOME/sofa-deps"

step() { printf '\n\033[1;36m=== %s ===\033[0m\n' "$*"; }

if ! distrobox list | awk -F'|' '{print $2}' | tr -d ' ' | grep -qx "$NAME"; then
    step "creating container $NAME ($IMAGE)"
    distrobox create --name "$NAME" --image "$IMAGE" --yes
fi

step "installing the toolchain and dependencies"
distrobox enter --name "$NAME" -- bash -euo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    sudo apt-get update -qq
    sudo apt-get install -y -qq --no-install-recommends \
        build-essential git cmake ninja-build meson pkg-config nasm python3 \
        python3-jsonschema ca-certificates \
        libass-dev liblcms2-dev libjpeg-dev libshaderc-dev libvulkan-dev \
        libvulkan1 mesa-vulkan-drivers spirv-tools libva-dev \
        libglfw3-dev libglm-dev libmysofa-dev zlib1g-dev \
        libpulse-dev libasound2-dev libpipewire-0.3-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
        libgl1-mesa-dev libunwind-dev libxxhash-dev >/dev/null
'

step "building libplacebo $LIBPLACEBO_VER (matching the host soname)"
distrobox enter --name "$NAME" -- bash -euo pipefail -c "
    if [ -f '$DEPS_PREFIX/lib/x86_64-linux-gnu/pkgconfig/libplacebo.pc' ] || \
       [ -f '$DEPS_PREFIX/lib/pkgconfig/libplacebo.pc' ]; then
        echo '  already built'
    else
        rm -rf /tmp/libplacebo
        git clone --depth 1 --branch '$LIBPLACEBO_VER' --recursive \
            https://code.videolan.org/videolan/libplacebo.git /tmp/libplacebo
        cd /tmp/libplacebo
        meson setup build --prefix='$DEPS_PREFIX' --buildtype=release \
            -Ddemos=false -Dtests=false
        meson compile -C build
        meson install -C build
        rm -rf /tmp/libplacebo
    fi
    so=\$(find '$DEPS_PREFIX' -name 'libplacebo.so.*' | head -1)
    echo \"  built: \$(basename \$so)\"
"

step "ABI check against the host"
printf "  host glibc:      %s\n"  "$(ldd --version | head -1 | grep -oE '[0-9]+\.[0-9]+$')"
printf "  host libplacebo: %s\n"  "$(basename "$(readlink -f "$(ldconfig -p | grep -m1 'libplacebo.so' | awk '{print $NF}')")")"
distrobox enter --name "$NAME" -- bash -c '
    printf "  build glibc:     %s\n" "$(ldd --version | head -1 | grep -oE "[0-9]+\.[0-9]+$")"
'
echo
echo "Build with:  distrobox enter --name $NAME -- <command>"
echo "and export PKG_CONFIG_PATH=$DEPS_PREFIX/lib/x86_64-linux-gnu/pkgconfig first."
