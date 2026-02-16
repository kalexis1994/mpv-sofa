# mpv Patches for HRTF Spatial Audio

These files modify mpv's source to add HRTF binaural spatialization.

## Files to copy into mpv-src/

### New files:
- `af_hrtf.c` → `mpv-src/audio/filter/af_hrtf.c`
- `ad_losslesshd.c` → `mpv-src/audio/decode/ad_losslesshd.c`

### Files to patch:

#### `filters/user_filters.h`
Add after other extern declarations:
```c
extern const struct mp_user_filter_entry af_hrtf;
```

#### `filters/user_filters.c`
Add to `af_list[]` array:
```c
#if HAVE_HRTF
    &af_hrtf,
#endif
```

#### `meson.build`
Add to sources (around line 69, after other af_ files):
```meson
'audio/filter/af_hrtf.c',
```

Add dependency block (after rubberband block, ~line 748):
```meson
# HRTF binaural spatialization
libmysofa = dependency('libmysofa', required: get_option('hrtf'))
features += {'hrtf': libmysofa.found()}
if features['hrtf']
    dependencies += libmysofa
    sources += files('audio/filter/af_hrtf.c')
endif
```

#### `meson.options`
Add:
```meson
option('hrtf', type: 'feature', value: 'auto', description: 'HRTF binaural audio (libmysofa)')
```

## Usage
After building the modified mpv:
```bash
mpv --af=hrtf=sofa=/path/to/hrir.sofa movie.mkv
```

Or from libmpv:
```c
mpv_set_option_string(mpv, "af", "hrtf=sofa=/path/to/hrir.sofa");
```
