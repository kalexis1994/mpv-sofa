//! C FFI wrapper for truehd TrueHD/Atmos decoder.
//!
//! Provides a C-compatible interface for real-time decoding of TrueHD Atmos
//! bitstreams into separated bed channels + individual object audio with
//! 3D position metadata (OAMD).
//!
//! Architecture:
//!   Raw TrueHD packets → Extractor → Parser → Decoder
//!     → Bed PCM (presentation 0: 2/6/8 ch)
//!     → Object PCM (presentation 3: N objects)
//!     → OAMD positions per object

use std::ffi::c_int;
use std::slice;

use truehd::process::decode::{DecodedAccessUnit, Decoder};
use truehd::process::extract::Extractor;
use truehd::process::parse::Parser;

/// Maximum channels in decoded output (bed + objects)
const MAX_CHANNELS: usize = 24;
/// Maximum spatial objects
const MAX_OBJECTS: usize = 16;
/// Samples per access unit (TrueHD max)
const MAX_SAMPLES_PER_AU: usize = 160;

/// Object position from OAMD metadata
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ThdObjectPos {
    /// Position X: 0.0=left, 0.5=center, 1.0=right
    pub x: f32,
    /// Position Y: 0.0=front, 0.5=center, 1.0=back
    pub y: f32,
    /// Position Z: 0.0=bottom, 1.0=top
    pub z: f32,
    /// Object gain in dB (-128 = muted)
    pub gain_db: i8,
    /// 1 if object is active this frame
    pub active: c_int,
    /// Object size (0=point source)
    pub size: f32,
}

/// Decoded audio frame with bed + objects
#[repr(C)]
pub struct ThdDecodedFrame {
    /// Interleaved bed PCM samples (24-bit in i32): [sample][channel]
    /// Bed channels: FL,FR,FC,LFE,BL,BR,SL,SR (up to 8)
    pub bed_samples: [[i32; 8]; MAX_SAMPLES_PER_AU],
    /// Number of bed channels (2, 6, or 8)
    pub bed_channels: c_int,
    /// Number of valid bed samples
    pub bed_sample_count: c_int,

    /// Object PCM samples: [sample][object]
    /// Each object is a mono audio channel
    pub obj_samples: [[i32; MAX_OBJECTS]; MAX_SAMPLES_PER_AU],
    /// Number of active objects
    pub obj_count: c_int,

    /// Object positions from OAMD
    pub obj_pos: [ThdObjectPos; MAX_OBJECTS],

    /// Sample rate (48000)
    pub sample_rate: c_int,
    /// 1 if Atmos content detected
    pub is_atmos: c_int,
    /// 1 if this is a duplicate frame (should be skipped)
    pub is_duplicate: c_int,
    /// OAMD ramp duration in samples
    pub ramp_duration: c_int,
}

impl Default for ThdDecodedFrame {
    fn default() -> Self {
        Self {
            bed_samples: [[0; 8]; MAX_SAMPLES_PER_AU],
            bed_channels: 0,
            bed_sample_count: 0,
            obj_samples: [[0; MAX_OBJECTS]; MAX_SAMPLES_PER_AU],
            obj_count: 0,
            obj_pos: [ThdObjectPos::default(); MAX_OBJECTS],
            sample_rate: 48000,
            is_atmos: 0,
            is_duplicate: 0,
            ramp_duration: 0,
        }
    }
}

/// Opaque decoder context
pub struct ThdDecoder {
    extractor: Extractor,
    parser: Parser,
    decoder_bed: Decoder,
    decoder_atmos: Decoder,
    is_atmos: bool,
    /// Buffered decoded frames (bed + atmos paired)
    pending_bed: Vec<DecodedAccessUnit>,
    pending_atmos: Vec<DecodedAccessUnit>,
}

impl ThdDecoder {
    fn new() -> Self {
        ThdDecoder {
            extractor: Extractor::default(),
            parser: Parser::default(),
            decoder_bed: Decoder::default(),
            decoder_atmos: Decoder::default(),
            is_atmos: false,
            pending_bed: Vec::new(),
            pending_atmos: Vec::new(),
        }
    }

    fn process_buffered(&mut self) {
        // Process all available frames from the extractor
        while let Some(frame_result) = self.extractor.next() {
            match frame_result {
                Ok(frame) => {
                    match self.parser.parse(&frame) {
                        Ok(access_unit) => {
                            // Decode presentation 0 (bed: stereo/5.1/7.1)
                            match self.decoder_bed.decode_presentation(&access_unit, 0) {
                                Ok(decoded) => {
                                    self.pending_bed.push(decoded);
                                }
                                Err(e) => {
                                    log::warn!("Bed decode error: {}", e);
                                }
                            }

                            // Try presentation 3 (Atmos objects)
                            match self.decoder_atmos.decode_presentation(&access_unit, 3) {
                                Ok(decoded) => {
                                    self.is_atmos = true;
                                    self.pending_atmos.push(decoded);
                                }
                                Err(_) => {
                                    // Not Atmos or decode error - no objects
                                }
                            }
                        }
                        Err(e) => {
                            log::warn!("Parse error: {}", e);
                        }
                    }
                }
                Err(e) => {
                    log::warn!("Extract error: {}", e);
                }
            }
        }
    }

    fn fill_frame(&mut self, out: &mut ThdDecodedFrame) -> bool {
        if self.pending_bed.is_empty() {
            return false;
        }

        let bed = self.pending_bed.remove(0);

        // Fill bed audio
        out.bed_channels = bed.channel_count as c_int;
        out.bed_sample_count = bed.sample_length as c_int;
        out.sample_rate = bed.sampling_frequency as c_int;
        out.is_duplicate = if bed.is_duplicate { 1 } else { 0 };

        for s in 0..bed.sample_length.min(MAX_SAMPLES_PER_AU) {
            for ch in 0..bed.channel_count.min(8) {
                out.bed_samples[s][ch] = bed.pcm_data[s][ch];
            }
        }

        // Fill object audio + positions if Atmos
        if !self.pending_atmos.is_empty() {
            let atmos = self.pending_atmos.remove(0);
            out.is_atmos = 1;

            let obj_count = atmos.channel_count.min(MAX_OBJECTS);
            out.obj_count = obj_count as c_int;

            for s in 0..atmos.sample_length.min(MAX_SAMPLES_PER_AU) {
                for obj in 0..obj_count {
                    out.obj_samples[s][obj] = atmos.pcm_data[s][obj];
                }
            }

            // Extract OAMD positions
            for oamd in &atmos.oamd {
                if let Some(ref obj_elem) = oamd.object_element {
                    // object_data is Vec<Vec<ObjectInfoBlock>>
                    // Each inner Vec has blocks for one object
                    for (i, obj_blocks) in obj_elem.object_data.iter().enumerate() {
                        if i >= MAX_OBJECTS {
                            break;
                        }
                        // Get the last block's render info for current position
                        if let Some(last_block) = obj_blocks.last() {
                            let ri = &last_block.object_render_info;
                            out.obj_pos[i].x = ri.pos3d[0] as f32;
                            out.obj_pos[i].y = ri.pos3d[1] as f32;
                            out.obj_pos[i].z = ri.pos3d[2] as f32;
                            out.obj_pos[i].active = if last_block.b_object_not_active { 0 } else { 1 };
                            out.obj_pos[i].size = ri.object_size[0] as f32;
                            out.obj_pos[i].gain_db = last_block.object_basic_info.object_gain;
                        }
                    }
                    // Ramp duration from the first block update info
                    if let Some(bui) = obj_elem.md_update_info.block_update_info.first() {
                        out.ramp_duration = bui.ramp_duration as c_int;
                    }
                }
            }
        } else {
            out.is_atmos = 0;
            out.obj_count = 0;
        }

        true
    }
}

// === C FFI Functions ===

/// Create a new TrueHD/Atmos decoder instance.
#[no_mangle]
pub extern "C" fn thd_decoder_create() -> *mut ThdDecoder {
    let decoder = Box::new(ThdDecoder::new());
    Box::into_raw(decoder)
}

/// Destroy a decoder instance.
#[no_mangle]
pub unsafe extern "C" fn thd_decoder_destroy(ctx: *mut ThdDecoder) {
    if !ctx.is_null() {
        drop(Box::from_raw(ctx));
    }
}

/// Feed raw TrueHD bitstream data to the decoder.
///
/// Returns 0 on success, -1 on error.
#[no_mangle]
pub unsafe extern "C" fn thd_decoder_send_packet(
    ctx: *mut ThdDecoder,
    data: *const u8,
    size: c_int,
) -> c_int {
    if ctx.is_null() || data.is_null() || size <= 0 {
        return -1;
    }

    let decoder = &mut *ctx;
    let packet = slice::from_raw_parts(data, size as usize);

    decoder.extractor.push_bytes(packet);
    decoder.process_buffered();

    0
}

/// Receive a decoded frame with bed audio + object audio + positions.
///
/// Returns:
///   0 = frame available in `frame`
///   1 = need more input (call send_packet)
///  -1 = error
#[no_mangle]
pub unsafe extern "C" fn thd_decoder_receive_frame(
    ctx: *mut ThdDecoder,
    frame: *mut ThdDecodedFrame,
) -> c_int {
    if ctx.is_null() || frame.is_null() {
        return -1;
    }

    let decoder = &mut *ctx;
    let frame = &mut *frame;

    // Zero the frame
    *frame = ThdDecodedFrame::default();

    if decoder.fill_frame(frame) {
        0
    } else {
        1 // need more data
    }
}

/// Check if the stream contains Atmos content.
/// Only valid after at least one frame has been decoded.
#[no_mangle]
pub unsafe extern "C" fn thd_decoder_is_atmos(ctx: *const ThdDecoder) -> c_int {
    if ctx.is_null() {
        return 0;
    }
    if (*ctx).is_atmos { 1 } else { 0 }
}

/// Get version string.
#[no_mangle]
pub extern "C" fn thd_decoder_version() -> *const std::ffi::c_char {
    b"truehd_ffi 0.2.0 (truehd 0.4.0)\0".as_ptr() as *const std::ffi::c_char
}
