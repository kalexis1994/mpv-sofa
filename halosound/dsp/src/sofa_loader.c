/*
 * HaloSound SOFA Loader
 *
 * When compiled with HAVE_MYSOFA: uses libmysofa for real SOFA loading.
 * Without HAVE_MYSOFA: provides a stub with dirac impulse HRIRs for testing.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "sofa_loader.h"

#ifdef HAVE_MYSOFA
#include <mysofa.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEFAULT_IR_LENGTH 128

SofaData* sofa_load_buffer(const uint8_t *data, int size, int target_sample_rate) {
    SofaData *sofa = calloc(1, sizeof(SofaData));
    if (!sofa) return NULL;

    sofa->sample_rate = target_sample_rate;

#ifdef HAVE_MYSOFA
    int err;
    int filter_length = 0;

    /* libmysofa can open from a file path. For buffer loading we'd need
     * mysofa_load_data which may not be available in all versions.
     * As a practical approach, we write to a temp file if needed. */
    /* For now, try direct open if data looks like a file path,
     * otherwise use the buffer API if available. */
    struct MYSOFA_EASY *easy = NULL;

    /* Try buffer-based loading (mysofa >= 1.0) */
    #if defined(MYSOFA_VERSION_MAJOR) && MYSOFA_VERSION_MAJOR >= 1
    easy = mysofa_open_data((const char *)data, size,
                            (float)target_sample_rate, &filter_length, &err);
    #else
    /* Fallback: write buffer to temp file */
    {
        const char *tmppath = "/tmp/halosound_temp.sofa";
        FILE *f = fopen(tmppath, "wb");
        if (f) {
            fwrite(data, 1, size, f);
            fclose(f);
            easy = mysofa_open(tmppath, (float)target_sample_rate,
                              &filter_length, &err);
        }
    }
    #endif

    if (!easy || err != MYSOFA_OK) {
        sofa->loaded = 0;
        sofa->ir_length = DEFAULT_IR_LENGTH;
        return sofa;
    }

    sofa->handle = easy;
    sofa->ir_length = filter_length;
    sofa->loaded = 1;
#else
    /* No libmysofa - stub mode with dirac impulse HRIRs */
    (void)data;
    (void)size;
    sofa->ir_length = DEFAULT_IR_LENGTH;
    sofa->loaded = 0;
#endif

    return sofa;
}

void sofa_get_hrir(SofaData *sofa, float x, float y, float z,
                   float *hrir_l, float *hrir_r,
                   float *delay_l, float *delay_r) {
    int len = sofa ? sofa->ir_length : DEFAULT_IR_LENGTH;

    memset(hrir_l, 0, len * sizeof(float));
    memset(hrir_r, 0, len * sizeof(float));
    *delay_l = 0;
    *delay_r = 0;

#ifdef HAVE_MYSOFA
    if (sofa && sofa->loaded && sofa->handle) {
        struct MYSOFA_EASY *easy = (struct MYSOFA_EASY *)sofa->handle;
        float dl = 0, dr = 0;

        mysofa_getfilter_float(easy, x, y, z, hrir_l, hrir_r, &dl, &dr);

        *delay_l = dl;
        *delay_r = dr;
        return;
    }
#endif

    /*
     * Stub mode: generate a simple dirac impulse with basic ITD.
     * This allows the convolution pipeline to work without a real SOFA file.
     * The azimuth-based ITD provides minimal spatialization.
     */
    (void)sofa;

    /* Compute azimuth from cartesian (x=front, y=left, z=up) */
    float dist = sqrtf(x * x + y * y + z * z);
    if (dist < 0.01f) dist = 0.01f;

    float azimuth = atan2f(y, x); /* radians, 0=front, +pi/2=left */

    /* Simple ITD model: ~0.7ms maximum for 90 degrees */
    float head_radius = 0.0875f; /* meters */
    float itd = head_radius * (azimuth + sinf(azimuth)) / 343.0f;

    if (itd > 0) {
        *delay_l = 0;
        *delay_r = itd;
    } else {
        *delay_l = -itd;
        *delay_r = 0;
    }

    /* Dirac impulse at sample 0 with simple level difference */
    float ild_factor = 1.0f + 0.3f * sinf(azimuth);
    hrir_l[0] = 0.5f * ild_factor;
    hrir_r[0] = 0.5f * (2.0f - ild_factor);
}

void sofa_destroy(SofaData *sofa) {
    if (!sofa) return;

#ifdef HAVE_MYSOFA
    if (sofa->handle) {
        mysofa_close((struct MYSOFA_EASY *)sofa->handle);
    }
#endif

    free(sofa);
}
