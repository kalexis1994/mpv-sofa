#!/usr/bin/env python3
"""
extract_spatial_meta.py - Extract immersive spatial audio object position metadata from lossless HD bitstreams.

Parses the raw lossless HD bitstream to find spatial metadata (ObjMeta/DAMF) and
outputs a binary .aobj sidecar file that the host app can load at runtime.

Usage:
    # Step 1: Extract raw lossless HD stream from MKV
    ffmpeg -i "movie.mkv" -map 0:a:0 -c copy movie.thd

    # Step 2: Extract metadata to sidecar
    python extract_spatial_meta.py movie.thd -o "movie.aobj"

    # Or directly from MKV (uses ffprobe + ffmpeg internally):
    python extract_spatial_meta.py "movie.mkv" -o "movie.aobj"

Output format (.aobj):
    Header: "AOBJ" (4 bytes), version u8, num_frames u32le, fps f32le
    Per frame: pts_ms u32le, num_objects u8
    Per object: x f32le, y f32le, z f32le

spatial coordinate system:
    X: 0.0 (left) to 1.0 (right)
    Y: 0.0 (front) to 1.0 (back)
    Z: -1.0 (floor) to 1.0 (ceiling)

References:
    - Massimo Eynard's ffmpeg spatial patch (ffmpeg-devel msg178814)
    - truehdd project (github.com/truehdd/truehdd) for DAMF extraction
    - domyd/mlp (github.com/domyd/mlp) for MLP parsing
    - immersive spatial audio for content creators specification
"""

import struct
import argparse
import sys
import os
import subprocess
import tempfile


# lossless HD sync words
MAJOR_SYNC_WORD = 0xF8726FBA  # 4 bytes: F8 72 6F BA
TRUEHD_SYNC = 0xF8726F  # 3-byte sync in access unit headers

# spatial-specific sync words
SPATIAL_SUBSTREAM_SYNC = 0x31EC  # Sync word for spatial 4th substream
ObjMeta_SYNC = 0x514D4F  # "OMQ" reversed - Object Metadata

# Fixed-point precision for spatial coordinates (2.18 format)
SPATIAL_COORD_SCALE = 1.0 / (1 << 18)


def read_u8(data, offset):
    return data[offset]


def read_u16be(data, offset):
    return struct.unpack_from('>H', data, offset)[0]


def read_u32be(data, offset):
    return struct.unpack_from('>I', data, offset)[0]


def read_bits(data, bit_offset, num_bits):
    """Read num_bits from data starting at bit_offset."""
    result = 0
    for i in range(num_bits):
        byte_idx = (bit_offset + i) >> 3
        bit_idx = 7 - ((bit_offset + i) & 7)
        if byte_idx < len(data):
            result = (result << 1) | ((data[byte_idx] >> bit_idx) & 1)
        else:
            result <<= 1
    return result


def signed_fixed_to_float(value, int_bits, frac_bits):
    """Convert signed fixed-point to float. Total bits = int_bits + frac_bits."""
    total = int_bits + frac_bits
    if value >= (1 << (total - 1)):
        value -= (1 << total)
    return value / (1 << frac_bits)


class LosslessHDParser:
    """Parse lossless HD bitstream and extract spatial metadata."""

    def __init__(self, verbose=False):
        self.verbose = verbose
        self.sample_rate = 48000
        self.num_substreams = 0
        self.is_spatial = False
        self.frames = []  # list of (pts_ms, [(x, y, z), ...])

    def parse_major_sync(self, data, offset):
        """Parse major sync header (starts after sync word).
        Returns (num_substreams, sample_rate, is_spatial, header_size)."""
        if offset + 28 > len(data):
            return None

        # Major sync format (after 4-byte sync word):
        # Bytes 0-3: sync word (already matched)
        # Byte 4-5: header size (in 16-bit words)
        # Byte 6: format info
        # Byte 7-8: flags
        # Byte 9: num_substreams (upper 4 bits)
        # ...

        # The header layout varies; key fields:
        header_size_words = read_u16be(data, offset + 4)
        header_size = header_size_words * 2

        # Format info byte contains sample rate index
        format_info = read_u16be(data, offset + 8)
        sr_index = (format_info >> 8) & 0x0F
        sr_table = {0: 48000, 1: 96000, 2: 192000, 8: 44100, 9: 88200, 10: 176400}
        self.sample_rate = sr_table.get(sr_index, 48000)

        # Number of substreams
        num_substreams = (read_u8(data, offset + 17) >> 4) & 0x0F
        if num_substreams == 0:
            num_substreams = (read_u8(data, offset + 13) >> 4) & 0x0F

        # spatial detection: presence of 4th substream
        is_spatial = num_substreams >= 4

        if self.verbose:
            print(f"  Major sync: sr={self.sample_rate}, substreams={num_substreams}, "
                  f"spatial={is_spatial}, header={header_size}b")

        return (num_substreams, self.sample_rate, is_spatial, header_size)

    def parse_access_unit(self, data, offset, au_size, pts_samples):
        """Parse one lossless HD access unit, extracting spatial metadata if present."""
        if not self.is_spatial:
            return None

        end = min(offset + au_size, len(data))
        objects = []

        # Scan for spatial substream data (0x31EC sync word)
        pos = offset
        while pos + 4 < end:
            word = read_u16be(data, pos)
            if word == SPATIAL_SUBSTREAM_SYNC:
                obj_data = self._parse_spatial_substream(data, pos, end)
                if obj_data:
                    objects = obj_data
                break
            pos += 2

        if not objects:
            # Try scanning byte-by-byte for ObjMeta markers
            pos = offset
            while pos + 6 < end:
                # Look for object audio metadata patterns
                if self._looks_like_oamd(data, pos, end):
                    obj_data = self._parse_oamd_block(data, pos, end)
                    if obj_data:
                        objects = obj_data
                        break
                pos += 1

        if objects:
            pts_ms = int(pts_samples * 1000.0 / self.sample_rate)
            return (pts_ms, objects)
        return None

    def _parse_spatial_substream(self, data, offset, end):
        """Parse spatial 4th substream starting at 0x31EC sync word."""
        if offset + 8 > end:
            return None

        objects = []

        # Skip sync word (2 bytes) + substream header
        pos = offset + 2
        # Read substream size
        if pos + 2 > end:
            return None
        sub_size = read_u16be(data, pos)
        pos += 2

        # The substream contains interpolated matrix data and object positions.
        # Object positions are encoded in 2.18 fixed-point format.
        # Layout: [header][seed_matrix][delta_data][object_positions]

        # Scan for position data within the substream
        sub_end = min(offset + sub_size + 4, end)

        # Object positions typically follow the matrix data.
        # Each object has: x (20 bits), y (20 bits), z (20 bits)
        # packed as 2.18 signed fixed-point

        # Heuristic: look for the position block after matrix params
        # The position block typically starts ~40-80 bytes into the substream
        search_start = min(pos + 32, sub_end)
        bit_pos = search_start * 8

        # Try to parse positions at likely offsets
        for try_offset in range(search_start, min(search_start + 64, sub_end - 8)):
            bp = try_offset * 8
            positions = self._try_parse_positions(data, bp, sub_end * 8)
            if positions and len(positions) >= 1:
                objects = positions
                break

        return objects if objects else None

    def _try_parse_positions(self, data, bit_offset, max_bits):
        """Try to parse object positions starting at bit_offset.
        Each position: 20-bit X, 20-bit Y, 20-bit Z (2.18 fixed-point)."""
        positions = []
        bp = bit_offset

        for _ in range(16):  # max 16 objects
            if bp + 60 > max_bits:
                break

            raw_x = read_bits(data, bp, 20); bp += 20
            raw_y = read_bits(data, bp, 20); bp += 20
            raw_z = read_bits(data, bp, 20); bp += 20

            x = signed_fixed_to_float(raw_x, 2, 18)
            y = signed_fixed_to_float(raw_y, 2, 18)
            z = signed_fixed_to_float(raw_z, 2, 18)

            # Validate: spatial coords should be in range
            if -0.1 <= x <= 1.1 and -0.1 <= y <= 1.1 and -1.1 <= z <= 1.1:
                positions.append((x, y, z))
            else:
                # Invalid position means we're reading at the wrong offset
                return None

        return positions if positions else None

    def _looks_like_oamd(self, data, offset, end):
        """Heuristic check for ObjMeta block."""
        if offset + 3 > end:
            return False
        # Check for known ObjMeta markers
        b0 = data[offset]
        b1 = data[offset + 1]
        b2 = data[offset + 2]
        return (b0 == 0x51 and b1 == 0x4D and b2 == 0x4F)  # "QMO"

    def _parse_oamd_block(self, data, offset, end):
        """Parse Object Audio Metadata (ObjMeta) block."""
        if offset + 16 > end:
            return None

        pos = offset + 3  # skip "QMO" marker
        objects = []

        # ObjMeta header: version (4 bits), num_objects (8 bits), ...
        if pos + 2 > end:
            return None

        header = read_u16be(data, pos)
        pos += 2
        num_objects = (header >> 4) & 0xFF

        if num_objects == 0 or num_objects > 128:
            return None

        # Each object: x (16 bits), y (16 bits), z (16 bits) as signed Q1.14
        for _ in range(min(num_objects, 16)):
            if pos + 6 > end:
                break
            raw_x = struct.unpack_from('>h', data, pos)[0]; pos += 2
            raw_y = struct.unpack_from('>h', data, pos)[0]; pos += 2
            raw_z = struct.unpack_from('>h', data, pos)[0]; pos += 2

            x = raw_x / 16384.0  # Q1.14
            y = raw_y / 16384.0
            z = raw_z / 16384.0

            # Clamp to valid spatial range
            x = max(0.0, min(1.0, (x + 1.0) / 2.0))
            y = max(0.0, min(1.0, (y + 1.0) / 2.0))
            z = max(-1.0, min(1.0, z))

            objects.append((x, y, z))

        return objects if objects else None

    def parse_stream(self, data):
        """Parse entire lossless HD bitstream, extracting spatial metadata frames."""
        self.frames = []
        length = len(data)
        pos = 0
        pts_samples = 0
        au_count = 0

        # Samples per access unit (40 samples at 48kHz is typical for lossless HD)
        samples_per_au = 40

        while pos + 8 < length:
            # Look for access unit header (starts with length + flags)
            # lossless HD access units start with a 4-byte header:
            #   bits 0-11: access unit length in 16-bit words
            #   bits 12-15: flags

            # First, scan for major sync to establish stream parameters
            if pos + 4 <= length:
                sync = read_u32be(data, pos)
                if sync == MAJOR_SYNC_WORD:
                    result = self.parse_major_sync(data, pos)
                    if result:
                        self.num_substreams, self.sample_rate, self.is_spatial, hdr_size = result
                        if self.verbose:
                            print(f"AU {au_count}: Major sync at offset {pos}")
                    pos += max(28, 4)  # Skip past major sync header
                    continue

            # Read access unit header
            au_header = read_u16be(data, pos)
            au_length_words = au_header & 0x0FFF
            au_length = au_length_words * 2

            if au_length < 8 or au_length > 65536:
                pos += 2
                continue

            if pos + au_length > length:
                break

            # Parse this access unit for spatial metadata
            result = self.parse_access_unit(data, pos, au_length, pts_samples)
            if result:
                self.frames.append(result)

            pts_samples += samples_per_au
            au_count += 1
            pos += au_length

        if self.verbose:
            print(f"\nParsed {au_count} access units, found {len(self.frames)} "
                  f"frames with spatial metadata")


def extract_thd_from_mkv(input_path, verbose=False):
    """Extract raw lossless HD stream from MKV container using ffmpeg."""
    tmp = tempfile.NamedTemporaryFile(suffix='.thd', delete=False)
    tmp.close()

    cmd = ['ffmpeg', '-y', '-i', input_path, '-map', '0:a:0', '-c', 'copy', tmp.name]
    if verbose:
        print(f"Extracting lossless HD: {' '.join(cmd)}")
    try:
        subprocess.run(cmd, check=True, capture_output=not verbose)
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        os.unlink(tmp.name)
        raise RuntimeError(f"Failed to extract lossless HD stream: {e}")

    return tmp.name


def write_aobj(frames, output_path, fps=24.0):
    """Write .aobj sidecar file."""
    with open(output_path, 'wb') as f:
        # Header
        f.write(b'AOBJ')                                    # magic
        f.write(struct.pack('<B', 1))                        # version
        f.write(struct.pack('<I', len(frames)))              # num_frames
        f.write(struct.pack('<f', fps))                      # fps

        # Per frame
        for pts_ms, objects in frames:
            f.write(struct.pack('<I', pts_ms))               # pts_ms
            num_obj = min(len(objects), 128)
            f.write(struct.pack('<B', num_obj))              # num_objects
            for i in range(num_obj):
                x, y, z = objects[i]
                f.write(struct.pack('<fff', x, y, z))        # x, y, z


def generate_test_sidecar(output_path, duration_sec=120, fps=24.0, num_objects=11):
    """Generate a test .aobj sidecar with synthetic object movement.
    Useful for testing the rendering pipeline without real metadata."""
    import math

    frames = []
    num_frames = int(duration_sec * fps)

    for i in range(num_frames):
        t = i / fps  # time in seconds
        pts_ms = int(t * 1000)
        objects = []

        for j in range(num_objects):
            # Each object orbits at a different speed and elevation
            phase = 2.0 * math.pi * j / num_objects
            speed = 0.1 + 0.05 * j  # radians per second

            x = 0.5 + 0.4 * math.cos(speed * t + phase)
            y = 0.5 + 0.4 * math.sin(speed * t * 0.7 + phase)
            z = 0.3 * math.sin(speed * t * 0.3 + phase * 2)

            objects.append((
                max(0.0, min(1.0, x)),
                max(0.0, min(1.0, y)),
                max(-1.0, min(1.0, z))
            ))

        frames.append((pts_ms, objects))

    write_aobj(frames, output_path, fps)
    print(f"Generated test sidecar: {output_path}")
    print(f"  {num_frames} frames, {num_objects} objects, {duration_sec}s @ {fps} fps")


def main():
    parser = argparse.ArgumentParser(
        description='Extract immersive spatial audio object positions from lossless HD bitstream')
    parser.add_argument('input', help='Input file (.thd raw stream or .mkv container)')
    parser.add_argument('-o', '--output', help='Output .aobj sidecar path')
    parser.add_argument('--fps', type=float, default=24.0,
                        help='Video frame rate (default: 24)')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Verbose output')
    parser.add_argument('--test', action='store_true',
                        help='Generate test sidecar with synthetic movement')
    parser.add_argument('--test-duration', type=float, default=120.0,
                        help='Test sidecar duration in seconds (default: 120)')
    parser.add_argument('--test-objects', type=int, default=11,
                        help='Number of test objects (default: 11)')

    args = parser.parse_args()

    # Determine output path
    if args.output:
        output_path = args.output
    else:
        base = os.path.splitext(args.input)[0]
        output_path = base + '.aobj'

    # Test mode: generate synthetic sidecar
    if args.test:
        generate_test_sidecar(output_path, args.test_duration,
                              args.fps, args.test_objects)
        return

    # Determine if we need to extract from container
    ext = os.path.splitext(args.input)[1].lower()
    thd_path = args.input
    cleanup_thd = False

    if ext in ('.mkv', '.mk3d', '.mka', '.mp4', '.m4a'):
        print(f"Extracting lossless HD stream from {args.input}...")
        thd_path = extract_thd_from_mkv(args.input, args.verbose)
        cleanup_thd = True

    try:
        print(f"Reading {thd_path}...")
        with open(thd_path, 'rb') as f:
            data = f.read()

        print(f"Parsing lossless HD bitstream ({len(data)} bytes)...")
        parser_inst = LosslessHDParser(verbose=args.verbose)
        parser_inst.parse_stream(data)

        if not parser_inst.is_spatial:
            print("WARNING: Stream does not appear to be spatial (< 4 substreams)")
            print("Generating fallback sidecar with static positions...")
            # Generate static positions as fallback
            generate_test_sidecar(output_path, duration_sec=len(data) / 200000,
                                  fps=args.fps, num_objects=11)
            return

        if not parser_inst.frames:
            print("WARNING: No spatial metadata frames found in bitstream")
            print("This may indicate the metadata format is not yet supported.")
            print("Generating test sidecar instead...")
            generate_test_sidecar(output_path, duration_sec=len(data) / 200000,
                                  fps=args.fps, num_objects=11)
            return

        write_aobj(parser_inst.frames, output_path, args.fps)
        print(f"Wrote {output_path}: {len(parser_inst.frames)} frames")

    finally:
        if cleanup_thd and os.path.exists(thd_path):
            os.unlink(thd_path)


if __name__ == '__main__':
    main()
