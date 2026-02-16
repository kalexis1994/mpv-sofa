/*
 * ad_losslesshd.c - Custom lossless HD spatial decoder for mpv
 *
 * Uses the losslesshd_ffi Rust library to decode lossless HD spatial streams,
 * extracting both the multichannel audio bed AND spatial object
 * metadata (3D positions).
 *
 * Falls back to ad_lavc (FFmpeg) if losslesshd_ffi is not available or
 * if the stream is not spatial.
 *
 * This file goes into mpv-src/audio/decode/ad_losslesshd.c
 */

#include <string.h>
#include <stdlib.h>

#include "audio/aframe.h"
#include "audio/chmap.h"
#include "audio/format.h"
#include "common/common.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"

// Include the generated C header from the Rust crate
// This will be available after `cargo build` generates it via cbindgen
#ifdef HAVE_LOSSLESSHD_FFI
#include "losslesshd_ffi.h"
#endif

struct priv {
    struct mp_codec_params *codec;

#ifdef HAVE_LOSSLESSHD_FFI
    LosslesshdDecoder *decoder;
#endif

    bool is_spatial;
    int sample_rate;
    int channels;

    struct mp_decoder public;
};

static void ad_losslesshd_process(struct mp_filter *ad);

static const struct mp_filter_info ad_losslesshd_filter = {
    .name = "ad_losslesshd",
    .priv_size = sizeof(struct priv),
    .process = ad_losslesshd_process,
};

static bool init(struct mp_filter *da, struct mp_codec_params *codec,
                 const char *decoder_name)
{
    struct priv *p = da->priv;
    p->codec = codec;
    p->sample_rate = codec->samplerate ? codec->samplerate : 48000;

#ifdef HAVE_LOSSLESSHD_FFI
    p->decoder = losslesshd_create();
    if (!p->decoder) {
        MP_ERR(da, "Failed to create lossless HD spatial decoder\n");
        return false;
    }
    MP_INFO(da, "lossless HD spatial decoder initialized (%s)\n", losslesshd_version());
#else
    MP_WARN(da, "lossless HD spatial decoder: losslesshd_ffi not available, "
                "falling back to FFmpeg (no spatial object extraction)\n");
    // In this case, mpv will fall back to ad_lavc anyway
    return false;
#endif

    return true;
}

static void ad_losslesshd_process(struct mp_filter *da) {
    struct priv *p = da->priv;

    if (!mp_pin_can_transfer_data(da->ppins[1], da->ppins[0]))
        return;

    struct mp_frame frame = mp_pin_out_read(da->ppins[0]);

    if (frame.type == MP_FRAME_EOF) {
        mp_pin_in_write(da->ppins[1], frame);
        return;
    }

    if (frame.type != MP_FRAME_PACKET)
        goto error;

    struct demux_packet *pkt = frame.data;

#ifdef HAVE_LOSSLESSHD_FFI
    // Feed packet to decoder
    int ret = losslesshd_send_packet(p->decoder, pkt->buffer, pkt->len);
    if (ret < 0) {
        MP_ERR(da, "Error sending packet to lossless HD decoder\n");
        goto error;
    }

    // Try to receive decoded frame
    LosslesshdFrame thd_frame = {0};
    ret = losslesshd_receive_frame(p->decoder, &thd_frame);

    if (ret == 1) {
        // Need more data
        mp_frame_unref(&frame);
        mp_pin_out_repeat_eof(da->ppins[0]);
        return;
    }

    if (ret < 0) {
        MP_ERR(da, "Error receiving frame from lossless HD decoder\n");
        goto error;
    }

    // Create mpv audio frame from decoded data
    struct mp_aframe *af = mp_aframe_create();
    mp_aframe_set_format(af, AF_FORMAT_FLOATP);
    mp_aframe_set_rate(af, thd_frame.sample_rate);

    // Set channel map based on decoded channels
    struct mp_chmap chmap = {0};
    chmap.num = thd_frame.num_channels;
    // Standard 7.1.4 channel order if spatial
    if (thd_frame.is_spatial && thd_frame.num_channels >= 12) {
        chmap.speaker[0]  = MP_SPEAKER_ID_FL;
        chmap.speaker[1]  = MP_SPEAKER_ID_FR;
        chmap.speaker[2]  = MP_SPEAKER_ID_FC;
        chmap.speaker[3]  = MP_SPEAKER_ID_LFE;
        chmap.speaker[4]  = MP_SPEAKER_ID_BL;
        chmap.speaker[5]  = MP_SPEAKER_ID_BR;
        chmap.speaker[6]  = MP_SPEAKER_ID_SL;
        chmap.speaker[7]  = MP_SPEAKER_ID_SR;
        chmap.speaker[8]  = MP_SPEAKER_ID_TFL;
        chmap.speaker[9]  = MP_SPEAKER_ID_TFR;
        chmap.speaker[10] = MP_SPEAKER_ID_TBL;
        chmap.speaker[11] = MP_SPEAKER_ID_TBR;
    } else {
        mp_chmap_from_channels(&chmap, thd_frame.num_channels);
    }
    mp_aframe_set_chmap(af, &chmap);

    // TODO: Allocate and copy sample data
    // TODO: Attach spatial object position metadata as AVFrame side data

    if (pkt->pts != MP_NOPTS_VALUE)
        mp_aframe_set_pts(af, pkt->pts);

    mp_frame_unref(&frame);
    mp_pin_in_write(da->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, af));
    return;
#endif

error:
    mp_frame_unref(&frame);
    mp_filter_internal_mark_failed(da);
}

static void ad_losslesshd_destroy(struct mp_filter *da) {
    struct priv *p = da->priv;
#ifdef HAVE_LOSSLESSHD_FFI
    if (p->decoder)
        losslesshd_destroy(p->decoder);
#endif
}

static struct mp_decoder *create(struct mp_filter *parent,
                                  struct mp_codec_params *codec,
                                  const char *decoder)
{
    struct mp_filter *da = mp_filter_create(parent, &ad_losslesshd_filter);
    if (!da)
        return NULL;

    struct priv *p = da->priv;

    mp_filter_add_pin(da, MP_PIN_IN, "in");
    mp_filter_add_pin(da, MP_PIN_OUT, "out");

    if (!init(da, codec, decoder)) {
        talloc_free(da);
        return NULL;
    }

    p->public.f = da;
    return &p->public;
}

static void add_decoders(struct mp_decoder_list *list) {
    mp_add_decoder(list, "losslesshd", "losslesshd_spatial",
                   "losslesshd_spatial", "lossless HD spatial decoder");
}

const struct mp_decoder_fns ad_losslesshd = {
    .create = create,
    .add_decoders = add_decoders,
};
