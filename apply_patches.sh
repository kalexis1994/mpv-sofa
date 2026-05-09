#!/bin/bash
# Apply HRTF patches to mpv source tree
# Run from the project root (hrtf/)

set -e

MPV_SRC="mpv-src"

echo "=== Applying HRTF patches to mpv ==="

# 1. Copy our filter and its module headers
echo "Copying af_hrtf.c + module headers..."
cp mpv-patches/af_hrtf.c   "$MPV_SRC/audio/filter/af_hrtf.c"
cp mpv-patches/af_hrtf_*.h "$MPV_SRC/audio/filter/"

# 2. Copy PFFFT (if submodule exists)
if [ -f "external/pffft/pffft.c" ]; then
    echo "Copying PFFFT..."
    cp external/pffft/pffft.c "$MPV_SRC/audio/filter/pffft.c"
    cp external/pffft/pffft.h "$MPV_SRC/audio/filter/pffft.h"
else
    echo "WARNING: external/pffft not found. Please clone: git submodule add https://github.com/marton78/pffft external/pffft"
fi

# 3. Patch user_filters.h
echo "Patching user_filters.h..."
FILTERS_H="$MPV_SRC/filters/user_filters.h"
if ! grep -q "af_hrtf" "$FILTERS_H"; then
    # Add extern declaration before the last #endif or after last extern
    sed -i '/extern const struct mp_user_filter_entry af_drop;/a extern const struct mp_user_filter_entry af_hrtf;' "$FILTERS_H"
    echo "  Added af_hrtf extern declaration"
else
    echo "  af_hrtf already declared in user_filters.h"
fi

# 4. Patch user_filters.c
echo "Patching user_filters.c..."
FILTERS_C="$MPV_SRC/filters/user_filters.c"
if ! grep -q "af_hrtf" "$FILTERS_C"; then
    # Add &af_hrtf after &af_drop
    sed -i '/&af_drop,/a\    \&af_hrtf,' "$FILTERS_C"
    echo "  Added af_hrtf to af_list[]"
else
    echo "  af_hrtf already in af_list[]"
fi

# 5. Patch meson.build
echo "Patching meson.build..."
MESON_BUILD="$MPV_SRC/meson.build"
if ! grep -q "libmysofa" "$MESON_BUILD"; then
    # Find the rubberband block and add after it
    cat >> "$MESON_BUILD" << 'MESON_PATCH'

# HRTF binaural spatialization filter
libmysofa = dependency('libmysofa', required: get_option('hrtf'))
features += {'hrtf': libmysofa.found()}
if features['hrtf']
    dependencies += libmysofa
    sources += files('audio/filter/af_hrtf.c', 'audio/filter/pffft.c')
endif
MESON_PATCH
    echo "  Added HRTF build configuration"
else
    echo "  HRTF build config already in meson.build"
fi

# 6. Patch meson.options
echo "Patching meson.options..."
MESON_OPTS="$MPV_SRC/meson.options"
if ! grep -q "hrtf" "$MESON_OPTS"; then
    echo "option('hrtf', type: 'feature', value: 'auto', description: 'HRTF binaural audio filter (libmysofa)')" >> "$MESON_OPTS"
    echo "  Added hrtf option"
else
    echo "  hrtf option already exists"
fi

echo ""
echo "=== Patches applied successfully! ==="
echo ""
echo "Next steps:"
echo "  cd $MPV_SRC"
echo "  meson setup build --buildtype=release -Dlibmpv=true -Dhrtf=enabled"
echo "  meson compile -C build"
