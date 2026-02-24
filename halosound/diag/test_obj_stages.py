"""
Test each stage of Atmos OBJECT EXTRACTION pipeline.
Generates WAV files for each step from raw decode to final mix.
Segment: LOTR Two Towers 2:03-2:10, Atmos track (stream 5)
"""
import numpy as np
import subprocess, os, struct

SR = 48000
D = os.path.dirname(os.path.abspath(__file__))
FFMPEG = 'F:/hrtf/ffmpeg-build/bin/ffmpeg.exe'
MOVIE = 'C:/Users/Alexis/Downloads/movies/The.Lord.of.the.Rings.The.Two.Towers2002.mkv'

def write_wav(filename, data, sr=48000):
    """Write float32 numpy array to WAV (16-bit). Shape: (samples,) or (samples, channels)."""
    if data.ndim == 1:
        data = data.reshape(-1, 1)
    ch = data.shape[1]
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
    """Two-pass declicker."""
    out = signal.copy()
    for thresh in [threshold, threshold * 1.5]:
        prev = out[0]
        i = 1
        while i < len(out):
            if abs(out[i] - prev) > thresh:
                end = i + 1
                while end < len(out) and end < i + lookahead:
                    if abs(out[end] - out[end-1]) < thresh:
                        break
                    end += 1
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

def normalize_stereo(L, R, target=0.9):
    mx = max(np.max(np.abs(L)), np.max(np.abs(R)), 0.001)
    return L / mx * target, R / mx * target

print("=" * 60)
print("Atmos Object Extraction - Stage-by-stage")
print("LOTR Two Towers 2:03-2:10")
print("=" * 60)

# === Extract raw PCM ===
print("\n[0] Extracting PCM...")
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

BED_NAMES = ['FL','FR','FC','LFE','BL','BR','SL','SR']
OBJ_NAMES = [f'Obj{i}' for i in range(8)]

# === Print RMS per channel ===
print("\n--- Channel RMS (16ch extract_objects) ---")
for i in range(16):
    rms = np.sqrt(np.mean(r16[:,i]**2))
    pk = np.max(np.abs(r16[:,i]))
    nm = BED_NAMES[i] if i < 8 else OBJ_NAMES[i-8]
    tag = "BED" if i < 8 else "OBJ"
    active = "ACTIVE" if rms > 0.001 else "silent"
    print(f"  ch{i:2d} {nm:>4s} [{tag}] rms={rms:.6f} peak={pk:.4f} {active}")

# =====================================================
# STAGE 1: Reference - standard 8ch (no extract_objects)
# =====================================================
print("\n[1] REFERENCE: Standard 8ch decode (CLEAN, no extract_objects)")
bed8_L = r8[:,0] + r8[:,2]*0.707 + r8[:,4]*0.707 + r8[:,6]*0.707
bed8_R = r8[:,1] + r8[:,2]*0.707 + r8[:,5]*0.707 + r8[:,7]*0.707
L, R = normalize_stereo(bed8_L, bed8_R)
write_wav('OBJ_01_reference_8ch.wav', np.stack([L, R], axis=1))

# =====================================================
# STAGE 2: Raw 16ch bed (with artifacts)
# =====================================================
print("\n[2] RAW: 16ch bed only (channels 0-7, HAS artifacts in BL/BR)")
bed16_L = r16[:,0] + r16[:,2]*0.707 + r16[:,4]*0.707 + r16[:,6]*0.707
bed16_R = r16[:,1] + r16[:,2]*0.707 + r16[:,5]*0.707 + r16[:,7]*0.707
L, R = normalize_stereo(bed16_L, bed16_R)
write_wav('OBJ_02_raw_bed_16ch.wav', np.stack([L, R], axis=1))

# =====================================================
# STAGE 3: Individual bed channels raw (problematic ones)
# =====================================================
print("\n[3] Individual bed channels (BL/BR with artifacts)")
write_wav('OBJ_03a_BL_raw.wav', r16[:,4])
write_wav('OBJ_03b_BR_raw.wav', r16[:,5])
write_wav('OBJ_03c_BL_clean_8ch.wav', r8[:,4])
write_wav('OBJ_03d_BR_clean_8ch.wav', r8[:,5])

# =====================================================
# STAGE 4: Declicked bed channels
# =====================================================
print("\n[4] DECLICKED bed channels")
bl_fixed = declick(r16[:,4])
br_fixed = declick(r16[:,5])
write_wav('OBJ_04a_BL_declicked.wav', bl_fixed)
write_wav('OBJ_04b_BR_declicked.wav', br_fixed)

# Stats
bl_raw_jumps = np.sum(np.abs(np.diff(r16[:,4])) > 0.3)
br_raw_jumps = np.sum(np.abs(np.diff(r16[:,5])) > 0.3)
bl_fix_jumps = np.sum(np.abs(np.diff(bl_fixed)) > 0.3)
br_fix_jumps = np.sum(np.abs(np.diff(br_fixed)) > 0.3)
print(f"  BL: {bl_raw_jumps} -> {bl_fix_jumps} jumps > 0.3")
print(f"  BR: {br_raw_jumps} -> {br_fix_jumps} jumps > 0.3")

# =====================================================
# STAGE 5: Declicked bed downmix
# =====================================================
print("\n[5] DECLICKED bed downmix (stereo)")
bed16d_L = r16[:,0] + r16[:,2]*0.707 + bl_fixed*0.707 + r16[:,6]*0.707
bed16d_R = r16[:,1] + r16[:,2]*0.707 + br_fixed*0.707 + r16[:,7]*0.707
L, R = normalize_stereo(bed16d_L, bed16d_R)
write_wav('OBJ_05_declicked_bed.wav', np.stack([L, R], axis=1))

# =====================================================
# STAGE 6: Individual object channels (raw from extract_objects)
# =====================================================
print("\n[6] Individual OBJECT channels (ch8-15)")
for i in range(8):
    ch = 8 + i
    rms = np.sqrt(np.mean(r16[:,ch]**2))
    if rms > 0.0001:
        # Amplify quiet objects for audibility
        gain = min(20.0, 0.1 / max(rms, 1e-6))
        sig = r16[:,ch] * gain
        write_wav(f'OBJ_06{chr(97+i)}_obj{i}_ch{ch}_x{gain:.0f}.wav', sig)
        print(f"    Obj{i}: rms={rms:.6f}, amplified {gain:.0f}x")
    else:
        print(f"    Obj{i}: SILENT (rms={rms:.8f})")

# =====================================================
# STAGE 7: All objects summed (mono)
# =====================================================
print("\n[7] All objects summed (mono)")
obj_sum = np.sum(r16[:,8:], axis=1)
obj_rms = np.sqrt(np.mean(obj_sum**2))
print(f"  Objects sum rms={obj_rms:.6f}")
# Amplified version
gain = min(20.0, 0.1 / max(obj_rms, 1e-6))
write_wav('OBJ_07a_objects_sum_raw.wav', obj_sum)
write_wav(f'OBJ_07b_objects_sum_x{gain:.0f}.wav', obj_sum * gain)

# =====================================================
# STAGE 8: Bed + Objects combined (no declick)
# =====================================================
print("\n[8] RAW bed + objects combined (NO declick)")
full_raw_L = bed16_L + obj_sum
full_raw_R = bed16_R + obj_sum
L, R = normalize_stereo(full_raw_L, full_raw_R)
write_wav('OBJ_08_raw_bed_plus_objects.wav', np.stack([L, R], axis=1))

# =====================================================
# STAGE 9: Declicked bed + Objects combined
# =====================================================
print("\n[9] DECLICKED bed + objects combined")
full_fix_L = bed16d_L + obj_sum
full_fix_R = bed16d_R + obj_sum
L, R = normalize_stereo(full_fix_L, full_fix_R)
write_wav('OBJ_09_declicked_bed_plus_objects.wav', np.stack([L, R], axis=1))

# =====================================================
# STAGE 10: Objects amplified + clean bed
# =====================================================
print("\n[10] Declicked bed + objects AMPLIFIED (10x)")
full_amp_L = bed16d_L + obj_sum * 10
full_amp_R = bed16d_R + obj_sum * 10
L, R = normalize_stereo(full_amp_L, full_amp_R)
write_wav('OBJ_10_bed_plus_objects_amp10x.wav', np.stack([L, R], axis=1))

# =====================================================
# Summary
# =====================================================
print("\n" + "=" * 60)
print("SUMMARY")
print("=" * 60)
print("""
Listen in order:
  OBJ_01 = Reference 8ch (should sound CLEAN)
  OBJ_02 = Raw 16ch bed (HAS artifacts in BL/BR)
  OBJ_03 = Individual BL/BR channels (raw vs clean)
  OBJ_04 = Declicked BL/BR (should match clean)
  OBJ_05 = Declicked bed stereo mix
  OBJ_06 = Individual object channels (amplified)
  OBJ_07 = All objects summed (raw + amplified)
  OBJ_08 = Raw bed + objects (artifacts + objects)
  OBJ_09 = Declicked bed + objects (FINAL MIX)
  OBJ_10 = Declicked bed + objects amplified 10x (hear the objects!)

Compare: OBJ_01 (ref) vs OBJ_05 (declicked bed) vs OBJ_09 (final)
""")
