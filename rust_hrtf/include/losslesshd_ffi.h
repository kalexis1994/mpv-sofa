#ifndef LOSSLESSHD_FFI_H
#define LOSSLESSHD_FFI_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Opaque decoder context
 */
typedef struct ThdDecoder ThdDecoder;

/**
 * Object position from OAMD metadata
 */
typedef struct ThdObjectPos {
  /**
   * Position X: 0.0=left, 0.5=center, 1.0=right
   */
  float x;
  /**
   * Position Y: 0.0=front, 0.5=center, 1.0=back
   */
  float y;
  /**
   * Position Z: 0.0=bottom, 1.0=top
   */
  float z;
  /**
   * Object gain in dB (-128 = muted)
   */
  int8_t gain_db;
  /**
   * 1 if object is active this frame
   */
  int active;
  /**
   * Object size (0=point source)
   */
  float size;
} ThdObjectPos;

/**
 * Decoded audio frame with bed + objects
 */
typedef struct ThdDecodedFrame {
  /**
   * Interleaved bed PCM samples (24-bit in i32): [sample][channel]
   * Bed channels: FL,FR,FC,LFE,BL,BR,SL,SR (up to 8)
   */
  int32_t bed_samples[MAX_SAMPLES_PER_AU][8];
  /**
   * Number of bed channels (2, 6, or 8)
   */
  int bed_channels;
  /**
   * Number of valid bed samples
   */
  int bed_sample_count;
  /**
   * Object PCM samples: [sample][object]
   * Each object is a mono audio channel
   */
  int32_t obj_samples[MAX_SAMPLES_PER_AU][MAX_OBJECTS];
  /**
   * Number of active objects
   */
  int obj_count;
  /**
   * Object positions from OAMD
   */
  struct ThdObjectPos obj_pos[MAX_OBJECTS];
  /**
   * Sample rate (48000)
   */
  int sample_rate;
  /**
   * 1 if Atmos content detected
   */
  int is_atmos;
  /**
   * 1 if this is a duplicate frame (should be skipped)
   */
  int is_duplicate;
  /**
   * OAMD ramp duration in samples
   */
  int ramp_duration;
} ThdDecodedFrame;

/**
 * Create a new TrueHD/Atmos decoder instance.
 */
struct ThdDecoder *thd_decoder_create(void);

/**
 * Destroy a decoder instance.
 */
void thd_decoder_destroy(struct ThdDecoder *ctx);

/**
 * Feed raw TrueHD bitstream data to the decoder.
 *
 * Returns 0 on success, -1 on error.
 */
int thd_decoder_send_packet(struct ThdDecoder *ctx, const uint8_t *data, int size);

/**
 * Receive a decoded frame with bed audio + object audio + positions.
 *
 * Returns:
 *   0 = frame available in `frame`
 *   1 = need more input (call send_packet)
 *  -1 = error
 */
int thd_decoder_receive_frame(struct ThdDecoder *ctx, struct ThdDecodedFrame *frame);

/**
 * Check if the stream contains Atmos content.
 * Only valid after at least one frame has been decoded.
 */
int thd_decoder_is_atmos(const struct ThdDecoder *ctx);

/**
 * Get version string.
 */
const char *thd_decoder_version(void);

#endif  /* LOSSLESSHD_FFI_H */
