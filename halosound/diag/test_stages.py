"""
Test each stage of Atmos object extraction pipeline.
Generates WAV files for listening at each point.
Segment: LOTR Two Towers 2:03-2:10, Atmos track (stream 5)
"""
import numpy as np
import subprocess, os, struct

SR = 48000
D = os.path.dirname(os.path.abspath(__file__))
FFMPEG = 'F:/hrtf/ffmpeg-build/bin/ffmpeg.exe'
MOVIE = 'C:/Users/Alexis/Downloads/movies/The.Lord.of.the.Rings.The.Two.Towers2002.mkv'

def write_wav(filename, data, sr=48000, channels=1):
    """Write float32 numpy array to WAV (16-bit)."""
    if data.ndim == 1:
        data = data.reshape(-1, 1)
    ch = data.shape[1]
    # Clip and convert to int16
    data = np.clip(data, -1.0, 1.0)
    pcm = (data * 32767).astype(np.int16)
    path = os.path.join(D, filename)
    with open(path, 'wb') as f:
        n_samples = pcm.shape[0]
        data_size = n_samples * ch * 2
        f.write(b'RIFF')
        f.write(struct.pack('<I', 36 + data_size))
        f.write(b'WAVE')
        f.write(b'fmt ')
        f.write(struct.pack('<IHHIIHH', 16, 1, ch, sr, sr * ch * 2, ch * 2, 16))
        f.write(b'data')
        f.write(struct.pack('<I', data_size))
        f.write(pcm.tobytes())
    print(f"  -> {filename} ({os.path.getsize(path)//1024} KB)")

def load_pcm(path, channels):
    d = np.fromfile(path, dtype=np.float32)
    s = len(d) // channels
    return d[:s*channels].reshape(s, channels)

def declick(signal, threshold=0.10, lookahead=8):
    """Two-pass declicker: interpolate over jumps > threshold.
    Pass 1 at given threshold, pass 2 at threshold*1.5 to mop up."""
    out = signal.copy()
    for thresh in [threshold, threshold * 1.5]:
        prev = out[0]
        i = 1
        while i < len(out):
            if abs(out[i] - prev) > thresh:
                # Find end of glitch burst
                end = i + 1
                while end < len(out) and end < i + lookahead:
                    if abs(out[end] - out[end-1]) < thresh:
                        break
                    end += 1
                # Interpolate
                end_val = out[min(end, len(out)-1)]
                span = end - i + 1
                for j in range(i, min(end, len(out))):
                    t = (j - i + 1) / span
                    out[j] = prev * (1 - t) + end_val * t
                prev = out[min(end-1, len(out)-1)]
                i = end
            else:
                prev = out[i]
                i += 1
    return out

print("=" * 60)
print("Atmos Object Extraction - Stage-by-stage test")
print("LOTR Two Towers 2:03-2:10")
print("=" * 60)

# === Extract raw PCM ===
print("\n[1] Extracting 16ch with extract_objects...")
r16_path = os.path.join(D, 'raw_16ch.pcm')
r8_path = os.path.join(D, 'raw_8ch.pcm')

if not os.path.exists(r16_path):
    subprocess.run([FFMPEG, '-y', '-ss', '123', '-t', '7', '-extract_objects', '1',
                    '-i', MOVIE, '-map', '0:5', '-vn',
                    '-acodec', 'pcm_f32le', '-f', 'f32le', '-ar', str(SR), '-ac', '16',
                    r16_path], capture_output=True)
if not os.path.exists(r8_path):
    subprocess.run([FFMPEG, '-y', '-ss', '123', '-t', '7',
                    '-i', MOVIE, '-map', '0:5', '-vn',
                    '-acodec', 'pcm_f32le', '-f', 'f32le', '-ar', str(SR), '-ac', '8',
                    r8_path], capture_output=True)

r16 = load_pcm(r16_path, 16)
r8 = load_pcm(r8_path, 8)
print(f"  16ch: {r16.shape}, 8ch: {r8.shape}")

# === Stage A: Reference (standard 8ch, no extract_objects) ===
print("\n[A] Reference: standard 8ch bed downmix (CLEAN)")
bed8_L = r8[:,0] + r8[:,2]*0.707 + r8[:,4]*0.707 + r8[:,6]*0.707
bed8_R = r8[:,1] + r8[:,2]*0.707 + r8[:,5]*0.707 + r8[:,7]*0.707
mx = max(np.max(np.abs(bed8_L)), np.max(np.abs(bed8_R)), 0.001)
write_wav('A_reference_8ch_bed.wav', np.stack([bed8_L/mx*0.9, bed8_R/mx*0.9], axis=1), SR, 2)

# === Stage B: 16ch bed RAW (with artifacts) ===
print("\n[B] 16ch bed downmix RAW (HAS ARTIFACTS in BL/BR)")
bed16_L = r16[:,0] + r16[:,2]*0.707 + r16[:,4]*0.707 + r16[:,6]*0.707
bed16_R = r16[:,1] + r16[:,2]*0.707 + r16[:,5]*0.707 + r16[:,7]*0.707
mx = max(np.max(np.abs(bed16_L)), np.max(np.abs(bed16_R)), 0.001)
write_wav('B_16ch_bed_raw.wav', np.stack([bed16_L/mx*0.9, bed16_R/mx*0.9], axis=1), SR, 2)

# === Stage C: Individual problem channels ===
print("\n[C] Individual channels comparison")
# BL raw vs clean
write_wav('C1_BL_raw_extract_obj.wav', r16[:,4], SR, 1)
write_wav('C2_BL_clean_standard.wav', r8[:,4], SR, 1)

# === Stage D: Objects only (raw from extract_objects) ===
print("\n[D] Objects only (raw from extract_objects)")
# Obj0 and Obj1 are the only active ones
write_wav('D1_obj0_ch8.wav', r16[:,8] * 10, SR, 1)  # amplified 10x (very quiet)
write_wav('D2_obj1_ch9.wav', r16[:,9] * 10, SR, 1)
# All objects summed
obj_sum = np.sum(r16[:,8:], axis=1)
write_wav('D3_all_objects_sum.wav', obj_sum * 10, SR, 1)

# === Stage E: Declicked BL/BR ===
print("\n[E] Declicked BL and BR channels")
bl_fixed = declick(r16[:,4])
br_fixed = declick(r16[:,5])
write_wav('E1_BL_declicked.wav', bl_fixed, SR, 1)
write_wav('E2_BR_declicked.wav', br_fixed, SR, 1)

# === Stage F: Full 16ch bed DECLICKED ===
print("\n[F] 16ch bed downmix DECLICKED")
bed16d_L = r16[:,0] + r16[:,2]*0.707 + bl_fixed*0.707 + r16[:,6]*0.707
bed16d_R = r16[:,1] + r16[:,2]*0.707 + br_fixed*0.707 + r16[:,7]*0.707
mx = max(np.max(np.abs(bed16d_L)), np.max(np.abs(bed16d_R)), 0.001)
write_wav('F_16ch_bed_declicked.wav', np.stack([bed16d_L/mx*0.9, bed16d_R/mx*0.9], axis=1), SR, 2)

# === Stage G: Full mix (bed declicked + objects) ===
print("\n[G] Full mix: declicked bed + objects")
full_L = bed16d_L + np.sum(r16[:,8:], axis=1)
full_R = bed16d_R + np.sum(r16[:,8:], axis=1)
mx = max(np.max(np.abs(full_L)), np.max(np.abs(full_R)), 0.001)
write_wav('G_full_mix_declicked.wav', np.stack([full_L/mx*0.9, full_R/mx*0.9], axis=1), SR, 2)

# === Stats ===
print("\n--- Glitch counts ---")
for ch, nm in [(4,'BL raw'), (5,'BR raw')]:
    d = np.abs(np.diff(r16[:,ch]))
    print(f"  {nm}: {np.sum(d>0.3)} jumps > 0.3")

bl_d = np.abs(np.diff(bl_fixed))
br_d = np.abs(np.diff(br_fixed))
print(f"  BL declicked: {np.sum(bl_d>0.3)} jumps > 0.3")
print(f"  BR declicked: {np.sum(br_d>0.3)} jumps > 0.3")

print("\nDone! Listen to files A through G in order.")
