"""
Verify the thd_channel_order mapping issue.
Show what combined_mask includes and what gets zeroed.
"""
import subprocess
import os

FFMPEG = 'F:/hrtf/ffmpeg-build/bin/ffmpeg.exe'
MOVIE = 'C:/Users/Alexis/Downloads/movies/The.Lord.of.the.Rings.The.Two.Towers2002.mkv'

# Run FFmpeg with verbose logging to see the channel mapping
print("Running FFmpeg with extract_objects and debug logging...")
result = subprocess.run(
    [FFMPEG, '-ss', '123', '-t', '1', '-extract_objects', '1',
     '-i', MOVIE, '-map', '0:5', '-vn',
     '-acodec', 'pcm_f32le', '-f', 'f32le', '-ar', '48000', '-ac', '16',
     '-loglevel', 'debug',
     '/dev/null'],
    capture_output=True, text=True, timeout=30
)

# Filter for interesting lines
stderr = result.stderr
for line in stderr.split('\n'):
    if any(kw in line.lower() for kw in ['atmos', 'spatial', 'output_data', 'ch_assign',
                                          'matrix_ch', 'max_channel', 'max_matrix',
                                          'blockpos', 'mask', 'combined', 'bed',
                                          'anomaly', 'channel_layout', 'restart',
                                          'noise_type', 'truehd', 'mlp']):
        print(line)
