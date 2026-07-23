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
#include <math.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

#include "hrtf_engine.h"

#ifndef __EMSCRIPTEN__
#include <mysofa.h>
#endif

#define BLOCK 256   /* HRTF_BLOCK_SIZE — halo_process hard requirement */

#ifndef __EMSCRIPTEN__
/* ---- --info: dump a SOFA file's AES69 metadata as JSON ----------------- */

static void json_escape_print(const char* s) {
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c == '\n' || c == '\r') putchar(' ');
        else if (c < 0x20) continue;
        else putchar(c);
    }
    putchar('"');
}

static int print_sofa_info(const char* path) {
    int err = 0;
    struct MYSOFA_HRTF* h = mysofa_load(path, &err);
    if (!h) {
        fprintf(stderr, "cannot load sofa (err %d): %s\n", err, path);
        return 1;
    }
    printf("{\"measurements\":%u,\"irLength\":%u,\"sampleRate\":%g",
           h->M, h->N,
           (h->DataSamplingRate.values && h->DataSamplingRate.elements > 0)
               ? h->DataSamplingRate.values[0] : 0.0);

    static const char* keys[] = {
        "Title", "DatabaseName", "ListenerShortName", "Organization",
        "License", "Comment", "AuthorContact", "DateCreated",
    };
    for (struct MYSOFA_ATTRIBUTE* a = h->attributes; a; a = a->next) {
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            if (a->name && a->value && !strcmp(a->name, keys[k]) &&
                a->value[0]) {
                printf(",\"%c%s\":", keys[k][0] + 32, keys[k] + 1);
                json_escape_print(a->value);
            }
        }
    }
    printf("}\n");
    mysofa_free(h);
    return 0;
}
#endif

/*
 * ---- DAMF mode (--damf PREFIX) -----------------------------------------
 *
 * Object-based Atmos rendering. truehdd decodes a TrueHD Atmos stream into
 *   PREFIX.atmos           manifest: bed channels + object IDs (CAF order)
 *   PREFIX.atmos.audio     CAF, 24-bit BE PCM, one channel per bed/object
 *   PREFIX.atmos.metadata  timed events: 3D position + gain per ID
 * Every object is fed to the engine as a movable speaker: positions update
 * per block via halo_set_speaker_pos (the engine crossfades HRIRs), so the
 * scene renders from the true object trajectories instead of a fixed bed.
 * With --follow the reader tails files still being written by truehdd and
 * finishes when PREFIX.done appears.
 */

#define DAMF_MAX_CH 32

typedef struct {
    int  nch;                    /* total channels in CAF */
    int  ids[DAMF_MAX_CH];       /* metadata ID per channel */
    int  is_lfe[DAMF_MAX_CH];    /* bypass HRTF, direct mix */
    int  is_object[DAMF_MAX_CH]; /* 1 = dynamic object, 0 = bed channel */
    float gain[DAMF_MAX_CH];     /* linear, from metadata events */
    float az0[DAMF_MAX_CH];      /* initial position (bed: by name) */
    float el0[DAMF_MAX_CH];
} DamfScene;

/* --solo: audition one layer of the DAMF mix. */
enum { SOLO_NONE = 0, SOLO_BED = 1, SOLO_OBJECTS = 2 };

/* The engine hard-codes channel 3 as LFE (direct mix, no HRTF). In DAMF
 * mode channel 3 is an arbitrary object, so the engine runs with one extra
 * channel and everything from index 3 shifts up: engine ch 3 stays silent. */
static int engmap(int c) { return c < 3 ? c : c + 1; }

/* DAMF coords: x -1(L)..1(R), y 1(front)..-1(back), z 0..1(top).
 * Engine: azimuth 0=front 90=left, elevation 0=ear level 90=up. */
static void damf_pos_to_speaker(float x, float y, float z,
                                float* az, float* el, float* dist) {
    float h = hypotf(x, y);
    *az = (h > 1e-6f || fabsf(x) > 1e-6f) ? atan2f(-x, y) * 57.29578f : 0.0f;
    *el = atan2f(z, h > 1e-6f ? h : 1e-6f) * 57.29578f;
    float d = sqrtf(x * x + y * y + z * z);
    *dist = 2.0f * (d < 0.35f ? 0.35f : d);   /* nominal 2m ring, no in-head */
}

/* Fixed positions for named bed channels (az, el). */
static int bed_pos_by_name(const char* name, float* az, float* el) {
    static const struct { const char* n; float az, el; } tab[] = {
        {"L", 30, 0}, {"R", -30, 0}, {"C", 0, 0}, {"LFE", 0, -30},
        {"Ls", 110, 0}, {"Rs", -110, 0}, {"Lss", 90, 0}, {"Rss", -90, 0},
        {"Lrs", 150, 0}, {"Rrs", -150, 0},
        {"Lts", 90, 60}, {"Rts", -90, 60},
        {"Ltf", 45, 45}, {"Rtf", -45, 45}, {"Ltr", 135, 45}, {"Rtr", -135, 45},
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (!strcmp(tab[i].n, name)) { *az = tab[i].az; *el = tab[i].el; return 1; }
    }
    return 0;
}

/* Parse PREFIX.atmos: bed channel names (+IDs) then object IDs, in CAF
 * channel order. */
static int damf_read_manifest(const char* prefix, DamfScene* sc) {
    char path[1024];
    snprintf(path, sizeof(path), "%s.atmos", prefix);
    FILE* f = fopen(path, "r");
    if (!f) return -1;

    sc->nch = 0;
    char line[512];
    int in_objects = 0;
    char pending_name[64] = "";
    while (fgets(line, sizeof(line), f)) {
        char name[64]; int id;
        if (strstr(line, "objects:")) { in_objects = 1; continue; }
        if (!in_objects && sscanf(line, " - channel: %63s", name) == 1) {
            strncpy(pending_name, name, sizeof(pending_name) - 1);
            continue;
        }
        if (sscanf(line, " %*[-] ID: %d", &id) == 1 || sscanf(line, " ID: %d", &id) == 1) {
            if (sc->nch >= DAMF_MAX_CH) break;
            int ch = sc->nch++;
            sc->ids[ch] = id;
            sc->gain[ch] = 1.0f;
            sc->is_lfe[ch] = 0;
            sc->is_object[ch] = in_objects;
            sc->az0[ch] = 0; sc->el0[ch] = 0;
            if (!in_objects) {
                float az = 0, el = 0;
                if (bed_pos_by_name(pending_name, &az, &el)) {
                    if (!strcmp(pending_name, "LFE")) sc->is_lfe[ch] = 1;
                    sc->az0[ch] = az; sc->el0[ch] = el;
                }
                pending_name[0] = 0;
            }
        }
    }
    fclose(f);
    return sc->nch > 0 ? 0 : -1;
}

/* Incremental metadata reader: parses one event at a time, applies it when
 * the playhead reaches its samplePos. */
typedef struct {
    FILE* f;
    int   have_event;
    int   ev_id;
    long long ev_sample;
    int   ev_has_pos;
    float ev_x, ev_y, ev_z;
    float ev_gain_db;
} DamfMeta;

static int damf_meta_next(DamfMeta* m, int follow, const char* done_path) {
    /* Reads lines until a complete event (terminated by the next "- ID:" or
     * EOF) is buffered. Returns 1 when m->ev_* holds an applicable event. */
    static char line[512];
    long pos;
    for (;;) {
        pos = ftell(m->f);
        if (!fgets(line, sizeof(line), m->f)) {
            if (follow) {
                FILE* dn = done_path ? fopen(done_path, "r") : NULL;
                if (!dn) { clearerr(m->f); sleep_ms(25); continue; }
                fclose(dn);
            }
            /* final flush: emit buffered event if complete */
            if (m->have_event == 2) { m->have_event = 0; return 1; }
            return 0;
        }
        int id; float x, y, z; long long sp; float g;
        if (sscanf(line, " - ID: %d", &id) == 1) {
            if (m->have_event == 2) {          /* previous event complete */
                fseek(m->f, pos, SEEK_SET);    /* re-read this line next call */
                m->have_event = 0;
                return 1;
            }
            m->have_event = 1;
            m->ev_id = id; m->ev_has_pos = 0; m->ev_gain_db = 0; m->ev_sample = 0;
            continue;
        }
        if (!m->have_event) continue;
        if (sscanf(line, " samplePos: %lld", &sp) == 1) { m->ev_sample = sp; m->have_event = 2; }
        else if (sscanf(line, " pos: [%f, %f, %f]", &x, &y, &z) == 3) {
            m->ev_x = x; m->ev_y = y; m->ev_z = z; m->ev_has_pos = 1;
        }
        else if (sscanf(line, " gain: %f", &g) == 1) m->ev_gain_db = g;
    }
}

/* CAF reader: returns FILE* positioned at PCM data; fills nch. */
static FILE* damf_open_caf(const char* prefix, int* nch, int follow, const char* done_path) {
    char path[1024];
    snprintf(path, sizeof(path), "%s.atmos.audio", prefix);
    FILE* f = NULL;
    for (int tries = 0; tries < 400; tries++) {          /* wait for creation */
        f = fopen(path, "rb");
        if (f) break;
        if (!follow) break;
        sleep_ms(25);
    }
    if (!f) { fprintf(stderr, "[render] no CAF: %s\n", path); return NULL; }

    /* Header: 'caff' ver flags, then chunks (type + int64 size). */
    uint8_t h[8];
    if (fread(h, 1, 8, f) != 8 || memcmp(h, "caff", 4)) { fclose(f); return NULL; }
    for (;;) {
        uint8_t ch[12];
        while (fread(ch, 1, 12, f) != 12) {
            if (!follow) { fclose(f); return NULL; }
            clearerr(f); sleep_ms(25);
        }
        long long sz = 0;
        for (int i = 0; i < 8; i++) sz = (sz << 8) | ch[4 + i];
        if (!memcmp(ch, "desc", 4)) {
            uint8_t d[32];
            if (fread(d, 1, 32, f) != 32) { fclose(f); return NULL; }
            *nch = (d[24] << 24) | (d[25] << 16) | (d[26] << 8) | d[27];
            int bits = (d[28] << 24) | (d[29] << 16) | (d[30] << 8) | d[31];
            if (memcmp(d + 8, "lpcm", 4) || bits != 24) {
                fprintf(stderr, "[render] unexpected CAF format (bits=%d)\n", bits);
                fclose(f); return NULL;
            }
        } else if (!memcmp(ch, "data", 4)) {
            uint8_t edit[4];
            while (fread(edit, 1, 4, f) != 4) {
                if (!follow) { fclose(f); return NULL; }
                clearerr(f); sleep_ms(25);
            }
            return f;   /* positioned at first frame; sz may be -1 (growing) */
        } else {
            /* skip unknown chunk (may need to wait for it to be written) */
            if (sz < 0) { fclose(f); return NULL; }
            long long skipped = 0;
            while (skipped < sz) {
                uint8_t tmp[4096];
                size_t want = (size_t)((sz - skipped) > 4096 ? 4096 : (sz - skipped));
                size_t got = fread(tmp, 1, want, f);
                if (got == 0) {
                    if (!follow) { fclose(f); return NULL; }
                    clearerr(f); sleep_ms(25); continue;
                }
                skipped += (long long)got;
            }
        }
    }
}

static int run_damf(const char* prefix, const uint8_t* sofa_buf, int sofa_len,
                    int rate, int room, float volume,
                    long long skip_samples, int follow, int solo) {
    char done_path[1024];
    snprintf(done_path, sizeof(done_path), "%s.done", prefix);

    DamfScene sc;
    /* Manifest is written as soon as truehdd detects Atmos. */
    for (int tries = 0; ; tries++) {
        if (damf_read_manifest(prefix, &sc) == 0) break;
        if (!follow || tries > 400) {
            fprintf(stderr, "[render] no DAMF manifest at %s.atmos\n", prefix);
            return 1;
        }
        sleep_ms(25);
    }
    /* +1: engine ch 3 is reserved (hard-coded LFE) and kept silent. */
    if (sc.nch > 15) sc.nch = 15;
    int eng_nch = sc.nch + 1;

    HaloEngine* eng = halo_create(rate, eng_nch);
    if (!eng) return 1;
    if (halo_load_sofa(eng, sofa_buf, sofa_len) != 0)
        fprintf(stderr, "warning: sofa load reported failure, continuing\n");
    halo_set_room_preset(eng, room);
    for (int c = 0; c < sc.nch; c++)
        halo_set_speaker_pos(eng, engmap(c), sc.az0[c], sc.el0[c], 2.0f);

    int caf_ch = 0;
    FILE* caf = damf_open_caf(prefix, &caf_ch, follow, done_path);
    if (!caf) { halo_destroy(eng); return 1; }
    if (caf_ch != sc.nch)
        fprintf(stderr, "[render] warn: CAF %dch vs manifest %dch\n", caf_ch, sc.nch);
    int nch = caf_ch < sc.nch ? caf_ch : sc.nch;

    char mpath[1024];
    snprintf(mpath, sizeof(mpath), "%s.atmos.metadata", prefix);
    DamfMeta meta = {0};
    meta.f = fopen(mpath, "r");

    {
        int nobj = 0;
        for (int c = 0; c < nch; c++) nobj += sc.is_object[c];
        fprintf(stderr, "[render] DAMF: %d ch (%d bed + %d objects), follow=%d%s\n",
                nch, nch - nobj, nobj, follow,
                solo == SOLO_BED ? ", solo=bed" :
                solo == SOLO_OBJECTS ? ", solo=objects" : "");
    }

    size_t fbytes = (size_t)caf_ch * 3;
    uint8_t* raw = (uint8_t*)malloc(fbytes * BLOCK);
    float* in  = (float*)calloc((size_t)BLOCK * eng_nch, sizeof(float));
    float* out = (float*)malloc(sizeof(float) * BLOCK * 2);
    float* lfe = (float*)malloc(sizeof(float) * BLOCK);
    long long playhead = 0;
    int pending = 0;   /* an unapplied event sits in meta.ev_* */

    for (;;) {
        /* Apply all metadata events due at/before this block. */
        while (meta.f) {
            if (!pending) {
                if (!damf_meta_next(&meta, follow, done_path)) { fclose(meta.f); meta.f = NULL; break; }
                pending = 1;
            }
            if (meta.ev_sample > playhead + BLOCK) break;
            for (int c = 0; c < nch; c++) {
                if (sc.ids[c] != meta.ev_id) continue;
                sc.gain[c] = powf(10.0f, meta.ev_gain_db / 20.0f);
                if (meta.ev_has_pos && !sc.is_lfe[c]) {
                    float az, el, dist;
                    damf_pos_to_speaker(meta.ev_x, meta.ev_y, meta.ev_z, &az, &el, &dist);
                    halo_set_speaker_pos(eng, engmap(c), az, el, dist);
                }
            }
            pending = 0;
        }

        /* Read one block of 24-bit BE frames (tail the growing file). */
        size_t need = fbytes * BLOCK, have = 0;
        while (have < need) {
            size_t got = fread(raw + have, 1, need - have, caf);
            have += got;
            if (got == 0) {
                FILE* dn = fopen(done_path, "r");
                if (dn) { fclose(dn); break; }
                if (!follow) break;
                clearerr(caf); sleep_ms(20);
            }
        }
        size_t frames = have / fbytes;
        if (frames == 0) break;

        memset(lfe, 0, sizeof(float) * BLOCK);
        memset(in, 0, (size_t)BLOCK * eng_nch * sizeof(float));
        for (size_t i = 0; i < frames; i++) {
            for (int c = 0; c < nch; c++) {
                /* Solo audition: drop the other layer (LFE counts as bed). */
                if (solo == SOLO_BED     &&  sc.is_object[c]) continue;
                if (solo == SOLO_OBJECTS && !sc.is_object[c]) continue;
                const uint8_t* p = raw + i * fbytes + (size_t)c * 3;
                int32_t v = ((int32_t)(int8_t)p[0] << 16) | (p[1] << 8) | p[2];
                float s = (float)v / 8388608.0f * sc.gain[c];
                if (sc.is_lfe[c]) lfe[i] += s;
                else in[i * eng_nch + engmap(c)] = s;
            }
        }

        halo_process(eng, in, out, BLOCK);

        for (size_t i = 0; i < frames; i++) {       /* LFE direct, engine-style */
            out[i * 2]     += lfe[i] * 0.5f;
            out[i * 2 + 1] += lfe[i] * 0.5f;
        }
        if (volume != 1.0f)
            for (size_t i = 0; i < frames * 2; i++) out[i] *= volume;

        const float* wp = out;
        size_t wframes = frames;
        if (skip_samples > 0) {                     /* seek-alignment trim */
            long long drop = skip_samples < (long long)wframes ? skip_samples : (long long)wframes;
            wp += drop * 2; wframes -= (size_t)drop; skip_samples -= drop;
        }
        if (wframes > 0 && fwrite(wp, sizeof(float) * 2, wframes, stdout) != wframes) break;
        playhead += (long long)frames;
        if (frames < BLOCK) break;
    }

    if (meta.f) fclose(meta.f);
    fclose(caf);
    halo_destroy(eng);
    free(raw); free(in); free(out); free(lfe);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "usage: halosound-render --sofa FILE [--channels N] [--rate HZ]\n"
        "                        [--layout N] [--room N] [--volume F]\n"
        "                        [--damf PREFIX [--follow] [--skip N]\n"
        "                                       [--solo bed|objects]]\n"
        "  stdin:  f32le interleaved, N channels\n"
        "  stdout: f32le interleaved, stereo binaural\n"
        "  --damf:   object-based Atmos input from truehdd DAMF files\n"
        "            (PREFIX.atmos/.atmos.audio/.atmos.metadata); --follow\n"
        "            tails growing files until PREFIX.done exists; --skip\n"
        "            drops N output samples (seek alignment); --solo\n"
        "            auditions one layer of the mix (LFE counts as bed)\n"
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
#ifndef __EMSCRIPTEN__
    if (argc >= 3 && !strcmp(argv[1], "--info"))
        return print_sofa_info(argv[2]);
#endif

    const char* sofa_path = NULL;
    const char* damf_prefix = NULL;
    int channels = 8, rate = 48000, layout = -1, room = 1, follow = 0, solo = SOLO_NONE;
    long long skip = 0;
    float volume = 1.0f;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sofa") && i + 1 < argc)          sofa_path = argv[++i];
        else if (!strcmp(argv[i], "--channels") && i + 1 < argc) channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc)     rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--layout") && i + 1 < argc)   layout = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--room") && i + 1 < argc)     room = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc)   volume = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--damf") && i + 1 < argc)     damf_prefix = argv[++i];
        else if (!strcmp(argv[i], "--follow"))                   follow = 1;
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc)     skip = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--solo") && i + 1 < argc) {
            const char* s = argv[++i];
            if      (!strcmp(s, "bed"))     solo = SOLO_BED;
            else if (!strcmp(s, "objects")) solo = SOLO_OBJECTS;
            else { usage(); return 1; }
        }
        else { usage(); return 2; }
    }
    if (!sofa_path || (!damf_prefix && (channels < 1 || channels > 16))) { usage(); return 2; }

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

    if (damf_prefix) {
        int rc = run_damf(damf_prefix, sbuf, (int)ssz, rate, room, volume, skip, follow, solo);
        free(sbuf);
        return rc;
    }

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
