//! C FFI wrapper for losslesshd lossless HD spatial decoder.
//!
//! Provides a simple C-compatible interface for:
//! 1. Feeding raw lossless HD bitstream packets
//! 2. Receiving decoded multichannel PCM
//! 3. Receiving spatial object metadata (positions)

use std::ffi::c_int;
use std::ptr;
use std::slice;

/// Maximum channels in decoded output
const MAX_CHANNELS: usize = 16;
/// Maximum spatial objects
const MAX_OBJECTS: usize = 128;

/// spatial object position
#[repr(C)]
pub struct LosslesshdObjectPos {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub size: f32,
    pub valid: c_int,
}

/// Decoded audio frame
#[repr(C)]
pub struct LosslesshdFrame {
    /// Per-channel sample data (planar float32)
    pub channels: [*mut f32; MAX_CHANNELS],
    /// Number of channels with valid data
    pub num_channels: c_int,
    /// Number of samples per channel
    pub num_samples: c_int,
    /// Sample rate
    pub sample_rate: c_int,
    /// Presentation timestamp (in timebase units)
    pub pts: i64,

    /// spatial object positions for this frame
    pub objects: [LosslesshdObjectPos; MAX_OBJECTS],
    /// Number of valid objects
    pub num_objects: c_int,

    /// Whether this is an spatial stream (vs regular lossless HD)
    pub is_spatial: c_int,
}

/// Opaque decoder context
pub struct LosslesshdDecoder {
    // TODO: Replace with actual losslesshd crate decoder when API stabilizes
    // For now this is a skeleton that falls back to providing the raw PCM
    // from the standard lossless HD bed while the losslesshd crate matures.
    buffer: Vec<u8>,
    sample_rate: u32,
    initialized: bool,
}

impl LosslesshdDecoder {
    fn new() -> Self {
        LosslesshdDecoder {
            buffer: Vec::with_capacity(65536),
            sample_rate: 48000,
            initialized: false,
        }
    }
}

/// Create a new lossless HD spatial decoder instance.
/// Returns an opaque pointer. Must be freed with `losslesshd_destroy`.
#[no_mangle]
pub extern "C" fn losslesshd_create() -> *mut LosslesshdDecoder {
    let decoder = Box::new(LosslesshdDecoder::new());
    Box::into_raw(decoder)
}

/// Destroy a decoder instance.
#[no_mangle]
pub unsafe extern "C" fn losslesshd_destroy(ctx: *mut LosslesshdDecoder) {
    if !ctx.is_null() {
        drop(Box::from_raw(ctx));
    }
}

/// Feed raw lossless HD bitstream data to the decoder.
///
/// `data`: pointer to raw lossless HD packet data
/// `size`: size in bytes
///
/// Returns 0 on success, negative on error.
#[no_mangle]
pub unsafe extern "C" fn losslesshd_send_packet(
    ctx: *mut LosslesshdDecoder,
    data: *const u8,
    size: c_int,
) -> c_int {
    if ctx.is_null() || data.is_null() || size <= 0 {
        return -1;
    }

    let decoder = &mut *ctx;
    let packet = slice::from_raw_parts(data, size as usize);

    // Buffer the packet data
    decoder.buffer.extend_from_slice(packet);

    // TODO: When losslesshd crate API is ready:
    // decoder.inner.send_packet(packet);

    0
}

/// Receive a decoded frame.
///
/// `frame`: pointer to a LosslesshdFrame to fill in.
///
/// Returns:
///   0  = frame available
///   1  = need more input (call send_packet)
///  -1  = error
#[no_mangle]
pub unsafe extern "C" fn losslesshd_receive_frame(
    ctx: *mut LosslesshdDecoder,
    frame: *mut LosslesshdFrame,
) -> c_int {
    if ctx.is_null() || frame.is_null() {
        return -1;
    }

    let _decoder = &mut *ctx;
    let frame = &mut *frame;

    // Initialize frame to zero
    frame.num_channels = 0;
    frame.num_samples = 0;
    frame.num_objects = 0;
    frame.is_spatial = 0;

    // TODO: Implement actual lossless HD spatial decoding using the losslesshd crate.
    // The losslesshd crate (from the losslesshd project) is still maturing its API.
    // When ready, this function will:
    //
    // 1. Decode the buffered lossless HD bitstream
    // 2. Extract the 7.1 bed channels (always present)
    // 3. If spatial: extract up to 16 object audio channels
    // 4. If spatial: extract 3D positions (x,y,z) for each object
    // 5. Fill the LosslesshdFrame with this data
    //
    // For the MVP, the host app will use FFmpeg (via mpv's ad_lavc) for
    // basic lossless HD decoding (7.1 bed) and the af_hrtf filter for
    // spatialization. The losslesshd decoder will be swapped in when the
    // crate API is stable enough for real-time streaming decode.

    // Signal "need more data" for now
    1
}

/// Get the version string.
#[no_mangle]
pub extern "C" fn losslesshd_version() -> *const std::ffi::c_char {
    b"losslesshd_ffi 0.1.0 (skeleton)\0".as_ptr() as *const std::ffi::c_char
}
