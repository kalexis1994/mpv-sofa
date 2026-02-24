/*
 * HaloSound Channel Layouts
 * Standard speaker positions for surround formats
 */

#ifndef CHANNEL_LAYOUTS_H
#define CHANNEL_LAYOUTS_H

typedef struct {
    float azimuth;    /* degrees: 0=front, 90=left, -90=right */
    float elevation;  /* degrees: 0=ear level, 90=above */
    float distance;   /* meters */
} HrtfSpeakerPos;

/* Layout IDs */
#define LAYOUT_STEREO  0
#define LAYOUT_51      1
#define LAYOUT_71      2
#define LAYOUT_714     3
#define LAYOUT_714_OBJ 4  /* 7.1.4 + 4 Atmos objects = 16ch */

/* Returns number of channels for the given layout.
 * Fills positions[0..n-1] with speaker positions.
 * max_channels limits output. */
int channel_layout_get_positions(int layout, HrtfSpeakerPos *positions,
                                 int max_channels);

/* Human-readable layout name */
const char* channel_layout_name(int layout);

/* Channel count for a layout */
int channel_layout_count(int layout);

#endif
