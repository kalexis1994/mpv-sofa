/*
 * HaloSound Channel Layouts
 * Standard speaker positions for surround formats
 * Based on ITU-R BS.775 / Dolby Atmos speaker placement
 */

#include <string.h>
#include "channel_layouts.h"

/* All distances in meters (default 2.0m) */
#define D 2.0f

/* Stereo: 2 channels */
static const HrtfSpeakerPos layout_stereo[] = {
    { 30.0f,  0.0f, D},   /* FL */
    {-30.0f,  0.0f, D},   /* FR */
};

/* 5.1: 6 channels */
static const HrtfSpeakerPos layout_51[] = {
    { 30.0f,   0.0f, D},  /* 0: FL */
    {-30.0f,   0.0f, D},  /* 1: FR */
    {  0.0f,   0.0f, D},  /* 2: FC */
    {  0.0f, -30.0f, D},  /* 3: LFE */
    {135.0f,   0.0f, D},  /* 4: BL */
    {-135.0f,  0.0f, D},  /* 5: BR */
};

/* 7.1: 8 channels */
static const HrtfSpeakerPos layout_71[] = {
    { 30.0f,   0.0f, D},  /* 0: FL */
    {-30.0f,   0.0f, D},  /* 1: FR */
    {  0.0f,   0.0f, D},  /* 2: FC */
    {  0.0f, -30.0f, D},  /* 3: LFE */
    {135.0f,   0.0f, D},  /* 4: BL */
    {-135.0f,  0.0f, D},  /* 5: BR */
    { 90.0f,   0.0f, D},  /* 6: SL */
    {-90.0f,   0.0f, D},  /* 7: SR */
};

/* 7.1.4: 12 channels */
static const HrtfSpeakerPos layout_714[] = {
    { 30.0f,   0.0f, D},  /* 0: FL */
    {-30.0f,   0.0f, D},  /* 1: FR */
    {  0.0f,   0.0f, D},  /* 2: FC */
    {  0.0f, -30.0f, D},  /* 3: LFE */
    {135.0f,   0.0f, D},  /* 4: BL */
    {-135.0f,  0.0f, D},  /* 5: BR */
    { 90.0f,   0.0f, D},  /* 6: SL */
    {-90.0f,   0.0f, D},  /* 7: SR */
    { 45.0f,  45.0f, D},  /* 8: TFL */
    {-45.0f,  45.0f, D},  /* 9: TFR */
    {135.0f,  45.0f, D},  /* 10: TBL */
    {-135.0f, 45.0f, D},  /* 11: TBR */
};

/* 7.1.4 + 4 objects: 16 channels (Atmos with extract_objects) */
static const HrtfSpeakerPos layout_714_obj[] = {
    { 30.0f,   0.0f, D},  /* 0: FL */
    {-30.0f,   0.0f, D},  /* 1: FR */
    {  0.0f,   0.0f, D},  /* 2: FC */
    {  0.0f, -30.0f, D},  /* 3: LFE */
    {135.0f,   0.0f, D},  /* 4: BL */
    {-135.0f,  0.0f, D},  /* 5: BR */
    { 90.0f,   0.0f, D},  /* 6: SL */
    {-90.0f,   0.0f, D},  /* 7: SR */
    { 45.0f,  45.0f, D},  /* 8: Obj0 (default TFL) */
    {-45.0f,  45.0f, D},  /* 9: Obj1 (default TFR) */
    {135.0f,  45.0f, D},  /* 10: Obj2 (default TBL) */
    {-135.0f, 45.0f, D},  /* 11: Obj3 (default TBR) */
    {  0.0f,  90.0f, D},  /* 12: Obj4 (top center) */
    { 90.0f,  30.0f, D},  /* 13: Obj5 (side left high) */
    {-90.0f,  30.0f, D},  /* 14: Obj6 (side right high) */
    {180.0f,  45.0f, D},  /* 15: Obj7 (rear top center) */
};

static const struct {
    const HrtfSpeakerPos *positions;
    int count;
    const char *name;
} layouts[] = {
    { layout_stereo,   2,  "Stereo" },
    { layout_51,       6,  "5.1 Surround" },
    { layout_71,       8,  "7.1 Surround" },
    { layout_714,      12, "7.1.4 Atmos" },
    { layout_714_obj,  16, "7.1.4 + Objects" },
};

#define NUM_LAYOUTS (sizeof(layouts) / sizeof(layouts[0]))

int channel_layout_get_positions(int layout, HrtfSpeakerPos *positions,
                                 int max_channels) {
    if (layout < 0 || layout >= (int)NUM_LAYOUTS)
        layout = LAYOUT_71;

    int n = layouts[layout].count;
    if (n > max_channels) n = max_channels;

    memcpy(positions, layouts[layout].positions, n * sizeof(HrtfSpeakerPos));
    return n;
}

const char* channel_layout_name(int layout) {
    if (layout < 0 || layout >= (int)NUM_LAYOUTS)
        return "Unknown";
    return layouts[layout].name;
}

int channel_layout_count(int layout) {
    if (layout < 0 || layout >= (int)NUM_LAYOUTS)
        return 0;
    return layouts[layout].count;
}
