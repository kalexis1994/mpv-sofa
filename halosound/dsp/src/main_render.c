/*
 * halosound-render — native CLI for server-side binaural rendering.
 *
 * Reads interleaved float32le multichannel PCM on stdin, runs the SAME
 * DSP the TV client uses (HRTF convolution via libmysofa + room preset +
 * freeverb + early reflections), writes interleaved float32le STEREO on
 * stdout.  Designed to sit in an ffmpeg pipeline:
 *
 *   ffmpeg -i movie -map 0:a:N -f f32le -ac 8 -ar 48000 - |
 *     halosound-render --sofa p.sofa --channels 8 --room 1 |
 *   ffmpeg -ss T -i movie -f f32le -ac 2 -ar 48000 -i - \
 *     -map 0:v -c:v copy -map 1:a -c:a aac ... -f hls ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "hrtf_engine.h"

#define BLOCK 256   /* HRTF_BLOCK_SIZE — halo_process hard requirement */

static void usage(void) {
    fprintf(stderr,
        "usage: halosound-render --sofa FILE [--channels N] [--rate HZ]\n"
        "                        [--layout N] [--room N] [--volume F]\n"
        "  stdin:  f32le interleaved, N channels\n"
        "  stdout: f32le interleaved, stereo binaural\n"
        "  --layout: -1 auto from channels (default), 0 stereo, 1 5.1,\n"
        "            2 7.1, 3 7.1.4, 4 7.1.4+objects\n"
        "  --room:   0 Studio 1 HomeTheater 2 Cinema 3 ConcertHall\n"
        "            4 LivingRoom 5 ScreeningRoom 6 IMAX 7 DolbyCinema 8 Dry\n");
}

static int layout_from_channels(int ch) {
    if (ch >= 16) return 4;
    if (ch >= 12) return 3;
    if (ch >= 8)  return 2;
    if (ch >= 6)  return 1;
    return 0;
}

int main(int argc, char** argv) {
    const char* sofa_path = NULL;
    int channels = 8, rate = 48000, layout = -1, room = 1;
    float volume = 1.0f;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sofa") && i + 1 < argc)          sofa_path = argv[++i];
        else if (!strcmp(argv[i], "--channels") && i + 1 < argc) channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc)     rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--layout") && i + 1 < argc)   layout = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--room") && i + 1 < argc)     room = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc)   volume = (float)atof(argv[++i]);
        else { usage(); return 2; }
    }
    if (!sofa_path || channels < 1 || channels > 16) { usage(); return 2; }

#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /* Load the SOFA profile into memory (engine takes a buffer). */
    FILE* sf = fopen(sofa_path, "rb");
    if (!sf) { fprintf(stderr, "cannot open sofa: %s\n", sofa_path); return 1; }
    fseek(sf, 0, SEEK_END);
    long ssz = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    uint8_t* sbuf = (uint8_t*)malloc(ssz);
    if (!sbuf || fread(sbuf, 1, ssz, sf) != (size_t)ssz) {
        fprintf(stderr, "cannot read sofa\n"); return 1;
    }
    fclose(sf);

    HaloEngine* eng = halo_create(rate, channels);
    if (!eng) { fprintf(stderr, "halo_create failed\n"); return 1; }
    if (halo_load_sofa(eng, sbuf, (int)ssz) != 0) {
        fprintf(stderr, "warning: sofa load reported failure, continuing\n");
    }
    free(sbuf);

    halo_set_layout(eng, layout >= 0 ? layout : layout_from_channels(channels));
    halo_set_room_preset(eng, room);

    fprintf(stderr, "[render] %dch @%dHz room=%d sofa=%s\n",
            channels, rate, room, sofa_path);

    float* in  = (float*)malloc(sizeof(float) * BLOCK * channels);
    float* out = (float*)malloc(sizeof(float) * BLOCK * 2);
    size_t frame_bytes = sizeof(float) * channels;

    for (;;) {
        /* Fill one 256-sample block (handle short reads at EOF). */
        size_t got = fread(in, frame_bytes, BLOCK, stdin);
        if (got == 0) break;
        if (got < BLOCK) {
            memset(in + got * channels, 0, (BLOCK - got) * frame_bytes);
        }

        halo_process(eng, in, out, BLOCK);

        if (volume != 1.0f) {
            for (int i = 0; i < BLOCK * 2; i++) out[i] *= volume;
        }
        if (fwrite(out, sizeof(float) * 2, got, stdout) != got) break;
        if (got < BLOCK) break;   /* EOF tail flushed */
    }

    halo_destroy(eng);
    free(in);
    free(out);
    return 0;
}
